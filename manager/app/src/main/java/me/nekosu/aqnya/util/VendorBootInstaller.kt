package me.nekosu.aqnya.util

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Environment
import android.provider.MediaStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import kotlin.random.Random
import me.nekosu.aqnya.R

object VendorBootInstaller {

    private const val SCRIPT_NAME = "scripts/vendor-boot.sh"

    /** 内核模块在 assets 中的存放目录，例如 assets/ko/android14-6.1_nksu.ko。 */
    private const val KO_ASSET_DIR = "ko"

    private const val NCORE_LIB_NAME = "libncore.so"

    /** 本地 LKM 在安装目录中暂存的文件名。 */
    private const val LOCAL_KO_NAME = "local_nksu.ko"

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

    fun prepare(context: Context): File {
        val dir = installDir(context)
        val script = File(dir, SCRIPT_NAME)

        copyAsset(context, SCRIPT_NAME, script)
        script.setReadable(true, false)
        script.setExecutable(true, false)

        return script
    }

    /**
     * 把内核模块放到安装目录并返回其文件：
     *  - 传了 [koUri] 时复制用户选择的本地 .ko；
     *  - 否则从 assets/ko 复制打包的 [koName]。
     */
    private fun resolveKo(
        context: Context,
        koName: String?,
        koUri: Uri?,
    ): File? {
        val dir = installDir(context)
        if (koUri != null) {
            val out = File(dir, LOCAL_KO_NAME)
            out.delete()
            context.contentResolver.openInputStream(koUri)?.use { input ->
                out.outputStream().use { input.copyTo(it) }
            } ?: return null
            out.setReadable(true, false)
            return out
        }
        if (koName != null) {
            val out = File(dir, koName)
            copyAsset(context, "$KO_ASSET_DIR/$koName", out)
            out.setReadable(true, false)
            return out
        }
        return null
    }

