// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.data.installer

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Environment
import android.provider.MediaStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import me.nekosu.aqnya.R
import me.nekosu.aqnya.core.Ncore
import java.io.File
import kotlin.random.Random

object VendorBootInstaller {
    /** 内核模块在 assets 中的存放目录，例如 assets/ko/android14-6.1_nksu.ko。 */
    private const val KO_ASSET_DIR = "ko"

    private const val NCORE_LIB_NAME = "libncore.so"

    /** 本地 LKM 在安装目录中暂存的文件名。 */
    private const val LOCAL_KO_NAME = "local_nksu.ko"

    /** 安装输出文件名: nekosu_<随机串>_vendor_boot.img。 */
    private const val OUTPUT_PREFIX = "nekosu_"
    private const val OUTPUT_SUFFIX = "_vendor_boot.img"

    /** 本次安装从分区 dump 的临时原镜像；仅用于 patch 与回滚，安装后删除。 */
    private const val SOURCE_IMAGE_NAME = "vendor_boot.source.img"
    private const val BOOT_SOURCE_IMAGE_NAME = "boot.source.img"
    private const val RANDOM_TOKEN_LENGTH = 8
    private const val RANDOM_ALPHABET = "abcdefghijklmnopqrstuvwxyz0123456789"

    private fun outputImageName(): String = newOutputImageName(OUTPUT_SUFFIX)

    /** Random, install-specific export name; [suffix] includes the leading dot. */
    internal fun newOutputImageName(suffix: String): String = "$OUTPUT_PREFIX${randomToken()}$suffix"

    fun installDir(context: Context): File = File(context.filesDir, "nksu-install").apply { mkdirs() }

    /** ncore, the module runtime / su daemon bundled by the manager. */
    fun ncorePath(context: Context): File = File(context.applicationInfo.nativeLibraryDir, NCORE_LIB_NAME)

    /** Copy a user-selected image into the install directory. */
    internal fun stageImage(
        context: Context,
        uri: Uri,
        name: String,
    ): File {
        val dir = installDir(context)
        val out = File(dir, name)
        context.contentResolver.openInputStream(uri)?.use { input ->
            out.outputStream().use { input.copyTo(it) }
        } ?: throw IllegalStateException(context.getString(R.string.install_err_read_file))
        return out
    }

