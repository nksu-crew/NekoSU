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

    private fun outputImageName(): String = "$OUTPUT_PREFIX${randomToken()}$OUTPUT_SUFFIX"

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
     * 定位本机 vendor_boot 分区。
     *
     * 依次尝试标准的 by-name 链接，并优先当前 slot 的 `vendor_boot<slot>`。
     * 返回可直接 `dd` 的路径；找不到（如非 GKI 设备）返回 null。
     */
    fun detectVendorBootPartition(): String? {
        val cmd =
            """
            SLOT=${'$'}(getprop ro.boot.slot_suffix 2>/dev/null)
            for base in /dev/block/by-name /dev/block/bootdevice/by-name; do
              for name in vendor_boot vendor_boot${'$'}SLOT; do
                if [ -e "${'$'}base/${'$'}name" ]; then
                  echo "${'$'}base/${'$'}name"
                  exit 0
                fi
              done
            done
            exit 1
            """.trimIndent()

        val r = RootShell.exec(cmd)
        return r.output
            .lineSequence()
            .map { it.trim() }
            .firstOrNull { it.startsWith("/dev/") }
    }

    /**
     * 直接安装：以 root 读取本机 vendor_boot，打入 nksu.ko 后写回分区，
     * 不再需要用户手动 fastboot。
     *
     * 流程：检测分区 -> dump 原镜像并导出备份 -> patch -> 导出补丁镜像 ->
     * 写回分区 -> 读回校验（失败则自动回滚原镜像）。
     *
     * 需要设备已具备 root（Magisk/KernelSU 等 su，或已装 nksu）。
     */
    suspend fun directInstall(
        context: Context,
        koName: String,
        onOutput: (String) -> Unit,
    ): Int =
        withContext(Dispatchers.IO) {
            val script = prepare(context, koName)
            val dir = installDir(context)
            val ncore = ncorePath(context)

            if (!ncore.exists() || !ncore.canExecute()) {
                onOutput("[nksu] ERROR: ncore 不可执行: ${ncore.absolutePath}")
                return@withContext -1
            }
            val koFile = File(dir, koName)
            if (!koFile.exists()) {
                onOutput("[nksu] ERROR: assets/$koName 未找到或复制失败")
                return@withContext -1
            }
            if (!RootShell.available()) {
                onOutput("[nksu] ERROR: 未检测到 root（需要 Magisk/KernelSU 等 su，或已安装 nksu）")
                return@withContext -1
            }

            val part = detectVendorBootPartition()
            if (part == null) {
                onOutput("[nksu] ERROR: 未找到 vendor_boot 分区")
                return@withContext -1
            }
            onOutput("[nksu] vendor_boot: $part")

            // 1) dump 当前分区作为源镜像，同时作为回滚备份。
            val dump = File(dir, backupImageName())
            dump.delete()
            if (RootShell.execStreaming("dd if=${quote(part)} of=${quote(dump.absolutePath)} bs=4096", onOutput) != 0 ||
                !dump.exists()
            ) {
                onOutput("[nksu] ERROR: 读取 vendor_boot 失败")
                return@withContext -1
            }
            runCatching { exportToDownload(context, dump) }
                .onSuccess { onOutput("[nksu] 原镜像备份已导出: ${it.absolutePath}") }
                .onFailure { onOutput("[nksu] 备份导出失败: ${it.message}") }

            // 2) 打入 nksu.ko（复用 vendor-boot.sh）。
            val produced = File(dir, outputImageName())
            produced.delete()
            val patchCmd =
                buildString {
                    append("NKSU_NCORE=${quote(ncore.absolutePath)} ")
                    append("NKSU_OUT=${quote(produced.absolutePath)} ")
                    append("NKSU_WORK=${quote(File(dir, "work").absolutePath)} ")
                    append("sh ${quote(script.absolutePath)} ${quote(dump.absolutePath)} ${quote(koFile.absolutePath)}")
                }
            if (RootShell.execStreaming(patchCmd, onOutput) != 0 || !produced.exists()) {
                onOutput("[nksu] ERROR: 补丁失败")
                return@withContext -1
            }
            runCatching { exportToDownload(context, produced) }
                .onSuccess { onOutput("[nksu] 补丁镜像已导出: ${it.absolutePath}") }
                .onFailure { onOutput("[nksu] 补丁镜像导出失败: ${it.message}") }

            // 3) 写回分区并读回校验；不一致则回滚原镜像。
            val size = produced.length()
            val blocks = (size + 4095) / 4096

            // 安全检查：补丁镜像不能超过分区容量。
            val sizeCmd = "cat /sys/class/block/${'$'}(basename ${'$'}(readlink -f ${quote(part)}))/size"
            val partSectors = RootShell.exec(sizeCmd).output.trim().toLongOrNull()
            if (partSectors != null && size > partSectors * 512L) {
                onOutput("[nksu] ERROR: 补丁镜像 ($size 字节) 超过 vendor_boot 分区容量 (${partSectors * 512L} 字节)")
                return@withContext -1
            }

            val readback = File(dir, "vendor_boot.readback.img")
            val trimmed = File(dir, "vendor_boot.readback.trim")
            val flashCmd =
                buildString {
                    append("dd if=${quote(produced.absolutePath)} of=${quote(part)} bs=4096 || exit 1\n")
                    append("sync\n")
                    append("dd if=${quote(part)} of=${quote(readback.absolutePath)} bs=4096 count=$blocks || exit 1\n")
                    append("head -c $size ${quote(readback.absolutePath)} > ${quote(trimmed.absolutePath)} || exit 1\n")
                    append("if cmp ${quote(produced.absolutePath)} ${quote(trimmed.absolutePath)} >/dev/null 2>&1; then\n")
                    append("  echo '[nksu] 写入校验通过'\n")
                    append("else\n")
                    append("  echo '[nksu] 写入校验失败，正在回滚原镜像'\n")
                    append("  dd if=${quote(dump.absolutePath)} of=${quote(part)} bs=4096\n")
                    append("  sync\n")
                    append("  exit 1\n")
                    append("fi")
                }
            val flashExit = RootShell.execStreaming(flashCmd, onOutput)
            readback.delete()
            trimmed.delete()
            if (flashExit != 0) {
                onOutput("[nksu] ERROR: 刷入 vendor_boot 失败")
                return@withContext -1
            }

            onOutput("[nksu] 直接安装完成，重启后生效")
            0
        }

    /** 备份镜像文件名: nekosu_<随机串>_vendor_boot_orig.img。 */
    private fun backupImageName(): String = "$OUTPUT_PREFIX${randomToken()}_vendor_boot_orig.img"

    private fun randomToken(): String =
        (1..RANDOM_TOKEN_LENGTH)
            .map { RANDOM_ALPHABET[Random.nextInt(RANDOM_ALPHABET.length)] }
            .joinToString("")

    private fun quote(s: String): String = "'" + s.replace("'", "'\\''") + "'"

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