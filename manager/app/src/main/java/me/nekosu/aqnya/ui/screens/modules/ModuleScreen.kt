package me.nekosu.aqnya.ui.screens.modules

import android.app.Application
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Add
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.key
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import dev.chrisbanes.haze.hazeSource
import dev.chrisbanes.haze.rememberHazeState
import me.nekosu.aqnya.R
import me.nekosu.aqnya.ui.component.AppFloatingActionButton
import me.nekosu.aqnya.ui.component.HazeTopAppBar
import me.nekosu.aqnya.ui.component.KeyEventBlocker
import me.nekosu.aqnya.ui.component.LogDoneFab
import me.nekosu.aqnya.ui.component.LogPane
import me.nekosu.aqnya.ui.component.LogTopBar
import me.nekosu.aqnya.ui.navigation.LocalBottomBarClearance
import me.nekosu.aqnya.ui.webui.WebUIActivity
import me.nekosu.aqnya.util.LogUtils

/**
 * 模块管理页 —— 参照 KernelSU 管理器的模块列表 / 安装界面。
 *
 * 列表由用户态 ncore 提供；安装与 action 输出都直接展示为整屏日志
 * （见 [LogPane] / [LogTopBar]），与 KernelSU 一致，不再套一层卡片。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ModuleScreen() {
    val context = LocalContext.current
    val moduleViewModel: ModuleViewModel =
        viewModel(factory = ModuleViewModelFactory(context.applicationContext as Application))

    val state by moduleViewModel.uiState.collectAsState()
    val snackbarHostState = remember { SnackbarHostState() }
    val hazeState = rememberHazeState()

    val pickZip =
        rememberLauncherForActivityResult(
            contract = ActivityResultContracts.OpenDocument(),
        ) { uri ->
            if (uri != null) moduleViewModel.install(uri)
        }

    LaunchedEffect(Unit) { moduleViewModel.refresh() }

    LaunchedEffect(state.message) {
        val message = state.message ?: return@LaunchedEffect
        snackbarHostState.showSnackbar(message)
        moduleViewModel.consumeMessage()
    }

    val installVisible = state.installing || state.installDone
    val actionVisible = state.actionModuleId != null
    val busy = state.installing || state.actionRunning

    // 安装 / action 进行中时屏蔽返回，避免中途离开导致协程被取消。
    BackHandler(enabled = busy) { }

    // 交互式安装（音量键选择等）可能等待较久，进行中保持屏幕常亮。
    val view = LocalView.current
    DisposableEffect(view, busy) {
        view.keepScreenOn = busy
        onDispose { view.keepScreenOn = false }
    }

    Scaffold(
        containerColor = MaterialTheme.colorScheme.surfaceContainer,
        topBar = {
            when {
                installVisible -> {
                    LogTopBar(
                        title =
                            when {
                                state.installing -> stringResource(R.string.modules_install_running)
                                state.installSuccess == true -> stringResource(R.string.modules_install_done)
                                else -> stringResource(R.string.modules_install_failed)
                            },
                        onBack = if (state.installing) null else moduleViewModel::resetInstall,
                        onSaveLog = {
                            LogUtils.saveLog(context, "nksu-module-install.log", state.installLog)
                        },
                    )
                }

                actionVisible -> {
                    LogTopBar(
                        title =
                            when {
                                state.actionRunning -> stringResource(R.string.modules_action_running)
                                state.actionSuccess -> stringResource(R.string.modules_action_done)
                                else -> stringResource(R.string.modules_action_failed)
                            },
                        onBack = if (state.actionRunning) null else moduleViewModel::resetAction,
                        onSaveLog = {
                            LogUtils.saveLog(context, "nksu-module-action.log", state.actionOutput)
                        },
                    )
                }

                else -> {
                    HazeTopAppBar(
                        state = hazeState,
                        title = stringResource(R.string.modules_title),
                    )
                }
            }
        },
        snackbarHost = { SnackbarHost(snackbarHostState) },
        floatingActionButton = {
            when {
                state.installDone -> {
                    LogDoneFab(
                        label = stringResource(R.string.modules_install_close),
                        onClick = moduleViewModel::resetInstall,
                    )
                }

                state.actionDone -> {
                    LogDoneFab(
                        label = stringResource(R.string.modules_action_close),
                        onClick = moduleViewModel::resetAction,
                    )
                }

                !state.installing && !actionVisible -> {
                    AppFloatingActionButton(
                        onClick = { pickZip.launch(arrayOf("application/zip", "application/octet-stream", "*/*")) },
                    ) {
                        Icon(Icons.Rounded.Add, contentDescription = stringResource(R.string.modules_install))
                    }
                }

                else -> Unit
            }
        },
        contentWindowInsets =
            WindowInsets.safeDrawing.only(
                WindowInsetsSides.Top + WindowInsetsSides.Horizontal,
            ),
    ) { innerPadding ->
        Box(modifier = Modifier.fillMaxSize()) {
            // 日志界面显示时消费音量键，避免系统音量条盖住提示；脚本仍从
            // /dev/input 读到按键，因此不影响「按音量键选择」。
            if (installVisible || actionVisible) {
                KeyEventBlocker { it.key == Key.VolumeDown || it.key == Key.VolumeUp }
            }

            when {
                installVisible -> {
                    Box(Modifier.fillMaxSize().padding(innerPadding)) {
                        LogPane(text = state.installLog, running = state.installing)
                    }
                }

                actionVisible -> {
                    Box(Modifier.fillMaxSize().padding(innerPadding)) {
                        LogPane(text = state.actionOutput, running = state.actionRunning)
                    }
                }

                state.modules.isEmpty() -> {
                    Box(Modifier.fillMaxSize().padding(innerPadding)) {
                        EmptyState(loading = state.loading)
                    }
                }

                else -> {
                    LazyColumn(
                        modifier = Modifier.fillMaxSize().hazeSource(hazeState),
                        contentPadding =
                            PaddingValues(
                                start = 16.dp,
                                end = 16.dp,
                                // 用顶栏高度做内容内边距，列表往上滚时经过顶栏下方。
                                top = innerPadding.calculateTopPadding() + 12.dp,
                                // 底部留出空间，避免最后一张卡被安装 FAB / 悬浮导航栏挡住。
                                bottom = LocalBottomBarClearance.current,
                            ),
                        verticalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        items(state.modules, key = { it.id }) { module ->
                            ModuleCard(
                                module = module,
                                updateInfo = state.updates[module.id],
                                onToggle = { enabled -> moduleViewModel.setEnabled(module, enabled) },
                                onUpdate = { moduleViewModel.update(module) },
                                onAction = { moduleViewModel.runAction(module) },
                                onOpenWebUi = { context.startActivity(WebUIActivity.intent(context, module.id)) },
                                onRemove = { moduleViewModel.remove(module) },
                                onUndoRemove = { moduleViewModel.undoRemove(module) },
                                onUninstall = { moduleViewModel.uninstall(module) },
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun EmptyState(loading: Boolean) {
    Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        if (loading) {
            CircularProgressIndicator()
        } else {
            Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(
                    text = stringResource(R.string.modules_empty),
                    style = MaterialTheme.typography.titleMedium,
                )
                Text(
                    text = stringResource(R.string.modules_empty_hint),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}
