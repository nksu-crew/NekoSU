package me.nekosu.aqnya.core

import android.util.Log

object NcoreLoader {
    private const val TAG = "ncore_loader"
    private var isLibraryLoaded = false

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
    external fun helloLog()

    external fun ctl(value: Int): Int

    external fun adduid(value: Int): Int

    external fun deluid(value: Int): Int

    external fun hasuid(value: Int): Int

    external fun addRule(
        path: String,
        statusBits: Long,
    ): Int

    external fun delRule(path: String): Int

    external fun setCap(
        uid: Int,
        caps: Long,
    ): Int

    external fun getCap(uid: Int): Long

    external fun delCap(uid: Int): Int

    external fun addSelinuxRule(
        src: String?,
        tgt: String?,
        cls: String?,
        perm: String?,
        effect: Int,
        invert: Boolean,
    ): Int

    external fun setProfile(
        uid: Int,
        rootUid: Int,
        caps: Long,
        domain: String,
        namespace: Int,
    ): Int

    external fun listProfiles(): String?

    external fun readFile(path: String): ByteArray?

    /**
     * 经常驻 root 子进程读取文件：只 fork 一次，之后按请求走管道，避免 WebUI
     * 每读一个资源都要 fork。返回空数组表示文件不存在或为空；返回 null 表示读取器
     * 本身失败，调用方应回退到 [readFile]。
     */
    external fun readFilePersistent(path: String): ByteArray?

    /** 把 [srcPath] 流式写入 [dstPath]（root 权限，子进程执行）；成功返回 true。 */
    external fun writeBlock(
        srcPath: String,
        dstPath: String,
    ): Boolean

    external fun isGki(): Boolean

    external fun kernelVersion(): String?

    /** uname() 的原始 release 字符串，如 `5.15.194-android13-8-...`；失败返回 null。 */
    external fun kernelRelease(): String?

    /** 运行中内核模块（nksu.ko）的构建版本，用于和 APK 版本比对。 */
    external fun moduleVersion(): String?

    /** 内核 feature 列表，每行 `<id> <name> <value>`；失败返回 null。 */
    external fun featureList(): String?

    /** 查询单个 feature 的值；失败返回 -1。 */
    external fun featureGet(id: Int): Long

    /** 开关 feature（value 非 0 开启）；成功返回 0。 */
    external fun featureSet(
        id: Int,
        value: Long,
    ): Int

    external fun getSlotSuffix(): String?

    /** 查找 vendor_boot 分区路径（root 子进程）；找不到返回 null。 */
    external fun findVendorBoot(
        slotSuffix: String,
        allowSlotless: Boolean,
    ): String?
}
