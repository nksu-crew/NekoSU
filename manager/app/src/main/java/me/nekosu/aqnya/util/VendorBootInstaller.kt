package me.nekosu.aqnya.util

import android.content.Context
import android.net.Uri
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

object VendorBootInstaller {

    private const val SCRIPT_NAME = "install-vendor-boot.sh"

    private const val NCORE_LIB_NAME = "libncore.so"

    fun installDir(context: Context): File =
        File(context.filesDir, "nksu-install").apply { mkdirs() }

    fun ncorePath(context: Context): File =
        File(context.applicationInfo.nativeLibraryDir, NCORE_LIB_NAME)

    fun prepare(context: Context, koName: String?): File {
        val dir = installDir(context)
        val script = File(dir, SCRIPT_NAME)

        copyAsset(context, SCRIPT_NAME, script)
        script.setReadable(true, false)
        script.setExecutable(true, false)

        if (koName != null) {
            val koFile = File(dir, koName)
            copyAsset(context, koName, koFile)
            koFile.setReadable(true, false)
        }

        return script
    }

    private fun copyAsset(context: Context, assetName: String, dest: File) {
        try {
            context.assets.open(assetName).use { input ->
                dest.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
        } catch (e: Exception) {
            throw IllegalStateException("复制 assets/$assetName 失败: ${e.message}", e)
        }
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
            val script = prepare(context, koName)
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

            val koFile = koName?.let { name ->
                File(dir, name).also { out ->
                    if (!out.exists()) {
                        throw IllegalStateException("assets/$name 未找到或复制失败")
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