    private fun copyAsset(context: Context, assetName: String, dest: File) {
        try {
            // Asset names may contain a directory prefix (e.g. "scripts/vendor-boot.sh");
            // FileOutputStream does not create parents, so make them first.
            dest.parentFile?.mkdirs()
            context.assets.open(assetName).use { input ->
                dest.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
        } catch (e: Exception) {
            throw IllegalStateException(
                context.getString(R.string.install_err_copy_asset, assetName, e.message.orEmpty()),
                e,
            )
        }
    }

    fun koCandidates(context: Context): List<String> =
        (context.assets.list(KO_ASSET_DIR) ?: emptyArray())
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
        } ?: throw IllegalStateException(context.getString(R.string.install_err_read_file))
        return out
    }

    suspend fun install(
        context: Context,
        vendorBoot: File,
        koName: String?,
        koUri: Uri? = null,
        onOutput: (String) -> Unit,
    ): Int =
        withContext(Dispatchers.IO) {
            val script = prepare(context)
            val dir = installDir(context)

            val ncore = ncorePath(context)
            if (!ncore.exists() || !ncore.canExecute()) {
                throw IllegalStateException(
                    context.getString(R.string.install_err_ncore, ncore.absolutePath),
                )
            }

            val koFile =
                resolveKo(context, koName, koUri)
                    ?: throw IllegalStateException(context.getString(R.string.install_err_ko_missing))

            val cmd = buildList {
                add("sh")
                add(script.absolutePath)
                add(vendorBoot.absolutePath)
                add(koFile.absolutePath)
            }

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
                        .onSuccess { onOutput(context.getString(R.string.install_log_exported, it.absolutePath)) }
                        .onFailure { onOutput(context.getString(R.string.install_log_export_failed, it.message.orEmpty())) }
                } else {
                    onOutput(context.getString(R.string.install_log_patched_missing, produced.absolutePath))
                }
            }
            exit
        }

    fun detectVendorBootPartition(): String? = findVendorBoot(currentSlotSuffix(), allowSlotless = true)

    fun detectInactiveVendorBootPartition(): String? {
        val inactive = inactiveSlotSuffix() ?: return null
        return findVendorBoot(inactive, allowSlotless = false)
    }

    /** 当前设备 slot 后缀（如 "_a"）；非 A/B 或未知时返回 null。 */
    fun currentSlotSuffix(): String? {
        val cmd =
            """
            slot=${'$'}(getprop ro.boot.slot_suffix 2>/dev/null)
            if [ -z "${'$'}slot" ]; then
              s=${'$'}(getprop ro.boot.slot 2>/dev/null)
              [ -n "${'$'}s" ] && slot="_${'$'}s"
            fi
            [ -n "${'$'}slot" ] && echo "${'$'}slot"
            """.trimIndent()

        return RootShell.exec(cmd).output
            .lineSequence()
            .map { it.trim() }
            .firstOrNull { it.startsWith("_") }
    }

    /** 非活动 slot 后缀；非 A/B 设备返回 null。 */
    fun inactiveSlotSuffix(): String? =
        when (currentSlotSuffix()) {
            "_a" -> "_b"
            "_b" -> "_a"
            else -> null
        }

    /**
     * 查找 vendor_boot 分区路径。
     *
     * @slotSuffix null/空按无后缀处理；@allowSlotless 允许回退到无后缀的
     * `vendor_boot` 链接（只对当前 slot 安全，非活动 slot 不能回退，否则会
     * 误写当前分区）。
     */
    private fun findVendorBoot(
        slotSuffix: String?,
        allowSlotless: Boolean,
    ): String? {
        val slot = slotSuffix.orEmpty()
        val names = if (allowSlotless) "vendor_boot$slot vendor_boot" else "vendor_boot$slot"
        val cmd =
            """
            for base in /dev/block/by-name /dev/block/bootdevice/by-name; do
              for name in $names; do
                if [ -e "${'$'}base/${'$'}name" ]; then
                  echo "${'$'}base/${'$'}name"
                  exit 0
                fi
              done
            done
            exit 1
            """.trimIndent()

        return RootShell.exec(cmd).output
            .lineSequence()
            .map { it.trim() }
            .firstOrNull { it.startsWith("/dev/") }
    }

    /**
     * 直接安装：以 root 读取指定 vendor_boot 分区，打入 nksu.ko 后写回，
     * 不再需要用户手动 fastboot。
     *
     * 流程：dump 原镜像并导出备份 -> patch -> 导出补丁镜像 -> 写回分区 ->
     * 读回校验（失败则自动回滚原镜像）。KernelSU 的「直接安装 / 刷入非活动
     * 分区」都走这里，区别只是 @partition 指向当前或非活动 slot。
     *
     * 需要设备已具备 root（Magisk/KernelSU 等 su，或已装 nksu）。
     */
    suspend fun directInstall(
        context: Context,
        koName: String?,
        koUri: Uri? = null,
        partition: String,
        onOutput: (String) -> Unit,
    ): Int =
        withContext(Dispatchers.IO) {
            val script = prepare(context)
            val dir = installDir(context)
            val ncore = ncorePath(context)

            if (!ncore.exists() || !ncore.canExecute()) {
                onOutput(context.getString(R.string.install_log_err_ncore, ncore.absolutePath))
                return@withContext -1
            }
            val koFile = resolveKo(context, koName, koUri)
            if (koFile == null || !koFile.exists()) {
                onOutput(context.getString(R.string.install_log_err_ko, koName ?: koUri?.toString().orEmpty()))
                return@withContext -1
            }
            if (partition.isBlank()) {
                onOutput(context.getString(R.string.install_log_err_no_part))
                return@withContext -1
            }
            if (!RootShell.available()) {
                onOutput(context.getString(R.string.install_log_err_no_root))
                return@withContext -1
            }

            val part = partition
            onOutput(context.getString(R.string.install_log_partition, part))

            // 已具备 root：顺便把 ncore 放到 init.rc 会 exec 的固定路径，
            // 免去“首次重启后再由管理器补装、需二次重启”的往返。
            if (NcoreBoot.install(context)) {
                onOutput(context.getString(R.string.install_log_ncore_installed, NcoreBoot.BOOT_PATH))
            } else {
                onOutput(context.getString(R.string.install_log_ncore_warn))
            }

            // 1) dump 当前分区作为源镜像，同时作为回滚备份。
            val dump = File(dir, backupImageName())
            dump.delete()
            if (RootShell.execStreaming("dd if=${quote(part)} of=${quote(dump.absolutePath)} bs=4096", onOutput) != 0 ||
                !dump.exists()
            ) {
                onOutput(context.getString(R.string.install_log_err_dump))
                return@withContext -1
            }
            runCatching { exportToDownload(context, dump) }
                .onSuccess { onOutput(context.getString(R.string.install_log_backup_exported, it.absolutePath)) }
                .onFailure { onOutput(context.getString(R.string.install_log_backup_failed, it.message.orEmpty())) }

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
                onOutput(context.getString(R.string.install_log_err_patch))
                return@withContext -1
            }
            runCatching { exportToDownload(context, produced) }
                .onSuccess { onOutput(context.getString(R.string.install_log_patched_exported, it.absolutePath)) }
                .onFailure { onOutput(context.getString(R.string.install_log_patched_export_failed, it.message.orEmpty())) }

            // 3) 写回分区并读回校验；不一致则回滚原镜像。
            val size = produced.length()
            val blocks = (size + 4095) / 4096

            // 安全检查：补丁镜像不能超过分区容量。
            val sizeCmd = "cat /sys/class/block/${'$'}(basename ${'$'}(readlink -f ${quote(part)}))/size"
            val partSectors = RootShell.exec(sizeCmd).output.trim().toLongOrNull()
            if (partSectors != null && size > partSectors * 512L) {
                onOutput(context.getString(R.string.install_log_err_too_large, size, partSectors * 512L))
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
                    append("  echo ${quote(context.getString(R.string.install_log_verify_ok))}\n")
                    append("else\n")
                    append("  echo ${quote(context.getString(R.string.install_log_verify_failed))}\n")
                    append("  dd if=${quote(dump.absolutePath)} of=${quote(part)} bs=4096\n")
                    append("  sync\n")
                    append("  exit 1\n")
                    append("fi")
                }
            val flashExit = RootShell.execStreaming(flashCmd, onOutput)
            readback.delete()
            trimmed.delete()
            if (flashExit != 0) {
                onOutput(context.getString(R.string.install_log_err_flash))
                return@withContext -1
            }

            onOutput(context.getString(R.string.install_log_direct_done))
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
     * - 已授予「所有文件访问」时直接写入公共目录，保持生成的名称
     *   (Download/nekosu_<随机串>_vendor_boot.img)。
     * - 否则走 MediaStore.Downloads，无需存储权限即可发布文件。
     *
     * 返回导出后的目标文件。
     */
    fun exportToDownload(
        context: Context,
        source: File,
    ): File {
        val downloads =
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)

        // 没有「所有文件访问」就写不了公共目录，改走 MediaStore。
        if (!Environment.isExternalStorageManager()) {
            return exportViaMediaStore(context, source, downloads)
        }

        if (!downloads.exists() && !downloads.mkdirs()) {
            throw IllegalStateException(context.getString(R.string.install_err_mkdir, downloads.absolutePath))
        }
        val dest = File(downloads, source.name)
        source.copyTo(dest, overwrite = true)
        return dest
    }

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
                ?: throw IllegalStateException(context.getString(R.string.install_err_mediastore_insert))
        try {
            resolver.openOutputStream(uri)?.use { out ->
                source.inputStream().use { it.copyTo(out) }
            } ?: throw IllegalStateException(context.getString(R.string.install_err_open_stream))
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