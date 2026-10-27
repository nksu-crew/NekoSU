package me.nekosu.aqnya.data.kernel

import me.nekosu.aqnya.core.Ncore

object KernelInfo {
    private val ANDROID_TAG = Regex("-android(\\d+)")

    fun version(): String = normalize(Ncore.kernelVersion().orEmpty())

    fun isGki(): Boolean = Ncore.isGki()

    /**
     * 运行内核的 KMI，如 `android13-5.15`。
     *
     * 内核 uname release 里带 `-androidNN` 标记时直接用它；自定义内核常常没有
     * 这个标记（例如 `5.15.195-MorizukiNeko-260620LTSC`），此时返回 null，由
     * [matchKo] 退回到只按内核版本匹配，而不是抛异常崩掉安装页。
     */
    fun currentKmi(): String? {
        val android = androidVersion() ?: return null
        val v = version()
        if (v.isEmpty()) return null
        return "android$android-$v"
    }

    fun kmiOf(koName: String): String = koName.removeSuffix("_nksu.ko")

    fun matchKo(
        candidates: List<String>,
        kmi: String?,
    ): String? {
        if (candidates.isEmpty()) return null
        if (kmi != null) {
            candidates.firstOrNull { kmiOf(it) == kmi }?.let { return it }
        }
        val v = version()
        if (v.isNotEmpty()) {
            candidates.firstOrNull { kmiOf(it).endsWith("-$v") }?.let { return it }
        }
        return candidates.first()
    }

    private fun normalize(raw: String): String {
        val trimmed = raw.trim()
        val parts = trimmed.split('.', limit = 3)
        val major = parts.getOrNull(0)?.takeWhile { it.isDigit() }?.toIntOrNull() ?: return trimmed
        val minor = parts.getOrNull(1)?.takeWhile { it.isDigit() }?.toIntOrNull() ?: return trimmed
        return "$major.$minor"
    }

    /**
     * uname release 里的 `-androidNN` 版本号；自定义内核可能没有该标记，返回 null。
     */
    private fun androidVersion(): String? = Ncore.kernelRelease()?.let { ANDROID_TAG.find(it)?.groupValues?.getOrNull(1) }
}
