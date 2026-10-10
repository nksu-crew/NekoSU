package me.nekosu.aqnya.util

import kotlinx.serialization.Serializable

/**
 * 一个 Magisk / KernelSU 风格模块的元数据。
 *
 * 由用户态 ncore 枚举并以 JSON 返回，见 [ModuleRepository]。
 */
@Serializable
data class ModuleInfo(
    val id: String,
    val name: String = "",
    val version: String = "",
    val versionCode: String = "",
    val author: String = "",
    val description: String = "",
    val enabled: Boolean = true,
    val metamodule: Boolean = false,
    val update: Boolean = false,
    val remove: Boolean = false,
    val skipMount: Boolean = false,
    val hasSystem: Boolean = false,
    val hasActionScript: Boolean = false,
) {
    val title: String get() = name.ifBlank { id }

    val installDir: String get() = "/data/adb/modules/$id"
}
