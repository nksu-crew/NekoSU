package me.nekosu.aqnya.ui.screens.install

import android.app.Application
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.Android
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Error
import androidx.compose.material.icons.filled.InsertDriveFile
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.Numbers
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Smartphone
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.rememberTopAppBarState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavController
import me.nekosu.aqnya.R
import me.nekosu.aqnya.ui.component.CardGroup
import me.nekosu.aqnya.ui.component.CardItem
import me.nekosu.aqnya.ui.component.ListRow
import me.nekosu.aqnya.util.KernelInfo

/**
 * vendor_boot 安装界面 —— 参照 KernelSU 的 Material 3 安装页重构。
 *
 * 顶部 AppBar + 内核信息卡片 + 安装方式 + KMI 选择，底部固定安装按钮；
 * 安装过程中整页切换为进度与日志视图。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun InstallScreen(navController: NavController) {
    val context = LocalContext.current
    val installViewModel: InstallViewModel =
        viewModel(factory = InstallViewModelFactory(context.applicationContext as Application))

    val state by installViewModel.uiState.collectAsState()
    val scrollBehavior = TopAppBarDefaults.pinnedScrollBehavior(rememberTopAppBarState())

    val pickVendorBoot =
        rememberLauncherForActivityResult(
            contract = ActivityResultContracts.OpenDocument(),
        ) { uri ->
            if (uri != null) installViewModel.selectVendorBoot(uri)
        }

    val pickLocalKo =
        rememberLauncherForActivityResult(
            contract = ActivityResultContracts.OpenDocument(),
        ) { uri ->
            if (uri != null) installViewModel.selectLocalKo(uri)
        }

    LaunchedEffect(Unit) { installViewModel.load() }

    var confirmMethod by remember { mutableStateOf<InstallMethod?>(null) }
    var showKmiDialog by remember { mutableStateOf(false) }

    if (showKmiDialog) {
        ChooseKmiDialog(
            kmis = state.koCandidates.map { KernelInfo.kmiOf(it) },
            currentKmi = state.currentKmi,
            selected = (state.lkmSelection as? LkmSelection.Kmi)?.kmi ?: state.autoKo?.let { KernelInfo.kmiOf(it) },
            onDismiss = { showKmiDialog = false },
            onConfirm = { kmi ->
                showKmiDialog = false
                installViewModel.selectKmi(kmi)
            },
        )
    }

    confirmMethod?.let { method ->
        val inactive = method == InstallMethod.INACTIVE
        AlertDialog(
            onDismissRequest = { confirmMethod = null },
            title = {
                Text(
                    stringResource(
                        if (inactive) R.string.install_inactive_confirm_title else R.string.install_direct_confirm_title,
                    ),
                )
            },
            text = {
                Text(
                    stringResource(
                        if (inactive) {
                            R.string.install_inactive_confirm_message
                        } else {
                            R.string.install_direct_confirm_message
                        },
                        (if (inactive) state.inactiveTarget else state.directTarget).orEmpty(),
                    ),
                )
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        confirmMethod = null
                        installViewModel.install()
                    },
                ) {
                    Text(stringResource(R.string.install_direct_confirm_ok))
                }
            },
            dismissButton = {
                TextButton(onClick = { confirmMethod = null }) {
                    Text(stringResource(R.string.dialog_cancel))
                }
            },
        )
    }

    Scaffold(
        modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.install_title)) },
                navigationIcon = {
                    IconButton(onClick = { navController.popBackStack() }) {
                        Icon(
                            Icons.AutoMirrored.Filled.ArrowBack,
                            contentDescription = stringResource(R.string.cd_back),
                        )
                    }
                },
                scrollBehavior = scrollBehavior,
                colors =
                    TopAppBarDefaults.topAppBarColors(
                        containerColor = MaterialTheme.colorScheme.surface,
                        scrolledContainerColor = MaterialTheme.colorScheme.surface,
                    ),
            )
        },
        bottomBar = {
            InstallBottomBar(
                state = state,
                onInstall = {
                    when (state.method) {
                        InstallMethod.FILE -> installViewModel.install()
                        InstallMethod.DIRECT, InstallMethod.INACTIVE -> confirmMethod = state.method
                    }
                },
                onDone = { navController.popBackStack() },
            )
        },
        contentWindowInsets =
            WindowInsets.safeDrawing.only(
                WindowInsetsSides.Top + WindowInsetsSides.Horizontal,
            ),
    ) { innerPadding ->
        Column(
            modifier =
                Modifier
                    .fillMaxSize()
                    .padding(innerPadding)
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = 16.dp)
                    .padding(top = 8.dp, bottom = 16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            if (state.running || state.done) {
                InstallStatusSection(state)
            } else {
                KernelInfoSection(state)
                InstallMethodSection(
                    state = state,
                    onPick = { pickVendorBoot.launch(arrayOf("*/*")) },
                    onSelectMethod = installViewModel::selectMethod,
                    onRetryRoot = installViewModel::checkRoot,
                )
                KernelModuleSection(
                    state = state,
                    onSelectKmi = { showKmiDialog = true },
                    onPickLocal = { pickLocalKo.launch(arrayOf("*/*")) },
                    onClearLocal = installViewModel::clearLocalKo,
                )
                if (state.koCandidates.isEmpty()) {
                    Text(
                        text = stringResource(R.string.install_ko_missing),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            }
        }
    }
}

