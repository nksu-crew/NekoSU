// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.core.model

/**
 * 安装目标分区：
 *  - [VENDOR_BOOT] GKI 设备，模块打进 vendor_boot 的 `lib/modules`；
 *  - [INIT_BOOT]  非 GKI 设备，init 包装器写进 boot/init_boot 的 ramdisk。
 */
enum class InstallTarget(
    /** 用于日志、确认弹窗与路由的可读分区名。 */
    val partitionName: String,
) {
    VENDOR_BOOT("vendor_boot"),
    INIT_BOOT("init_boot"),
}

/**
 * 安装策略：
 *  - [IMAGE]    选择外部镜像，导出补丁镜像（无需 root）；
 *  - [DIRECT]   直接读写当前 slot 的分区（需要 root）；
 *  - [INACTIVE] 读写非活动 A/B slot 的分区（需要 root + A/B）。
 */
enum class InstallStrategy {
    IMAGE,
    DIRECT,
    INACTIVE,
}