    /**
     * 把内核模块放到安装目录并返回其文件：
     *  - 传了 [koUri] 时复制用户选择的本地 .ko；
     *  - 否则从 assets/ko 复制打包的 [koName]。
     */
    internal fun resolveKo(
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

    private fun copyAsset(
        context: Context,
        assetName: String,
        dest: File,
    ) {
        try {
            // Asset names may contain a directory prefix (e.g. "ko/...");
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
    ): File = stageImage(context, uri, "vendor_boot.img")

    /** Copy the user-selected boot image used for kernel symbol verification. */
    fun stageBoot(
        context: Context,
        uri: Uri,
    ): File = stageImage(context, uri, "boot.img")

    suspend fun install(
        context: Context,
        vendorBoot: File,
        bootImage: File?,
        koName: String?,
        koUri: Uri? = null,
        verifySymbols: Boolean = true,
        onOutput: (String) -> Unit,
    ): Int =
        withContext(Dispatchers.IO) {
            val dir = installDir(context)
            if (verifySymbols && (bootImage == null || !bootImage.isFile)) {
                throw IllegalStateException(context.getString(R.string.install_err_boot_missing))
            }

            val koFile =
                resolveKo(context, koName, koUri)
                    ?: throw IllegalStateException(context.getString(R.string.install_err_ko_missing))

            try {
                onOutput(context.getString(R.string.install_log_patching))
                val patched = patchImage(vendorBoot, bootImage, koFile, verifySymbols, onOutput)
                if (patched == null) {
                    onOutput(context.getString(R.string.install_log_err_patch))
                    return@withContext -1
                }

                // Rename the patched image to the export name rather than writing a
                // second full copy of it.
                val produced = File(dir, outputImageName())
                produced.delete()
                if (!patched.renameTo(produced)) {
                    patched.copyTo(produced, overwrite = true)
                    patched.delete()
                }
                runCatching { exportToDownload(context, produced) }
                    .onSuccess {
                        onOutput(context.getString(R.string.install_log_exported, it.absolutePath))
                        // The Download copy is the deliverable; drop the private one.
                        produced.delete()
                    }.onFailure { onOutput(context.getString(R.string.install_log_export_failed, it.message.orEmpty())) }
                0
            } finally {
                // The staged input images are only needed for this run.
                vendorBoot.delete()
                bootImage?.delete()
            }
        }

    /**
     * 在 libjni 内直接把 [koFile] 打进 [sourceImage]（v3/v4 vendor_boot）：
     * 解包、把模块安装到 `lib/modules/nksu.ko`、登记 `modules.load` /
     * `modules.dep`，保持各 ramdisk 的压缩与 v4 ramdisk 表，最后写出
     * `<image>.new`。启用 [verifySymbols] 时还会用 [bootImage] 校验模块所需的
     * 内核符号。成功返回该文件，失败返回 null。
     *
     * 补丁在 manager 进程内完成，只操作本地文件，无需 root 或外部可执行文件。
     */
    private fun patchImage(
        sourceImage: File,
        bootImage: File?,
        koFile: File,
        verifySymbols: Boolean,
        onOutput: (String) -> Unit,
    ): File? {
        val dir = sourceImage.parentFile ?: return null
        val output = File(dir, "${sourceImage.name}.new")
        output.delete()

        val error =
            runCatching {
                Ncore.patchVendorBoot(
                    imagePath = sourceImage.absolutePath,
                    modulePath = koFile.absolutePath,
                    bootPath = bootImage?.takeIf { verifySymbols }?.absolutePath,
                    verifySymbols = verifySymbols,
                    outputPath = output.absolutePath,
                )
            }.getOrElse {
                onOutput(it.message.orEmpty())
                output.delete()
                return null
            }
        if (error != null) {
            onOutput(error)
            output.delete()
            return null
        }
        if (!output.isFile) {
            return null
        }
        return output
    }

    fun detectVendorBootPartition(): String? =
        PartitionAccess.findPartition(
            "vendor_boot",
            PartitionAccess.currentSlotSuffix(),
            allowSlotless = true,
        )

    fun detectInactiveVendorBootPartition(): String? {
        val inactive = PartitionAccess.inactiveSlotSuffix() ?: return null
        return PartitionAccess.findPartition("vendor_boot", inactive, allowSlotless = false)
    }

    /** boot 分区（kernel + ramdisk），用于在 patch 前校验内核符号。 */
    fun detectBootPartition(): String? =
        PartitionAccess.findPartition(
            "boot",
            PartitionAccess.currentSlotSuffix(),
            allowSlotless = true,
        )

    fun detectInactiveBootPartition(): String? {
        val inactive = PartitionAccess.inactiveSlotSuffix() ?: return null
        return PartitionAccess.findPartition("boot", inactive, allowSlotless = false)
    }

    /** 是否 A/B 设备（有 slot 后缀）。 */
    fun isAbDevice(): Boolean = PartitionAccess.isAbDevice()

    /** nksu 内核模块或外部 su 任一可用即视为具备 root。 */
    fun hasRoot(): Boolean = PartitionAccess.hasRoot()

    /**
     * 直接安装：以 root 读取指定 vendor_boot 分区，打入 nksu.ko 后写回。
     *
     * 流程：dump 原镜像（开启符号校验时再 dump 同 slot 的 boot 分区） ->
     * 在 libjni 内打补丁 -> 写回分区；写入失败时把原镜像写回回滚。
     * KernelSU 的「直接安装 / 刷入非活动分区」都走这里，区别只是
     * @partition 指向当前或非活动 slot。
     *
     * patch 全程只操作本地临时文件，不需要 root；只有 dump / 写回分区用到 root，
     * 且优先 nksu 内核模块、必要时回退到 `/system/bin/su`。原镜像只现 dump 一份、
     * 供 patch 与失败回滚使用，结束即删除；不导出补丁镜像，也不做整分区读回校验
     * （写入结果即依据）。
     *
     * 需要设备已具备 root：`/system/bin/su`（Magisk/KernelSU 等）或已装 nksu。
     */
    suspend fun directInstall(
        context: Context,
        koName: String?,
        koUri: Uri? = null,
        partition: String,
        bootPartition: String?,
        verifySymbols: Boolean = true,
        onOutput: (String) -> Unit,
    ): Int =
        withContext(Dispatchers.IO) {
            val dir = installDir(context)

            val koFile = resolveKo(context, koName, koUri)
            if (koFile == null || !koFile.exists()) {
                onOutput(context.getString(R.string.install_log_err_ko, koName ?: koUri?.toString().orEmpty()))
                return@withContext -1
            }
            if (partition.isBlank()) {
                onOutput(context.getString(R.string.install_log_err_no_part))
                return@withContext -1
            }
            if (verifySymbols && bootPartition.isNullOrBlank()) {
                onOutput(context.getString(R.string.install_boot_partition_missing))
                return@withContext -1
            }
            // root 可以来自 nksu 内核模块（已装），也可以来自 /system/bin/su
            // （仅 Magisk/KernelSU 等）。两者都没有时才报告无 root。
            if (!PartitionAccess.hasRoot()) {
                onOutput(context.getString(R.string.install_log_err_no_root))
                return@withContext -1
            }

            val part = partition
            onOutput(context.getString(R.string.install_log_partition, part))

            // 1) 现 dump 一份原镜像，既作为 patch 的输入，也作为写入失败的回滚依据。
            val source = File(dir, SOURCE_IMAGE_NAME)
            source.delete()
            source.createNewFile()
            if (!PartitionAccess.copyBlock(part, source.absolutePath) || !source.exists()) {
                onOutput(context.getString(R.string.install_log_err_dump))
                source.delete()
                return@withContext -1
            }

            // 符号检查需要同一 slot 的内核，dump boot 分区供 libjni 解析 kallsyms。
            val bootSource = File(dir, BOOT_SOURCE_IMAGE_NAME)
            if (verifySymbols) {
                bootSource.delete()
                bootSource.createNewFile()
                onOutput(context.getString(R.string.install_log_boot_partition, bootPartition.orEmpty()))
                val bootDumped =
                    PartitionAccess.copyBlock(bootPartition.orEmpty(), bootSource.absolutePath) &&
                        bootSource.exists()
                if (!bootDumped) {
                    onOutput(context.getString(R.string.install_log_err_boot_dump))
                    source.delete()
                    bootSource.delete()
                    return@withContext -1
                }
            }

            // 2) 在 libjni 内直接把模块打进镜像（无需 root），得到补丁镜像。
            onOutput(context.getString(R.string.install_log_patching))
            val patched =
                patchImage(
                    source,
                    bootSource.takeIf { verifySymbols },
                    koFile,
                    verifySymbols,
                    onOutput,
                )
            if (patched == null) {
                onOutput(context.getString(R.string.install_log_err_patch))
                source.delete()
                bootSource.delete()
                return@withContext -1
            }

            // 3) 写回分区；失败则把原镜像写回，避免留下半写状态。
            val capacity = PartitionAccess.partitionSize(part)
            if (capacity != null && patched.length() > capacity) {
                onOutput(context.getString(R.string.install_log_err_too_large, patched.length(), capacity))
                source.delete()
                bootSource.delete()
                patched.delete()
                return@withContext -1
            }
            onOutput(context.getString(R.string.install_log_flashing))
            if (!PartitionAccess.copyBlock(patched.absolutePath, part)) {
                onOutput(context.getString(R.string.install_log_rollback))
                PartitionAccess.copyBlock(source.absolutePath, part)
                source.delete()
                bootSource.delete()
                patched.delete()
                onOutput(context.getString(R.string.install_log_err_flash))
                return@withContext -1
            }

            source.delete()
            bootSource.delete()
            patched.delete()
            onOutput(context.getString(R.string.install_log_direct_done))
            0
        }

    private fun randomToken(): String =
        (1..RANDOM_TOKEN_LENGTH)
            .map { RANDOM_ALPHABET[Random.nextInt(RANDOM_ALPHABET.length)] }
            .joinToString("")

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