@Composable
private fun KernelInfoSection(state: InstallUiState) {
    val unknown = stringResource(R.string.about_unknown_version)
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        SectionTitle(stringResource(R.string.install_device_info))
        CardGroup {
            CardItem(index = 0, total = 3) {
                ListRow(
                    icon = { Icon(Icons.Filled.Memory, contentDescription = null) },
                    headline = { Text(stringResource(R.string.kernel_version)) },
                    supporting = { Text(state.kernelVersion ?: unknown) },
                )
            }
            CardItem(index = 1, total = 3) {
                ListRow(
                    icon = { Icon(Icons.Filled.Android, contentDescription = null) },
                    headline = { Text(stringResource(R.string.install_mode)) },
                    supporting = { Text(if (state.isGki) "GKI" else "LKM") },
                )
            }
            CardItem(index = 2, total = 3) {
                ListRow(
                    icon = { Icon(Icons.Filled.Numbers, contentDescription = null) },
                    headline = { Text(stringResource(R.string.install_current_kmi)) },
                    supporting = { Text(state.currentKmi ?: unknown) },
                )
            }
        }
    }
}

@Composable
private fun InstallMethodSection(
    state: InstallUiState,
    onPick: () -> Unit,
    onSelectMethod: (InstallMethod) -> Unit,
    onRetryRoot: () -> Unit,
) {
    val options =
        buildList {
            add(InstallMethod.FILE)
            // NekoSU 直接安装打的是 vendor_boot（不依赖 GKI 标签），只要拿到 root
            // 并找到分区就可用；不能再要求 isGki，否则自定义内核（无 -androidNN
            // 标签）的设备会被错误地隐藏这两个选项。
            if (state.rootAvailable) {
                if (state.directTarget != null) add(InstallMethod.DIRECT)
                if (state.inactiveTarget != null) add(InstallMethod.INACTIVE)
            }
        }

    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        SectionTitle(stringResource(R.string.install_method))
        CardGroup {
            options.forEachIndexed { index, option ->
                CardItem(index = index, total = options.size) {
                    InstallMethodRow(
                        state = state,
                        option = option,
                        onPick = onPick,
                        onSelectMethod = onSelectMethod,
                    )
                }
            }
        }

        when {
            state.rootChecked && !state.rootAvailable -> {
                Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(start = 4.dp)) {
                    Text(
                        text = stringResource(R.string.install_direct_no_root),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.weight(1f),
                    )
                    IconButton(onClick = onRetryRoot) {
                        Icon(Icons.Filled.Refresh, contentDescription = stringResource(R.string.install_direct_retry))
                    }
                }
            }

            state.rootChecking -> {
                Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(start = 4.dp)) {
                    CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                    Spacer(Modifier.width(8.dp))
                    Text(
                        text = stringResource(R.string.install_root_checking),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
    }
}

@Composable
private fun InstallMethodRow(
    state: InstallUiState,
    option: InstallMethod,
    onPick: () -> Unit,
    onSelectMethod: (InstallMethod) -> Unit,
) {
    val selected = state.method == option
    ListItem(
        modifier =
            Modifier
                .fillMaxWidth()
                .clickable(enabled = !state.running) {
                    when (option) {
                        InstallMethod.FILE ->
                            if (state.method == InstallMethod.FILE) onPick() else onSelectMethod(InstallMethod.FILE)
                        else -> onSelectMethod(option)
                    }
                },
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        leadingContent = {
            RadioButton(selected = selected, onClick = { onSelectMethod(option) })
        },
        headlineContent = {
            Text(
                stringResource(
                    when (option) {
                        InstallMethod.FILE -> R.string.install_method_file
                        InstallMethod.DIRECT -> R.string.install_method_direct
                        InstallMethod.INACTIVE -> R.string.install_method_inactive
                    },
                ),
            )
        },
        supportingContent = {
            Text(
                when (option) {
                    InstallMethod.FILE -> state.vendorBootName ?: stringResource(R.string.install_pick_vendor_boot_desc)
                    InstallMethod.DIRECT -> state.directTarget ?: stringResource(R.string.install_method_direct_desc)
                    InstallMethod.INACTIVE -> state.inactiveTarget ?: stringResource(R.string.install_method_inactive_desc)
                },
            )
        },
        trailingContent = {
            Icon(
                imageVector =
                    when (option) {
                        InstallMethod.FILE -> Icons.Filled.InsertDriveFile
                        else -> Icons.Filled.Smartphone
                    },
                contentDescription = null,
            )
        },
    )
}

@Composable
private fun KernelModuleSection(
    state: InstallUiState,
    onSelectKmi: () -> Unit,
    onPickLocal: () -> Unit,
    onClearLocal: () -> Unit,
) {
    val kmis = state.koCandidates.map { KernelInfo.kmiOf(it) }
    val local = state.lkmSelection as? LkmSelection.Local
    val selectedKmi = (state.lkmSelection as? LkmSelection.Kmi)?.kmi ?: state.autoKo?.let { KernelInfo.kmiOf(it) }

    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        SectionTitle(stringResource(R.string.install_select_ko))
        CardGroup {
            val total = if (kmis.isNotEmpty()) 2 else 1
            var index = 0

            if (kmis.isNotEmpty()) {
                CardItem(index = index++, total = total) {
                    ListItem(
                        modifier =
                            Modifier
                                .fillMaxWidth()
                                .clickable(enabled = !state.running) { onSelectKmi() },
                        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                        leadingContent = {
                            Icon(Icons.Filled.Memory, contentDescription = null)
                        },
                        headlineContent = {
                            Text(stringResource(R.string.install_select_kmi))
                        },
                        supportingContent = {
                            val text =
                                when {
                                    state.lkmSelection is LkmSelection.Kmi -> selectedKmi.orEmpty()
                                    selectedKmi != null -> stringResource(R.string.install_auto_lkm, selectedKmi)
                                    else -> stringResource(R.string.install_ko_missing)
                                }
                            val tint =
                                if (state.lkmSelection is LkmSelection.Auto && selectedKmi != null) {
                                    MaterialTheme.colorScheme.primary
                                } else {
                                    MaterialTheme.colorScheme.onSurfaceVariant
                                }
                            Text(text = text, color = tint)
                        },
                        trailingContent = {
                            Icon(
                                Icons.AutoMirrored.Filled.KeyboardArrowRight,
                                contentDescription = null,
                                tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f),
                            )
                        },
                    )
                }
            }

            CardItem(index = index, total = total) {
                ListItem(
                    modifier =
                        Modifier
                            .fillMaxWidth()
                            .clickable(enabled = !state.running) { onPickLocal() },
                    colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                    leadingContent = {
                        Icon(Icons.Filled.InsertDriveFile, contentDescription = null)
                    },
                    headlineContent = {
                        Text(stringResource(R.string.install_use_local_lkm))
                    },
                    supportingContent = {
                        if (local != null) {
                            Text(stringResource(R.string.install_selected_lkm, local.name))
                        }
                    },
                    trailingContent = {
                        if (local != null) {
                            IconButton(onClick = onClearLocal) {
                                Icon(
                                    Icons.Filled.Close,
                                    contentDescription = stringResource(R.string.dialog_cancel),
                                )
                            }
                        } else {
                            Icon(
                                Icons.AutoMirrored.Filled.KeyboardArrowRight,
                                contentDescription = null,
                                tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f),
                            )
                        }
                    },
                )
            }
        }
    }
}

