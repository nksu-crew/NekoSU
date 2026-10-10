package me.nekosu.aqnya.util

import kotlinx.serialization.Serializable

/**
 * 一个 Magisk / KernelSU 风格模块的元数据。
 *
 * 由内核接口（IOC_LIST_MODULES 返回的 JSON）提供，见 [ModuleRepository]。
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
    // The kernel interface does not report these yet; the UI falls back to "not available"
    // until a WebUI / action script probe is wired up.
    val hasWebUi: Boolean = false,
    val hasActionScript: Boolean = false,
    val actionIconPath: String? = null,
    val webUiIconPath: String? = null,
) {
    val title: String get() = name.ifBlank { id }

    val subtitle: String
        get() =
            buildList {
                if (version.isNotBlank()) add(version)
                if (author.isNotBlank()) add(author)
            }.joinToString(" · ")

    val installDir: String get() = "/data/adb/modules/$id"
}
