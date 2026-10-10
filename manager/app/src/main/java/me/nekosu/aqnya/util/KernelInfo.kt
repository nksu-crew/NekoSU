package me.nekosu.aqnya.util

import android.os.Build
import me.nekosu.aqnya.ncore

/**
 * 设备内核信息。
 *
 * 优先使用 JNI 提供的 [ncore.kernelVersion] / [ncore.isGki]；当 native 库不可用时
 * 回退到 [System.getProperty]("os.version")，保证界面始终有可展示的数据。
 */
object KernelInfo {
    private val ANDROID_TAG = Regex("-android(\\d+)")

    /**
     * 规范化后的内核版本（major.minor），例如 "5.10"、"6.1"。
     *
     * JNI 侧使用 "%d.%02d" 格式化，会把 6.1 输出为 "6.01"，这里统一修正为 "6.1"，
     * 以便与 assets/ko 中的 KMI 命名（android14-6.1_nksu.ko）对应。
     */
    fun version(): String? {
        val fromJni = runCatching { ncore.kernelVersion() }.getOrNull()
        return normalize(fromJni?.takeIf { it.isNotBlank() } ?: System.getProperty("os.version"))
    }

    /** 当前设备内核是否为 GKI。 */
    fun isGki(): Boolean = runCatching { ncore.isGki() }.getOrDefault(false)

    /** 设备当前 KMI，例如 "android14-6.1"。kernel version 不可用时返回 null。 */
    fun currentKmi(): String? = version()?.let { "android${androidVersion()}-$it" }

    /** 从 assets/ko 中的 "<kmi>_nksu.ko" 文件名提取 KMI。 */
    fun kmiOf(koName: String): String = koName.removeSuffix("_nksu.ko")

    /**
     * 在候选模块中挑选与设备最匹配的一个：
     *  1. KMI 完全一致；
     *  2. 内核版本一致；
     *  3. 退化为第一个候选。
     */
    fun matchKo(
        candidates: List<String>,
        kmi: String?,
    ): String? {
        if (candidates.isEmpty()) return null
        if (kmi != null) {
            candidates.firstOrNull { kmiOf(it) == kmi }?.let { return it }
        }
        version()?.let { v ->
            candidates.firstOrNull { kmiOf(it).endsWith("-$v") }?.let { return it }
        }
        return candidates.first()
    }

    private fun normalize(raw: String?): String? {
        if (raw.isNullOrBlank()) return null
        val trimmed = raw.trim()
        val parts = trimmed.split('.', limit = 3)
        val major = parts.getOrNull(0)?.takeWhile { it.isDigit() }?.toIntOrNull() ?: return trimmed
        val minor = parts.getOrNull(1)?.takeWhile { it.isDigit() }?.toIntOrNull() ?: return trimmed
        return "$major.$minor"
    }

    /**
     * KMI 中的 Android 版本号。优先解析内核 release 里的 "-androidNN" 标记
     * （例如 "5.10.198-android12-9-g1234567" -> 12），与 KernelSU 的做法一致；
     * 解析不到时退回 SDK_INT 映射。
     */
    private fun androidVersion(): String {
        System.getProperty("os.version")?.let { release ->
            ANDROID_TAG.find(release)?.groupValues?.getOrNull(1)?.let { return it }
        }
        return when {
            Build.VERSION.SDK_INT >= 36 -> "16"
            Build.VERSION.SDK_INT == 35 -> "15"
            Build.VERSION.SDK_INT == 34 -> "14"
            Build.VERSION.SDK_INT == 33 -> "13"
            else -> "12"
        }
    }
}
