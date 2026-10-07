package me.nekosu.aqnya.util

import android.content.Context
import android.net.Uri
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

object VendorBootInstaller {
    const val ASSET_DIR = "nksu"

    private const val SCRIPT_NAME = "install-vendor-boot.sh"
    private const val NCORE_NAME = "ncore"
    private const val INIT_NAME = "init"

    private val EXECUTABLES = setOf(SCRIPT_NAME, NCORE_NAME, INIT_NAME)

    fun installDir(context: Context): File =
        File(context.filesDir, "nksu-install").apply { mkdirs() }

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

    fun koCandidates(context: Context): List<String> =
        (context.assets.list(ASSET_DIR) ?: emptyArray())
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

            val koFile =
                koName?.let { name ->
                    File(dir, name).also { out ->
                        context.assets.open("$ASSET_DIR/$name").use { input ->
                            out.outputStream().use { input.copyTo(it) }
                        }
                        out.setReadable(true, false)
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
                    .start()

            process.inputStream.bufferedReader().useLines { lines ->
                lines.forEach { onOutput(it) }
            }

            process.waitFor()
        }
}