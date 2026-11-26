// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.data.installer

import me.nekosu.aqnya.core.Ncore
import me.nekosu.aqnya.data.shell.RootShell

/**
 * root 下的分区访问：slot 查询、分区查找、dump / 写回与容量查询。
 *
 * 优先走 nksu 内核模块的 JNI（fork + setresuid，无需外部 su）；模块未加载时
 * 回退到 `/system/bin/su`，让只有 Magisk/KernelSU 的设备也能直接安装。
 */
internal object PartitionAccess {
    fun currentSlotSuffix(): String? = Ncore.getSlotSuffix()

    /** 非活动 slot 后缀；非 A/B 设备返回 null。 */
    fun inactiveSlotSuffix(): String? =
        when (currentSlotSuffix()) {
            "_a" -> "_b"
            "_b" -> "_a"
            else -> null
        }

    fun isAbDevice(): Boolean = currentSlotSuffix() != null

    /** nksu 内核模块或外部 su 任一可用即视为具备 root。 */
    fun hasRoot(): Boolean = Ncore.ctl(1) == 0 || RootShell.available()

    /**
     * 查找分区路径。
     *
     * @slotSuffix null/空按无后缀处理；[allowSlotless] 允许回退到无后缀的
     * 分区链接（只对当前 slot 安全，非活动 slot 不能回退，否则会误写当前分区）。
     */
    fun findPartition(
        partition: String,
        slotSuffix: String?,
        allowSlotless: Boolean,
    ): String? {
        val slot = slotSuffix.orEmpty()
        return Ncore.findBlock(partition, slot, allowSlotless)
            ?: if (RootShell.available()) RootShell.findBlock(partition, slot, allowSlotless) else null
    }

    /** 分区容量；没有外部 su（只能走 nksu JNI）时无法查询，返回 null。 */
    fun partitionSize(partition: String): Long? {
        if (!RootShell.available()) return null
        val result = RootShell.exec("blockdev --getsize64 ${quote(partition)} 2>/dev/null")
        return if (result.code == 0) result.output.trim().toLongOrNull() else null
    }

    /** 以 root 复制文件 / 分区：优先 nksu 内核模块，失败再回退到外部 su。 */
    fun copyBlock(
        src: String,
        dst: String,
    ): Boolean =
        Ncore.writeBlock(src, dst) ||
            (RootShell.available() && RootShell.copyFile(src, dst))

    private fun quote(s: String): String = "'" + s.replace("'", "'\\''") + "'"
}
