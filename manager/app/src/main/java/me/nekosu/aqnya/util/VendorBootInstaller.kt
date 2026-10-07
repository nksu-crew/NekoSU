package me.nekosu.aqnya.util

import android.content.Context
import android.net.Uri
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * vendor_boot 安装 (LKM) 的运行时支持:
 * - 把 assets 里的 nksu.ko / init / ncore / install-vendor-boot.sh 释放到 filesDir
 * - 复制用户选中的 vendor_boot.img
 * - 通过 su 执行安装脚本, 并流式回传输出
 */
object VendorBootInstaller {
    const val ASSET_DIR = "nksu"

    private const val SCRIPT_NAME = "install-vendor-boot.sh"
    private const val NCORE_NAME = "ncore"
    private const val INIT_NAME = "init"

    /** 需要释放到设备可执行目录的 asset 列表 */
    private val EXECUTABLES = setOf(SCRIPT_NAME, NCORE_NAME, INIT_NAME)

    fun installDir(context: Context): File = File(context.filesDir, "nksu-install").apply { mkdirs() }

    /**
     * 释放安装所需文件到 [installDir]。
     * @return 脚本文件的绝对路径
     */
    fun prepare(context: Context): File {
        val dir = installDir(context)
        val assets = context.assets.list(ASSET_DIR) ?: emptyArray()
        if (assets.isEmpty()) {
            throw IllegalStateException("assets/$ASSET_DIR 为空, 未打包 nksu.ko 等文件")
        }

        assets.forEach { name ->
            val out = File(dir, name)
            out.outputStream().use { os ->
                context.assets.open("$ASSET_DIR/$name").use { it.copyTo(os) }
            }
            out.setReadable(true, false)
            if (name in EXECUTABLES) {
                out.setExecutable(true, false)
            }
        }

        return File(dir, SCRIPT_NAME)
    }

    /** assets 中打包的所有 KMI ko 文件名 (androidNN-x.y_nksu.ko), 已排序。 */
    fun koCandidates(context: Context): List<String> =
        (context.assets.list(ASSET_DIR) ?: emptyArray())
            .filter { it.endsWith("_nksu.ko") }
            .sorted()

    /** 把用户选中的 uri 复制到安装目录, 返回目标文件。 */
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

    /**
     * 通过 su 执行安装脚本。
     *
     * @param koName assets 中的 ko 文件名; 会先释放到安装目录后传给脚本。
     * @param onOutput 逐行回调脚本输出。
     * @return 脚本退出码。
     */
    suspend fun install(
        context: Context,
        vendorBoot: File,
        koName: String?,
        onOutput: (String) -> Unit,
    ): Int =
        withContext(Dispatchers.IO) {
            val script = prepare(context)

            val koFile =
                koName?.let { name ->
                    File(installDir(context), name).also { out ->
                        context.assets.open("$ASSET_DIR/$name").use { input ->
                            out.outputStream().use { input.copyTo(it) }
                        }
                        out.setReadable(true, false)
                    }
                }

            val cmd = buildList {
                add(script.absolutePath)
                add(vendorBoot.absolutePath)
                if (koFile != null) add(koFile.absolutePath)
            }

            val process =
                ProcessBuilder(listOf("su", "-c", cmd.joinToString(" ")))
                    .redirectErrorStream(true)
                    .start()

            process.inputStream.bufferedReader().useLines { lines ->
                lines.forEach { onOutput(it) }
            }

            process.waitFor()
        }
}
