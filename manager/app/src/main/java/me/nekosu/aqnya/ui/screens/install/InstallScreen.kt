package me.nekosu.aqnya.ui.screens.install

import android.app.Application
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
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
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material.icons.automirrored.rounded.InsertDriveFile
import androidx.compose.material.icons.automirrored.rounded.KeyboardArrowRight
import androidx.compose.material.icons.rounded.Close
import androidx.compose.material.icons.rounded.Memory
import androidx.compose.material.icons.rounded.Smartphone
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SegmentedListItem
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.rememberTopAppBarState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavController
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import me.nekosu.aqnya.R
import me.nekosu.aqnya.data.kernel.KernelInfo
import me.nekosu.aqnya.data.shell.RootShell
import me.nekosu.aqnya.ui.component.AppDialog
import me.nekosu.aqnya.ui.component.AppRadioButton
import me.nekosu.aqnya.ui.component.AppTopAppBar
import me.nekosu.aqnya.ui.component.CardGroup
import me.nekosu.aqnya.ui.component.LogDoneFab
import me.nekosu.aqnya.ui.component.LogPane
import me.nekosu.aqnya.ui.component.LogTopBar
import me.nekosu.aqnya.ui.component.currentListItemShapes
import me.nekosu.aqnya.ui.theme.ContinuousCornerShape
import me.nekosu.aqnya.util.LogUtils

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun InstallScreen(navController: NavController) {
    val context = LocalContext.current
    val installViewModel: InstallViewModel =
        viewModel(factory = InstallViewModelFactory(context.applicationContext as Application))

    val state by installViewModel.uiState.collectAsState()
    val scrollBehavior = TopAppBarDefaults.pinnedScrollBehavior(rememberTopAppBarState())
    val scope = rememberCoroutineScope()

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

    LaunchedEffect(state.done) {
        if (!state.done) return@LaunchedEffect
        val message =
            if (state.success == true) {
                context.getString(
                    when (state.method) {
                        InstallMethod.FILE -> R.string.install_success
                        InstallMethod.DIRECT -> R.string.install_direct_success
                        InstallMethod.INACTIVE -> R.string.install_inactive_success
                    },
                )
            } else {
                context.getString(R.string.install_failed)
            }
        Toast.makeText(context, message, Toast.LENGTH_SHORT).show()
    }

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
        AppDialog(
            onDismissRequest = { confirmMethod = null },
            title =
                stringResource(
                    if (inactive) R.string.install_inactive_confirm_title else R.string.install_direct_confirm_title,
                ),
            confirmText = stringResource(R.string.install_direct_confirm_ok),
            onConfirm = {
                confirmMethod = null
                installViewModel.install()
            },
            dismissText = stringResource(R.string.dialog_cancel),
        ) {
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
        }
    }

    val logVisible = state.running || state.done

    Scaffold(
        containerColor = MaterialTheme.colorScheme.surfaceContainer,
        modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        topBar = {
            if (logVisible) {
                LogTopBar(
                    title = stringResource(R.string.install_log_title),
                    onBack = if (state.running) null else ({ navController.popBackStack() }),
                    onSaveLog = { LogUtils.saveLog(context, "nksu-install.log", state.message) },
                )
            } else {
                AppTopAppBar(
                    title = stringResource(R.string.install_title),
                    navigationIcon = {
                        IconButton(onClick = { navController.popBackStack() }) {
                            Icon(
                                Icons.AutoMirrored.Rounded.ArrowBack,
                                contentDescription = stringResource(R.string.cd_back),
                            )
                        }
                    },
                    scrollBehavior = scrollBehavior,
                    colors =
                        TopAppBarDefaults.topAppBarColors(
                            containerColor = MaterialTheme.colorScheme.surfaceContainer,
                            scrolledContainerColor = MaterialTheme.colorScheme.surfaceContainer,
                        ),
                )
            }
        },
        floatingActionButton = {
            when {
                state.done && state.success == true && state.method != InstallMethod.FILE -> {
                    LogDoneFab(
                        label = stringResource(R.string.reboot),
                        onClick = { scope.launch(Dispatchers.IO) { RootShell.reboot() } },
                    )
                }

                state.done -> {
                    LogDoneFab(
                        label = stringResource(R.string.install_done),
                        onClick = { navController.popBackStack() },
                    )
                }
            }
        },
        contentWindowInsets =
            WindowInsets.safeDrawing.only(
                WindowInsetsSides.Top + WindowInsetsSides.Horizontal,
            ),
    ) { innerPadding ->
        if (logVisible) {
            Box(modifier = Modifier.fillMaxSize().padding(innerPadding)) {
                LogPane(text = state.message, running = state.running)
            }
        } else {
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
                InstallMethodSection(
                    state = state,
                    onPick = { pickVendorBoot.launch(arrayOf("*/*")) },
                    onSelectMethod = installViewModel::selectMethod,
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
            }
        }
    }
}

