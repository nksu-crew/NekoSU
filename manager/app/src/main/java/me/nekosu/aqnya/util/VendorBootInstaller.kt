package me.nekosu.aqnya.util

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import androidx.annotation.RequiresApi
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import kotlin.random.Random

object VendorBootInstaller {

    private const val SCRIPT_NAME = "vendor-boot.sh"

    private const val NCORE_LIB_NAME = "libncore.so"

    /** 安装输出文件名: nekosu_<随机串>_vendor_boot.img。 */
    private const val OUTPUT_PREFIX = "nekosu_"
    private const val OUTPUT_SUFFIX = "_vendor_boot.img"
    private const val RANDOM_TOKEN_LENGTH = 8
    private const val RANDOM_ALPHABET = "abcdefghijklmnopqrstuvwxyz0123456789"

    private fun outputImageName(): String {
        val token =
            (1..RANDOM_TOKEN_LENGTH)
                .map { RANDOM_ALPHABET[Random.nextInt(RANDOM_ALPHABET.length)] }
                .joinToString("")
        return "$OUTPUT_PREFIX${token}$OUTPUT_SUFFIX"
    }

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

            // 每次安装生成随机文件名: nekosu_<随机串>_vendor_boot.img
            val produced = File(dir, outputImageName())
            produced.delete()

            val process =
                ProcessBuilder(cmd)
                    .directory(dir)
                    .redirectErrorStream(true)
                    .apply {
                        environment()["NKSU_NCORE"] = ncore.absolutePath
                        environment()["NKSU_OUT"] = produced.absolutePath
                    }
                    .start()

            process.inputStream.bufferedReader().useLines { lines ->
                lines.forEach { onOutput(it) }
            }

            val exit = process.waitFor()
            if (exit == 0) {
                if (produced.exists()) {
                    runCatching { exportToDownload(context, produced) }
                        .onSuccess { onOutput("[nksu] 已导出到: ${it.absolutePath}") }
                        .onFailure { onOutput("[nksu] 导出到 Download 失败: ${it.message}") }
                } else {
                    onOutput("[nksu] 未找到补丁镜像: ${produced.absolutePath}")
                }
            }
            exit
        }

    /**
     * 把生成的补丁镜像导出到 /sdcard/Download。
     *
     * - Android 10 以下、或已授予「所有文件访问」时直接写入公共目录,
     *   导出后保持生成的名称 (Download/nekosu_<随机串>_vendor_boot.img)。
     * - 否则走 MediaStore.Downloads, 无需存储权限即可发布文件。
     *
     * 返回导出后的目标文件。
     */
    fun exportToDownload(
        context: Context,
        source: File,
    ): File {
        val downloads =
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)

        // Android 10+ 只有拿到「所有文件访问」才能直接写公共目录, 否则走 MediaStore。
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val directWrite =
                Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && Environment.isExternalStorageManager()
            if (!directWrite) {
                return exportViaMediaStore(context, source, downloads)
            }
        }

        if (!downloads.exists() && !downloads.mkdirs()) {
            throw IllegalStateException("无法创建目录 ${downloads.absolutePath}")
        }
        val dest = File(downloads, source.name)
        source.copyTo(dest, overwrite = true)
        return dest
    }

    @RequiresApi(Build.VERSION_CODES.Q)
    private fun exportViaMediaStore(
        context: Context,
        source: File,
        downloads: File,
    ): File {
        val resolver = context.contentResolver
        val collection = MediaStore.Downloads.EXTERNAL_CONTENT_URI
        // 删除同名旧条目, 保证导出路径/文件名稳定 (失败不致命)。
        runCatching {
            resolver.delete(
                collection,
                "${MediaStore.MediaColumns.DISPLAY_NAME}=?",
                arrayOf(source.name),
            )
        }

        val values =
            ContentValues().apply {
                put(MediaStore.MediaColumns.DISPLAY_NAME, source.name)
                put(MediaStore.MediaColumns.MIME_TYPE, "application/octet-stream")
                put(MediaStore.MediaColumns.IS_PENDING, 1)
            }
        val uri =
            resolver.insert(collection, values)
                ?: throw IllegalStateException("MediaStore 插入失败")
        try {
            resolver.openOutputStream(uri)?.use { out ->
                source.inputStream().use { it.copyTo(out) }
            } ?: throw IllegalStateException("无法打开输出流")
        } catch (e: Exception) {
            resolver.delete(uri, null, null)
            throw e
        }
        values.clear()
        values.put(MediaStore.MediaColumns.IS_PENDING, 0)
        resolver.update(uri, values, null, null)
        return File(downloads, source.name)
    }
}