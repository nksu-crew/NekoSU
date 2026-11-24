// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.core

import android.util.Log
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.withContext
import java.util.concurrent.ExecutionException
import java.util.concurrent.Executors

object NcoreLoader {
    private const val TAG = "ncore_loader"

    @Volatile
    private var isLibraryLoaded = false

    @Synchronized
    fun init() {
        if (isLibraryLoaded) return
        try {
            System.loadLibrary("jni")
            isLibraryLoaded = true
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "Failed to load ncore library: ${e.message}")
        }
    }
}

object Ncore {
    private const val JNI_THREAD_NAME = "ncore-jni"

    private val onJniThread: ThreadLocal<Boolean> =
        ThreadLocal.withInitial<Boolean> { false }

    private val jniExecutor =
        Executors.newSingleThreadExecutor { r ->
            Thread({
                onJniThread.set(true)
                r.run()
            }, JNI_THREAD_NAME).apply { isDaemon = true }
        }

    private val jniDispatcher: CoroutineDispatcher = jniExecutor.asCoroutineDispatcher()

    private fun <T> runJni(block: () -> T): T {
        NcoreLoader.init()
        if (onJniThread.get() == true) return block()
        return try {
            jniExecutor.submit(block).get()
        } catch (e: ExecutionException) {
            throw e.cause ?: e
        }
    }

    private suspend fun <T> runJniAsync(block: () -> T): T {
        NcoreLoader.init()
        return withContext(jniDispatcher) { block() }
    }

    @JvmName("helloLog")
    private external fun nativeHelloLog()

    @JvmName("ctl")
    private external fun nativeCtl(value: Int): Int

    @JvmName("adduid")
    private external fun nativeAdduid(value: Int): Int

    @JvmName("deluid")
    private external fun nativeDeluid(value: Int): Int

    @JvmName("hasuid")
    private external fun nativeHasuid(value: Int): Int

    @JvmName("addRule")
    private external fun nativeAddRule(
        path: String,
        statusBits: Long,
    ): Int

    @JvmName("delRule")
    private external fun nativeDelRule(path: String): Int

    @JvmName("setCap")
    private external fun nativeSetCap(
        uid: Int,
        caps: Long,
    ): Int

    @JvmName("getCap")
    private external fun nativeGetCap(uid: Int): Long

    @JvmName("delCap")
    private external fun nativeDelCap(uid: Int): Int

    @JvmName("addSelinuxRule")
    private external fun nativeAddSelinuxRule(
        src: String?,
        tgt: String?,
        cls: String?,
        perm: String?,
        effect: Int,
        invert: Boolean,
    ): Int

    @JvmName("setProfileProto")
    private external fun nativeSetProfileProto(data: ByteArray): Int

    @JvmName("listProfiles")
    private external fun nativeListProfiles(): String?

    @JvmName("readFile")
    private external fun nativeReadFile(path: String): ByteArray?

    @JvmName("readFilePersistent")
    private external fun nativeReadFilePersistent(path: String): ByteArray?

    @JvmName("writeBlock")
    private external fun nativeWriteBlock(
        srcPath: String,
        dstPath: String,
    ): Boolean

    @JvmName("patchVendorBoot")
    private external fun nativePatchVendorBoot(
        imagePath: String,
        modulePath: String,
        bootPath: String?,
        verifySymbols: Boolean,
        outputPath: String,
    ): String?

    @JvmName("patchBoot")
    private external fun nativePatchBoot(
        imagePath: String,
        modulePath: String,
        initPath: String,
        outputPath: String,
    ): String?

    @JvmName("isGki")
    private external fun nativeIsGki(): Boolean

    @JvmName("kernelVersion")
    private external fun nativeKernelVersion(): String?

    @JvmName("kernelRelease")
    private external fun nativeKernelRelease(): String?

    @JvmName("moduleVersion")
    private external fun nativeModuleVersion(): String?

    @JvmName("featureList")
    private external fun nativeFeatureList(): String?

    @JvmName("featureGet")
    private external fun nativeFeatureGet(id: Int): Long

    @JvmName("featureSet")
    private external fun nativeFeatureSet(data: ByteArray): Int

    @JvmName("getSlotSuffix")
    private external fun nativeGetSlotSuffix(): String?

    @JvmName("findVendorBoot")
    private external fun nativeFindVendorBoot(
        slotSuffix: String,
        allowSlotless: Boolean,
    ): String?

    @JvmName("findBoot")
    private external fun nativeFindBoot(
        slotSuffix: String,
        allowSlotless: Boolean,
    ): String?

    @JvmName("helloLogApi")
    fun helloLog(): Unit = runJni { nativeHelloLog() }

    /** Returns 0 when the kernel control channel is available, -1 otherwise. */
    @JvmName("ctlApi")
    fun ctl(value: Int): Int = runJni { nativeCtl(value) }

