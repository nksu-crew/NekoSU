package me.nekosu.aqnya.util

import android.content.Context
import android.net.Uri
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

object VendorBootInstaller {

    private const val SCRIPT_NAME = "install-vendor-boot.sh"

    /**
     * ncore 以 native lib 的形式打包在 jniLibs/<abi>/libncore.so。
     * 安装后系统会把它落到 applicationInfo.nativeLibraryDir,
     * 那里的挂载点允许执行 (而 filesDir 所在的 /data 是 noexec)。
     */
    private const val NCORE_LIB_NAME = "libncore.so"

    fun installDir(context: Context): File =
        File(context.filesDir, "nksu-install").apply { mkdirs() }

    /**
     * 返回 nativeLibraryDir 下的 ncore 可执行路径。
     *
     * 注意: 需要 app 侧打开 android:extractNativeLibs="true"
     * (AGP 4.2+ 对应 packaging.jniLibs.useLegacyPackaging = true),
     * 否则 .so 会直接从 APK mmap, 磁盘上不存在可执行文件。
     */
    fun ncorePath(context: Context): File =
        File(context.applicationInfo.nativeLibraryDir, NCORE_LIB_NAME)

    /**
     * 把 assets 根目录下的资源 (脚本 + nksu.ko) 复制到 filesDir。
     * ncore 不在这里处理 —— 它走 nativeLibraryDir。
     */
    fun prepare(context: Context): File {
        val dir = installDir(context)
        val assets = context.assets.list("") ?: emptyArray()
        if (assets.isEmpty()) {
            throw IllegalStateException("assets 为空, 未打包 nksu.ko 等文件")
        }

        assets.forEach { name ->
            val out = File(dir, name)
            out.outputStream().use { os ->
                context.assets.open(name).use { it.copyTo(os) }
            }
            out.setReadable(true, false)
        }

        return File(dir, SCRIPT_NAME)
    }

    fun koCandidates(context: Context): List<String> =
        (context.assets.list("") ?: emptyArray())
            .filter { it.endsWith("_nksu.ko") }
            .sorted()

    fun stageVendorBoot(
        context: Context,
        uri: Uri,
    ): File {
        val dir = installDir(context)
        val out = File(dir, "vendor_boot.img")
        context.contentResolver.openInputStream(uri)?.use { input ->
            out.outputStream().use { input.copyTo(it) }
        } ?: throw IllegalStateException("无法读取所选文件")
        return out
    }

    suspend fun install(
        context: Context,
        vendorBoot: File,
        koName: String?,
        onOutput: (String) -> Unit,
    ): Int =
        withContext(Dispatchers.IO) {
            val script = prepare(context)
            val dir = installDir(context)

            val ncore = ncorePath(context)
            if (!ncore.exists() || !ncore.canExecute()) {
                throw IllegalStateException(
                    "ncore 不可执行: ${ncore.absolutePath}\n" +
                        "请确认:\n" +
                        "  1) jniLibs/<abi>/$NCORE_LIB_NAME 已随 APK 打包;\n" +
                        "  2) AndroidManifest 打开 extractNativeLibs (useLegacyPackaging=true);\n" +
                        "  3) 设备 ABI 与打包的 lib 目录匹配。"
                )
            }

            // prepare() 已把 assets 根目录下的文件都复制到 dir,
            // 这里只做一次存在性校验即可。
            val koFile =
                koName?.let { name ->
                    File(dir, name).also { out ->
                        if (!out.exists()) {
                            throw IllegalStateException("assets/$name 未找到或未被复制")
                        }
                    }
                }

            val cmd = buildList {
                add("sh")
                add(script.absolutePath)
                add(vendorBoot.absolutePath)
                if (koFile != null) add(koFile.absolutePath)
            }

            val process =
                ProcessBuilder(cmd)
                    .directory(dir)
                    .redirectErrorStream(true)
                    .apply {
                        environment()["NKSU_NCORE"] = ncore.absolutePath
                    }
                    .start()

            process.inputStream.bufferedReader().useLines { lines ->
                lines.forEach { onOutput(it) }
            }

            process.waitFor()
        }
}