package me.nekosu.aqnya.ui.screens.modules

import android.app.Application
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import me.nekosu.aqnya.R

/**
 * 模块管理页 —— 复刻 KernelSU 管理器的 MD3 (Material 3) 模块页。
 *
 * 列表来自内核接口；启用 / 卸载 / 安装走内核提供的 root shell。
 * 由于 NekoSU 目前没有模块仓库、WebUI、action 脚本等后端，这些入口以占位实现接入。
 */
@Composable
fun ModuleScreen() {
    val context = LocalContext.current
    val moduleViewModel: ModuleViewModel =
        viewModel(factory = ModuleViewModelFactory(context.applicationContext as Application))

    val uiState by moduleViewModel.uiState.collectAsState()

    LaunchedEffect(Unit) { moduleViewModel.fetchModuleList() }

    val actions =
        ModuleActions(
            onRefresh = { moduleViewModel.fetchModuleList() },
            onSearchStatusChange = moduleViewModel::updateSearchStatus,
            onSearchTextChange = moduleViewModel::updateSearchText,
            onClearSearch = { moduleViewModel.updateSearchText("") },
            onRequestUpdateConfirmation = moduleViewModel::requestUpdateConfirmation,
            onRequestUninstallConfirmation = moduleViewModel::requestUninstallConfirmation,
            onDismissConfirmRequest = moduleViewModel::dismissConfirmRequest,
            onConfirmUpdate = {
                moduleViewModel.emitEffect(ModuleEffect.Toast(context.getString(R.string.module_update_unsupported)))
            },
            onOpenRepo = {
                moduleViewModel.emitEffect(ModuleEffect.Toast(context.getString(R.string.module_repo_unsupported)))
            },
            onToggleSortActionFirst = moduleViewModel::toggleSortActionFirst,
            onToggleSortEnabledFirst = moduleViewModel::toggleSortEnabledFirst,
            onOpenWebUi = {
                moduleViewModel.emitEffect(ModuleEffect.Toast(context.getString(R.string.module_webui_unsupported)))
            },
            onToggleModule = moduleViewModel::toggleModule,
            onUninstallModule = moduleViewModel::uninstallModule,
            onUndoUninstallModule = moduleViewModel::undoUninstallModule,
            onOpenFlash = moduleViewModel::install,
            onExecuteModuleAction = {
                moduleViewModel.emitEffect(ModuleEffect.Toast(context.getString(R.string.module_action_unsupported)))
            },
        )

    ModulePagerMaterial(
        uiState = uiState,
        confirmDialogState = uiState.confirmDialogState,
        moduleEvent = moduleViewModel.moduleEvent,
        actions = actions,
        bottomInnerPadding = 0.dp,
    )
}