    @JvmName("adduidApi")
    fun adduid(value: Int): Int = runJni { nativeAdduid(value) }

    @JvmName("deluidApi")
    fun deluid(value: Int): Int = runJni { nativeDeluid(value) }

    @JvmName("hasuidApi")
    fun hasuid(value: Int): Int = runJni { nativeHasuid(value) }

    @JvmName("addRuleApi")
    fun addRule(
        path: String,
        statusBits: Long,
    ): Int = runJni { nativeAddRule(path, statusBits) }

    @JvmName("delRuleApi")
    fun delRule(path: String): Int = runJni { nativeDelRule(path) }

    @JvmName("setCapApi")
    fun setCap(
        uid: Int,
        caps: Long,
    ): Int = runJni { nativeSetCap(uid, caps) }

    @JvmName("getCapApi")
    fun getCap(uid: Int): Long = runJni { nativeGetCap(uid) }

    @JvmName("delCapApi")
    fun delCap(uid: Int): Int = runJni { nativeDelCap(uid) }

    @JvmName("addSelinuxRuleApi")
    fun addSelinuxRule(
        src: String?,
        tgt: String?,
        cls: String?,
        perm: String?,
        effect: Int,
        invert: Boolean,
    ): Int = runJni { nativeAddSelinuxRule(src, tgt, cls, perm, effect, invert) }

    @JvmName("setProfileProtoApi")
    fun setProfileProto(data: ByteArray): Int = runJni { nativeSetProfileProto(data) }

    @JvmName("listProfilesApi")
    fun listProfiles(): String? = runJni { nativeListProfiles() }

    @JvmName("readFileApi")
    fun readFile(path: String): ByteArray? = runJni { nativeReadFile(path) }

    @JvmName("readFilePersistentApi")
    fun readFilePersistent(path: String): ByteArray? = runJni { nativeReadFilePersistent(path) }

    @JvmName("writeBlockApi")
    fun writeBlock(
        srcPath: String,
        dstPath: String,
    ): Boolean = runJni { nativeWriteBlock(srcPath, dstPath) }

    /** Patch [imagePath] in-process and write the result to [outputPath]. null means success. */
    @JvmName("patchVendorBootApi")
    fun patchVendorBoot(
        imagePath: String,
        modulePath: String,
        bootPath: String?,
        verifySymbols: Boolean,
        outputPath: String,
    ): String? =
        runJni {
            nativePatchVendorBoot(imagePath, modulePath, bootPath, verifySymbols, outputPath)
        }

    @JvmName("patchBootApi")
    fun patchBoot(
        imagePath: String,
        modulePath: String,
        initPath: String,
        outputPath: String,
    ): String? =
        runJni {
            nativePatchBoot(imagePath, modulePath, initPath, outputPath)
        }

    @JvmName("isGkiApi")
    fun isGki(): Boolean = runJni { nativeIsGki() }

    @JvmName("kernelVersionApi")
    fun kernelVersion(): String? = runJni { nativeKernelVersion() }

    @JvmName("kernelReleaseApi")
    fun kernelRelease(): String? = runJni { nativeKernelRelease() }

    @JvmName("moduleVersionApi")
    fun moduleVersion(): String? = runJni { nativeModuleVersion() }

    @JvmName("featureListApi")
    fun featureList(): String? = runJni { nativeFeatureList() }

    @JvmName("featureGetApi")
    fun featureGet(id: Int): Long = runJni { nativeFeatureGet(id) }

    @JvmName("featureSetApi")
    fun featureSet(data: ByteArray): Int = runJni { nativeFeatureSet(data) }

    @JvmName("getSlotSuffixApi")
    fun getSlotSuffix(): String? = runJni { nativeGetSlotSuffix() }

    @JvmName("findVendorBootApi")
    fun findVendorBoot(
        slotSuffix: String,
        allowSlotless: Boolean,
    ): String? = runJni { nativeFindVendorBoot(slotSuffix, allowSlotless) }

    @JvmName("findBootApi")
    fun findBoot(
        slotSuffix: String,
        allowSlotless: Boolean,
    ): String? = runJni { nativeFindBoot(slotSuffix, allowSlotless) }

    object Suspend {
        suspend fun helloLog() = runJniAsync { nativeHelloLog() }

        suspend fun ctl(value: Int): Int = runJniAsync { nativeCtl(value) }

        suspend fun adduid(value: Int): Int = runJniAsync { nativeAdduid(value) }

        suspend fun deluid(value: Int): Int = runJniAsync { nativeDeluid(value) }

        suspend fun hasuid(value: Int): Int = runJniAsync { nativeHasuid(value) }

        suspend fun kernelVersion(): String? = runJniAsync { nativeKernelVersion() }

        suspend fun readFile(path: String): ByteArray? = runJniAsync { nativeReadFile(path) }
    }
}
