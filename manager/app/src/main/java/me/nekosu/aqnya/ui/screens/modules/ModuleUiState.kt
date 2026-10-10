package me.nekosu.aqnya.ui.screens.modules

import android.net.Uri
import androidx.compose.runtime.Immutable
import me.nekosu.aqnya.ui.component.SearchStatus
import me.nekosu.aqnya.util.ModuleInfo
import me.nekosu.aqnya.util.ModuleUpdateInfo

sealed interface ModuleConfirmRequest {
    data class Update(
        val module: ModuleInfo,
        val downloadUrl: String,
        val fileName: String,
    ) : ModuleConfirmRequest

    data class Uninstall(
        val module: ModuleInfo,
    ) : ModuleConfirmRequest
}

@Immutable
data class ModuleConfirmDialogState(
    val request: ModuleConfirmRequest,
    val title: String,
    val content: String? = null,
    val markdown: Boolean = false,
    val html: Boolean = false,
    val confirm: String? = null,
    val dismiss: String? = null,
)

sealed interface ModuleEffect {
    data class Toast(
        val message: String,
    ) : ModuleEffect

    data class SnackBar(
        val message: String,
    ) : ModuleEffect
}

@Immutable
data class ModuleUiState(
    val isRefreshing: Boolean = false,
    val hasLoaded: Boolean = false,
    val modules: List<ModuleInfo> = emptyList(),
    val moduleList: List<ModuleInfo> = emptyList(),
    val updateInfo: Map<String, ModuleUpdateInfo> = emptyMap(),
    val searchStatus: SearchStatus = SearchStatus(""),
    val searchResults: List<ModuleInfo> = emptyList(),
    val sortEnabledFirst: Boolean = false,
    val sortActionFirst: Boolean = false,
    val isSafeMode: Boolean = false,
    val magiskInstalled: Boolean = false,
    val confirmDialogState: ModuleConfirmDialogState? = null,
) {
    val installButtonVisible: Boolean
        get() = !(isSafeMode || magiskInstalled)
}

@Immutable
data class ModuleActions(
    val onRefresh: () -> Unit,
    val onSearchStatusChange: (SearchStatus) -> Unit,
    val onSearchTextChange: (String) -> Unit,
    val onClearSearch: () -> Unit,
    val onRequestUpdateConfirmation: (ModuleInfo, ModuleUpdateInfo) -> Unit,
    val onRequestUninstallConfirmation: (ModuleInfo) -> Unit,
    val onDismissConfirmRequest: () -> Unit,
    val onConfirmUpdate: (ModuleConfirmRequest.Update) -> Unit,
    val onOpenRepo: () -> Unit,
    val onToggleSortActionFirst: () -> Unit,
    val onToggleSortEnabledFirst: () -> Unit,
    val onOpenWebUi: (ModuleInfo) -> Unit,
    val onToggleModule: (ModuleInfo) -> Unit,
    val onUninstallModule: (ModuleInfo) -> Unit,
    val onUndoUninstallModule: (ModuleInfo) -> Unit,
    val onOpenFlash: (List<Uri>) -> Unit,
    val onExecuteModuleAction: (ModuleInfo) -> Unit,
)
