package me.nekosu.aqnya.ui.screens.modules

import android.app.Application
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Error
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import me.nekosu.aqnya.R
import me.nekosu.aqnya.ui.component.CardItem
import me.nekosu.aqnya.util.ModuleInfo

/**
 * 模块管理页 —— 参照 KernelSU 管理器的模块列表 / 安装界面。
 *
 * 列表由用户态 ncore 提供；安装走 root shell（见 [me.nekosu.aqnya.util.ModuleRepository]）。
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

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.modules_title)) },
                colors =
                    TopAppBarDefaults.topAppBarColors(
                        containerColor = MaterialTheme.colorScheme.surface,
                        scrolledContainerColor = MaterialTheme.colorScheme.surface,
                    ),
            )
        },
        snackbarHost = { SnackbarHost(snackbarHostState) },
        floatingActionButton = {
            if (!state.installing && !state.installDone && state.actionModuleId == null) {
                FloatingActionButton(
                    onClick = { pickZip.launch(arrayOf("application/zip", "application/octet-stream", "*/*")) },
                ) {
                    Icon(Icons.Filled.Add, contentDescription = stringResource(R.string.modules_install))
                }
            }
        },
        contentWindowInsets =
            WindowInsets.safeDrawing.only(
                WindowInsetsSides.Top + WindowInsetsSides.Horizontal,
            ),
    ) { innerPadding ->
        Box(modifier = Modifier.fillMaxSize().padding(innerPadding)) {
            when {
                state.installing || state.installDone -> {
                    InstallSection(state = state, onClose = moduleViewModel::resetInstall)
                }

                state.actionModuleId != null -> {
                    ActionSection(state = state, onClose = moduleViewModel::resetAction)
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
private fun ModuleCard(
    module: ModuleInfo,
    onToggle: (Boolean) -> Unit,
    onAction: () -> Unit,
    onRemove: () -> Unit,
    onUninstall: () -> Unit,
) {
    var menuOpen by remember { mutableStateOf(false) }

    CardItem(index = 0, total = 1) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        text = module.title,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold,
                        maxLines = 1,
                    )
                    if (module.metamodule) {
                        Spacer(Modifier.width(8.dp))
                        Badge(stringResource(R.string.modules_metamodule))
                    }
                    if (module.update) {
                        Spacer(Modifier.width(8.dp))
                        Badge(stringResource(R.string.modules_update))
                    }
                }
                if (module.subtitle.isNotBlank()) {
                    Text(
                        text = module.subtitle,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                if (module.description.isNotBlank()) {
                    Text(
                        text = module.description,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }

            Spacer(Modifier.width(8.dp))

            if (module.hasActionScript && module.enabled && !module.remove) {
                IconButton(onClick = onAction) {
                    Icon(
                        Icons.Filled.PlayArrow,
                        contentDescription = stringResource(R.string.modules_action_run),
                    )
                }
            }

            Switch(
                checked = module.enabled,
                onCheckedChange = onToggle,
            )

            Box {
                IconButton(onClick = { menuOpen = true }) {
                    Icon(Icons.Filled.MoreVert, contentDescription = stringResource(R.string.cd_more))
                }
                DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
                    if (module.hasActionScript && module.enabled && !module.remove) {
                        DropdownMenuItem(
                            text = { Text(stringResource(R.string.modules_action_run)) },
                            onClick = {
                                menuOpen = false
                                onAction()
                            },
                        )
                    }
                    DropdownMenuItem(
                        text = {
                            Text(
                                stringResource(
                                    if (module.enabled) R.string.modules_action_disable else R.string.modules_action_enable,
                                ),
                            )
                        },
                        onClick = {
                            menuOpen = false
                            onToggle(!module.enabled)
                        },
                    )
                    DropdownMenuItem(
                        text = { Text(stringResource(R.string.modules_action_remove)) },
                        onClick = {
                            menuOpen = false
                            onRemove()
                        },
                    )
                    DropdownMenuItem(
                        text = { Text(stringResource(R.string.modules_action_uninstall)) },
                        onClick = {
                            menuOpen = false
                            onUninstall()
                        },
                    )
                }
            }
        }
    }
}

@Composable
private fun Badge(text: String) {
    Surface(
        shape = RoundedCornerShape(6.dp),
        color = MaterialTheme.colorScheme.primary.copy(alpha = 0.15f),
    ) {
        Text(
            text = text,
            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.primary,
        )
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

@Composable
private fun InstallSection(
    state: ModulesUiState,
    onClose: () -> Unit,
) {
    val accent =
        when {
            state.installing -> MaterialTheme.colorScheme.primary
            state.installSuccess == true -> MaterialTheme.colorScheme.primary
            else -> MaterialTheme.colorScheme.error
        }

    Column(
        modifier = Modifier.fillMaxSize().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Card(
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(24.dp),
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer),
            elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
        ) {
            Column(modifier = Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    when {
                        state.installing -> CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.5.dp)
                        state.installSuccess == true -> Icon(Icons.Filled.CheckCircle, contentDescription = null, tint = accent)
                        else -> Icon(Icons.Filled.Error, contentDescription = null, tint = accent)
                    }
                    Spacer(Modifier.width(12.dp))
                    Text(
                        text =
                            when {
                                state.installing -> stringResource(R.string.modules_install_running)
                                state.installSuccess == true -> stringResource(R.string.modules_install_done)
                                else -> stringResource(R.string.modules_install_failed)
                            },
                        style = MaterialTheme.typography.titleMedium,
                        color = accent,
                    )
                }

                if (state.installing) {
                    LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                }

                if (state.installLog.isNotBlank()) {
                    Surface(
                        modifier = Modifier.fillMaxWidth().heightIn(max = 360.dp),
                        shape = RoundedCornerShape(16.dp),
                        color = MaterialTheme.colorScheme.surfaceContainerHighest,
                    ) {
                        SelectionContainer {
                            Text(
                                text = state.installLog,
                                modifier =
                                    Modifier
                                        .fillMaxWidth()
                                        .verticalScroll(rememberScrollState())
                                        .padding(12.dp),
                                style = MaterialTheme.typography.bodySmall,
                                fontFamily = FontFamily.Monospace,
                            )
                        }
                    }
                }
            }
        }

        if (state.installDone) {
            Button(onClick = onClose, modifier = Modifier.fillMaxWidth()) {
                Text(stringResource(R.string.modules_install_close))
            }
        }
    }
}

@Composable
private fun ActionSection(
    state: ModulesUiState,
    onClose: () -> Unit,
) {
    val scrollState = rememberScrollState()
    val accent =
        when {
            state.actionRunning -> MaterialTheme.colorScheme.primary
            state.actionSuccess -> MaterialTheme.colorScheme.primary
            else -> MaterialTheme.colorScheme.error
        }

    LaunchedEffect(state.actionOutput) {
        scrollState.animateScrollTo(scrollState.maxValue)
    }

    Column(
        modifier = Modifier.fillMaxSize().padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Card(
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(24.dp),
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer),
            elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
        ) {
            Column(modifier = Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    when {
                        state.actionRunning -> CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.5.dp)
                        state.actionSuccess -> Icon(Icons.Filled.CheckCircle, contentDescription = null, tint = accent)
                        else -> Icon(Icons.Filled.Error, contentDescription = null, tint = accent)
                    }
                    Spacer(Modifier.width(12.dp))
                    Text(
                        text =
                            when {
                                state.actionRunning -> stringResource(R.string.modules_action_running)
                                state.actionSuccess -> stringResource(R.string.modules_action_done)
                                else -> stringResource(R.string.modules_action_failed)
                            },
                        style = MaterialTheme.typography.titleMedium,
                        color = accent,
                    )
                }

                if (state.actionOutput.isNotBlank()) {
                    Surface(
                        modifier = Modifier.fillMaxWidth().heightIn(max = 420.dp),
                        shape = RoundedCornerShape(16.dp),
                        color = MaterialTheme.colorScheme.surfaceContainerHighest,
                    ) {
                        SelectionContainer {
                            Text(
                                text = state.actionOutput,
                                modifier =
                                    Modifier
                                        .fillMaxWidth()
                                        .verticalScroll(scrollState)
                                        .padding(12.dp),
                                style = MaterialTheme.typography.bodySmall,
                                fontFamily = FontFamily.Monospace,
                            )
                        }
                    }
                }
            }
        }

        if (state.actionDone) {
            Button(onClick = onClose, modifier = Modifier.fillMaxWidth()) {
                Text(stringResource(R.string.modules_action_close))
            }
        }
    }
}
