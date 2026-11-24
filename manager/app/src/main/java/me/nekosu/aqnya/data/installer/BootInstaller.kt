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
 * to the original init; no partition access or root is required.
 */
object BootInstaller {
    /** Bundled first-stage init wrapper; CMake packages it as libinit.so. */
    private const val INIT_LIB_NAME = "libinit.so"

    /** Staged copy of the user-selected boot/init_boot image. */
    private const val SOURCE_IMAGE_NAME = "boot.target.img"

    /** Install output name: nekosu_<random>_boot.img. */
    private const val OUTPUT_SUFFIX = "_boot.img"

    fun initPath(context: Context): File =
        File(context.applicationInfo.nativeLibraryDir, INIT_LIB_NAME)

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
}