@Composable
private fun InstallMethodSection(
    state: InstallUiState,
    onPick: () -> Unit,
    onSelectMethod: (InstallMethod) -> Unit,
) {
    val options =
        buildList {
            add(InstallMethod.FILE)
            if (state.rootAvailable) {
                if (state.directTarget != null) add(InstallMethod.DIRECT)
                if (state.inactiveTarget != null) add(InstallMethod.INACTIVE)
            }
        }

    CardGroup {
        options.forEach { option ->
            item {
                InstallMethodRow(
                    state = state,
                    option = option,
                    onPick = onPick,
                    onSelectMethod = onSelectMethod,
                )
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
    SegmentedListItem(
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
        shapes = currentListItemShapes(),
        colors = ListItemDefaults.segmentedColors(containerColor = MaterialTheme.colorScheme.surfaceBright),
        leadingContent = {
            AppRadioButton(selected = selected, onClick = { onSelectMethod(option) })
        },
        content = {
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
                        InstallMethod.FILE -> Icons.AutoMirrored.Rounded.InsertDriveFile
                        else -> Icons.Rounded.Smartphone
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
        CardGroup {
            if (kmis.isNotEmpty()) {
                item {
                    SegmentedListItem(
                        modifier =
                            Modifier
                                .fillMaxWidth()
                                .clickable(enabled = !state.running) { onSelectKmi() },
                        shapes = currentListItemShapes(),
                        colors = ListItemDefaults.segmentedColors(containerColor = MaterialTheme.colorScheme.surfaceBright),
                        leadingContent = {
                            Icon(Icons.Rounded.Memory, contentDescription = null)
                        },
                        content = {
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
                                Icons.AutoMirrored.Rounded.KeyboardArrowRight,
                                contentDescription = null,
                                tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f),
                            )
                        },
                    )
                }
            }

            item {
                SegmentedListItem(
                    modifier =
                        Modifier
                            .fillMaxWidth()
                            .clickable(enabled = !state.running) { onPickLocal() },
                    shapes = currentListItemShapes(),
                    colors = ListItemDefaults.segmentedColors(containerColor = MaterialTheme.colorScheme.surfaceBright),
                    leadingContent = {
                        Icon(Icons.AutoMirrored.Rounded.InsertDriveFile, contentDescription = null)
                    },
                    content = {
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
                                    Icons.Rounded.Close,
                                    contentDescription = stringResource(R.string.dialog_cancel),
                                )
                            }
                        } else {
                            Icon(
                                Icons.AutoMirrored.Rounded.KeyboardArrowRight,
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

    AppDialog(
        onDismissRequest = onDismiss,
        title = stringResource(R.string.install_select_kmi),
        confirmText = stringResource(R.string.dialog_confirm),
        onConfirm = { onConfirm(choice) },
        dismissText = stringResource(R.string.dialog_cancel),
    ) {
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
                            .clip(ContinuousCornerShape(12.dp))
                            .clickable { choice = kmi }
                            .padding(horizontal = 4.dp, vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    AppRadioButton(selected = choice == kmi, onClick = null)
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
    }
}

@Composable
private fun InstallBottomBar(
    state: InstallUiState,
    onInstall: () -> Unit,
    onDone: () -> Unit,
) {
    if (state.running) return
    Surface(color = MaterialTheme.colorScheme.surfaceContainer) {
        if (state.done) {
            Button(
                onClick = onDone,
                modifier =
                    Modifier
                        .fillMaxWidth()
                        .padding(vertical = 12.dp),
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
                        .padding(vertical = 12.dp),
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
