package me.nekosu.aqnya.util

import kotlinx.serialization.Serializable

/** `updateJson` 指向的远端更新信息（Magisk / KernelSU 格式）。 */
@Serializable
data class ModuleUpdateInfo(
    val version: String = "",
    val versionCode: Int = 0,
    val zipUrl: String = "",
    val changelog: String = "",
)
