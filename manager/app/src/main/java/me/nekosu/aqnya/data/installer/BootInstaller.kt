// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.data.installer

import android.content.Context
import android.net.Uri
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import me.nekosu.aqnya.R
import me.nekosu.aqnya.core.Ncore
import java.io.File

/**
 * Patch a boot/init_boot image so that the first-stage init loads [nksu.ko].
 *
 * This is the non-GKI counterpart of [VendorBootInstaller]: the image's ramdisk
 * gets the bundled init wrapper as `/init`, the original init as `/init.real`
 * and the LKM as `/nksu.ko`. The wrapper loads the module and then hands over
 * to the original init; no partition access or root is required for the image
 * mode. [directInstall] reuses the same patch to write the current (or inactive)
 * init_boot partition in place.
 */
object BootInstaller {
    /** Bundled first-stage init wrapper; CMake packages it as libinit.so. */
    private const val INIT_LIB_NAME = "libinit.so"

    /** Staged copy of the user-selected boot/init_boot image. */
    private const val SOURCE_IMAGE_NAME = "boot.target.img"

    /** Temporary dump of the partition while direct-installing. */
    private const val DIRECT_SOURCE_IMAGE_NAME = "boot.direct.img"

    /** Install output name: nekosu_<random>_boot.img. */
    private const val OUTPUT_SUFFIX = "_boot.img"

    fun initPath(context: Context): File = File(context.applicationInfo.nativeLibraryDir, INIT_LIB_NAME)

    /** Copy the selected boot/init_boot image into the install directory. */
    fun stageImage(
        context: Context,
        uri: Uri,
    ): File = VendorBootInstaller.stageImage(context, uri, SOURCE_IMAGE_NAME)

    /**
     * Patch [image] in-process with libjni, then export the result to Download.
     *
     * Returns 0 on success, -1 when no patched image was produced. [image] is
     * always removed afterwards; the patched image is the only deliverable.
     */
    suspend fun install(
        context: Context,
        image: File,
        koName: String?,
        koUri: Uri?,
        onOutput: (String) -> Unit,
    ): Int =
        withContext(Dispatchers.IO) {
            try {
                val init = initPath(context)
                if (!init.isFile) {
                    onOutput(context.getString(R.string.install_log_err_init_missing, init.absolutePath))
                    return@withContext -1
                }

                val koFile = VendorBootInstaller.resolveKo(context, koName, koUri)
                if (koFile == null || !koFile.isFile) {
                    onOutput(context.getString(R.string.install_log_err_ko, koName ?: koUri?.toString().orEmpty()))
                    return@withContext -1
                }

                onOutput(context.getString(R.string.install_log_boot_patching))
                val output =
                    File(
                        VendorBootInstaller.installDir(context),
                        VendorBootInstaller.newOutputImageName(OUTPUT_SUFFIX),
                    )
                output.delete()

                val error =
                    runCatching {
                        Ncore.patchBoot(
                            imagePath = image.absolutePath,
                            modulePath = koFile.absolutePath,
                            initPath = init.absolutePath,
                            outputPath = output.absolutePath,
                        )
                    }.getOrElse {
                        onOutput(it.message.orEmpty())
                        output.delete()
                        return@withContext -1
                    }
                if (error != null) {
                    onOutput(error)
                    output.delete()
                    return@withContext -1
                }
                if (!output.isFile) {
                    onOutput(context.getString(R.string.install_log_err_patch))
                    return@withContext -1
                }

                runCatching { VendorBootInstaller.exportToDownload(context, output) }
                    .onSuccess {
                        onOutput(context.getString(R.string.install_log_exported, it.absolutePath))
                        output.delete()
                    }.onFailure {
                        onOutput(context.getString(R.string.install_log_export_failed, it.message.orEmpty()))
                    }
                0
            } finally {
                image.delete()
            }
        }

    /**
     * init_boot/boot 分区查找：优先 init_boot（Android 13+），没有时回退到
     * boot（Android 12 及更早的 ramdisk 在 boot 里）。
     */
    fun detectInitBootPartition(): String? {
        val slot = PartitionAccess.currentSlotSuffix()
        return PartitionAccess.findPartition("init_boot", slot, allowSlotless = true)
            ?: PartitionAccess.findPartition("boot", slot, allowSlotless = true)
    }