@Composable
private fun ChooseKmiDialog(
    kmis: List<String>,
    currentKmi: String?,
    selected: String?,
    onDismiss: () -> Unit,
    onConfirm: (String) -> Unit,
) {
    if (kmis.isEmpty()) return
    var choice by remember { mutableStateOf(selected?.takeIf { it in kmis } ?: kmis.first()) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            Text(
                text = stringResource(R.string.install_select_kmi),
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold,
            )
        },
        text = {
            Column(
                modifier =
                    Modifier
                        .fillMaxWidth()
                        .heightIn(max = 380.dp)
                        .verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(2.dp),
            ) {
                kmis.forEach { kmi ->
                    Row(
                        modifier =
                            Modifier
                                .fillMaxWidth()
                                .clip(RoundedCornerShape(10.dp))
                                .clickable { choice = kmi }
                                .padding(horizontal = 4.dp, vertical = 8.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        RadioButton(selected = choice == kmi, onClick = null)
                        Column(modifier = Modifier.weight(1f)) {
                            Text(
                                text = kmi,
                                style = MaterialTheme.typography.bodyMedium,
                                fontWeight = FontWeight.SemiBold,
                            )
                            if (kmi == currentKmi) {
                                Text(
                                    text = stringResource(R.string.install_current_kmi),
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.primary,
                                )
                            }
                        }
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = { onConfirm(choice) }) {
                Text(stringResource(R.string.dialog_confirm), fontWeight = FontWeight.SemiBold)
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) {
                Text(stringResource(R.string.dialog_cancel))
            }
        },
    )
}

@Composable
private fun InstallStatusSection(state: InstallUiState) {
    val accent =
        when {
            state.running -> MaterialTheme.colorScheme.primary
            state.success == true -> MaterialTheme.colorScheme.primary
            else -> MaterialTheme.colorScheme.error
        }

    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(24.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
    ) {
        Column(
            modifier = Modifier.padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                when {
                    state.running -> CircularProgressIndicator(Modifier.size(22.dp), strokeWidth = 2.5.dp)
                    state.success == true -> Icon(Icons.Filled.CheckCircle, contentDescription = null, tint = accent)
                    else -> Icon(Icons.Filled.Error, contentDescription = null, tint = accent)
                }
                Spacer(Modifier.width(12.dp))
                Text(
                    text =
                        when {
                            state.running -> stringResource(R.string.install_working)
                            state.success == true ->
                                stringResource(
                                    when (state.method) {
                                        InstallMethod.FILE -> R.string.install_success
                                        InstallMethod.DIRECT -> R.string.install_direct_success
                                        InstallMethod.INACTIVE -> R.string.install_inactive_success
                                    },
                                )
                            else -> stringResource(R.string.install_failed)
                        },
                    style = MaterialTheme.typography.titleMedium,
                    color = accent,
                )
            }

            if (state.running) {
                LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
            }

            if (state.message.isNotBlank()) {
                Surface(
                    modifier = Modifier.fillMaxWidth().heightIn(max = 320.dp),
                    shape = RoundedCornerShape(16.dp),
                    color = MaterialTheme.colorScheme.surfaceContainerHighest,
                ) {
                    SelectionContainer {
                        Text(
                            text = state.message,
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
}

@Composable
private fun InstallBottomBar(
    state: InstallUiState,
    onInstall: () -> Unit,
    onDone: () -> Unit,
) {
    if (state.running) return
    Surface(color = MaterialTheme.colorScheme.surface) {
        if (state.done) {
            Button(
                onClick = onDone,
                modifier =
                    Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp, vertical = 12.dp),
            ) {
                Text(stringResource(R.string.install_done))
            }
        } else {
            Button(
                onClick = onInstall,
                enabled = state.canInstall,
                modifier =
                    Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp, vertical = 12.dp),
            ) {
                Text(stringResource(R.string.install_start))
            }
        }
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier.padding(start = 4.dp),
    )
}
