package me.nekosu.aqnya.ui.screens.modules

import android.app.Application
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Save
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExtendedFloatingActionButton
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
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
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import me.nekosu.aqnya.R
import me.nekosu.aqnya.ui.component.KeyEventBlocker
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
                    TopAppBar(
                        title = { Text(stringResource(R.string.modules_title)) },
                        colors =
                            TopAppBarDefaults.topAppBarColors(
                                containerColor = MaterialTheme.colorScheme.surface,
                                scrolledContainerColor = MaterialTheme.colorScheme.surface,
                            ),
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
                    FloatingActionButton(
                        onClick = { pickZip.launch(arrayOf("application/zip", "application/octet-stream", "*/*")) },
                    ) {
                        Icon(Icons.Filled.Add, contentDescription = stringResource(R.string.modules_install))
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
                    LogPane(text = state.installLog, running = state.installing, contentPadding = innerPadding)
                }

                actionVisible -> {
                    LogPane(text = state.actionOutput, running = state.actionRunning, contentPadding = innerPadding)
                }

                state.modules.isEmpty() -> {
                    EmptyState(loading = state.loading)
                }

                else -> {
                    LazyColumn(
                        modifier = Modifier.fillMaxSize(),
                        contentPadding = PaddingValues(horizontal = 16.dp, vertical = 12.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        items(state.modules, key = { it.id }) { module ->
                            ModuleCard(
                                module = module,
                                onToggle = { enabled -> moduleViewModel.setEnabled(module, enabled) },
                                onAction = { moduleViewModel.runAction(module) },
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

/**
 * 安装 / action 界面的顶栏 —— 参照 KernelSU：左侧返回，右侧保存日志。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LogTopBar(
    title: String,
    onBack: (() -> Unit)?,
    onSaveLog: () -> Unit,
) {
    TopAppBar(
        title = { Text(title) },
        colors =
            TopAppBarDefaults.topAppBarColors(
                containerColor = MaterialTheme.colorScheme.surface,
                scrolledContainerColor = MaterialTheme.colorScheme.surface,
            ),
        navigationIcon = {
            if (onBack != null) {
                IconButton(onClick = onBack) {
                    Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = stringResource(R.string.cd_back))
                }
            }
        },
        actions = {
            IconButton(onClick = onSaveLog) {
                Icon(Icons.Filled.Save, contentDescription = stringResource(R.string.cd_save))
            }
        },
    )
}

/**
 * 整屏日志 —— 与 KernelSU 相同，直接把输出以等宽字体铺满屏幕并自动滚到底部，
 * 不再套卡片 / 圆角框。
 */
@Composable
private fun LogPane(
    text: String,
    running: Boolean,
    contentPadding: PaddingValues,
) {
    val scrollState = rememberScrollState()

    LaunchedEffect(text) {
        scrollState.animateScrollTo(scrollState.maxValue)
    }

    Box(modifier = Modifier.fillMaxSize().padding(contentPadding)) {
        // 安装走一次性 root 命令，日志要等结束才回填；此时先给一个进度指示。
        if (text.isBlank() && running) {
            CircularProgressIndicator(
                modifier = Modifier.align(Alignment.Center).size(36.dp),
                strokeWidth = 2.5.dp,
            )
        }

        Column(
            modifier = Modifier.fillMaxSize().verticalScroll(scrollState),
        ) {
            SelectionContainer {
                Text(
                    text = text,
                    modifier = Modifier.fillMaxWidth().padding(8.dp),
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                )
            }
            // 给底部按钮留出空间，避免遮住最后几行日志。
            Spacer(Modifier.height(88.dp))
        }
    }
}

/** 日志结束后的「关闭」按钮，对应 KernelSU 的 extended FAB。 */
@Composable
private fun LogDoneFab(
    label: String,
    onClick: () -> Unit,
) {
    ExtendedFloatingActionButton(
        onClick = onClick,
        icon = { Icon(Icons.Filled.Close, contentDescription = null) },
        text = { Text(label) },
    )
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