    fun detectInactiveInitBootPartition(): String? {
        val inactive = PartitionAccess.inactiveSlotSuffix() ?: return null
        return PartitionAccess.findPartition("init_boot", inactive, allowSlotless = false)
            ?: PartitionAccess.findPartition("boot", inactive, allowSlotless = false)
    }

    /**
     * 直接安装：以 root 读取 init_boot（或回退的 boot）分区，用 init 包装器
     * 打补丁后写回。
     *
     * 流程与 [VendorBootInstaller.directInstall] 一致：dump 原镜像 -> 在 libjni
     * 内打补丁 -> 写回分区；写入失败时把原镜像写回回滚。init 包装器本身不做
     * 内核符号校验，因此不需要额外的 boot 分区。
     *
     * 需要设备已具备 root：`/system/bin/su`（Magisk/KernelSU 等）或已装 nksu。
     */
    suspend fun directInstall(
        context: Context,
        koName: String?,
        koUri: Uri? = null,
        partition: String,
        onOutput: (String) -> Unit,
    ): Int =
        withContext(Dispatchers.IO) {
            val init = initPath(context)
            if (!init.isFile) {
                onOutput(context.getString(R.string.install_log_err_init_missing, init.absolutePath))
                return@withContext -1
            }

            val koFile = VendorBootInstaller.resolveKo(context, koName, koUri)
            if (koFile == null || !koFile.isFile) {
                onOutput(context.getString(R.string.install_log_err_ko, koName ?: koUri?.toString().orEmpty()))
                return@withContext -1
            }
            if (partition.isBlank()) {
                onOutput(context.getString(R.string.install_log_err_no_init_boot_part))
                return@withContext -1
            }
            if (!PartitionAccess.hasRoot()) {
                onOutput(context.getString(R.string.install_log_err_no_root))
                return@withContext -1
            }

            val part = partition
            onOutput(context.getString(R.string.install_log_init_boot_partition, part))

            val dir = VendorBootInstaller.installDir(context)

            // 1) 现 dump 一份原镜像，既作为 patch 的输入，也作为写入失败的回滚依据。
            val source = File(dir, DIRECT_SOURCE_IMAGE_NAME)
            source.delete()
            source.createNewFile()
            if (!PartitionAccess.copyBlock(part, source.absolutePath) || !source.exists()) {
                onOutput(context.getString(R.string.install_log_err_init_boot_dump))
                source.delete()
                return@withContext -1
            }

            // 2) 在 libjni 内把 init 包装器与模块打进镜像（无需 root）。
            onOutput(context.getString(R.string.install_log_init_boot_patching))
            val patched = File(dir, "${source.name}.new")
            patched.delete()
            val error =
                runCatching {
                    Ncore.patchBoot(
                        imagePath = source.absolutePath,
                        modulePath = koFile.absolutePath,
                        initPath = init.absolutePath,
                        outputPath = patched.absolutePath,
                    )
                }.getOrElse {
                    onOutput(it.message.orEmpty())
                    patched.delete()
                    source.delete()
                    return@withContext -1
                }
            if (error != null || !patched.isFile) {
                if (error != null) onOutput(error)
                onOutput(context.getString(R.string.install_log_err_patch))
                patched.delete()
                source.delete()
                return@withContext -1
            }

            // 3) 写回分区；失败则把原镜像写回，避免留下半写状态。
            val capacity = PartitionAccess.partitionSize(part)
            if (capacity != null && patched.length() > capacity) {
                onOutput(context.getString(R.string.install_log_err_too_large_init_boot, patched.length(), capacity))
                patched.delete()
                source.delete()
                return@withContext -1
            }
            onOutput(context.getString(R.string.install_log_flashing_init_boot))
            if (!PartitionAccess.copyBlock(patched.absolutePath, part)) {
                onOutput(context.getString(R.string.install_log_rollback))
                PartitionAccess.copyBlock(source.absolutePath, part)
                patched.delete()
                source.delete()
                onOutput(context.getString(R.string.install_log_err_flash_init_boot))
                return@withContext -1
            }

            patched.delete()
            source.delete()
            onOutput(context.getString(R.string.install_log_init_boot_done))
            0
        }
}
