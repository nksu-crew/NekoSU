// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.screens.install

import android.app.Application
import android.widget.Toast
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.InsertDriveFile
import androidx.compose.material.icons.automirrored.rounded.KeyboardArrowRight
import androidx.compose.material.icons.rounded.Close
import androidx.compose.material.icons.rounded.Memory
import androidx.compose.material.icons.rounded.PhoneAndroid
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBarScrollBehavior
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
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavController
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import me.nekosu.aqnya.R
import me.nekosu.aqnya.core.model.InstallStrategy
import me.nekosu.aqnya.core.model.InstallTarget
import me.nekosu.aqnya.data.kernel.KernelInfo
import me.nekosu.aqnya.data.shell.RootShell
import me.nekosu.aqnya.ui.component.AppDialog
import me.nekosu.aqnya.ui.component.AppRadioButton
import me.nekosu.aqnya.ui.component.CardGroup
import me.nekosu.aqnya.ui.component.ListRow
import me.nekosu.aqnya.ui.component.LogDoneFab
import me.nekosu.aqnya.ui.component.LogPane
import me.nekosu.aqnya.ui.component.LogTopBar
import me.nekosu.aqnya.ui.component.Page
import me.nekosu.aqnya.ui.component.PageSection
import me.nekosu.aqnya.ui.theme.ContinuousCornerShape
import me.nekosu.aqnya.util.LogUtils

/**
 * 安装执行页。
 *
 * 模式（[target] + [strategy]）由 ChooseInstallScreen 选定并作为导航参数传入；
 * 本页只展示该模式的输入项、LKM 选择与安装按钮，安装过程切换为整屏日志。
 */
@Composable
fun InstallScreen(
    navController: NavController,
    target: InstallTarget,
    strategy: InstallStrategy,
) {
    val context = LocalContext.current
    val installViewModel: InstallViewModel =
        viewModel(factory = InstallViewModelFactory(context.applicationContext as Application, target, strategy))

    val state by installViewModel.uiState.collectAsState()
    val scope = rememberCoroutineScope()
    val leavePage: () -> Unit = { navController.popBackStack() }

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

    val pickBoot =
        rememberLauncherForActivityResult(
            contract = ActivityResultContracts.OpenDocument(),
        ) { uri ->
            if (uri != null) installViewModel.selectBoot(uri)
        }

    val pickTargetBoot =
        rememberLauncherForActivityResult(
            contract = ActivityResultContracts.OpenDocument(),
        ) { uri ->
            if (uri != null) installViewModel.selectTargetBoot(uri)
        }

    LaunchedEffect(Unit) { installViewModel.load() }
    LaunchedEffect(target, strategy) { installViewModel.setMode(target, strategy) }

    // 安装过程中屏蔽系统返回，避免中途离开导致协程被取消。
    BackHandler(enabled = state.running) { }

    LaunchedEffect(state.done) {
        if (!state.done) return@LaunchedEffect
        val message =
            if (state.success == true) {
                context.getString(
                    when {
                        state.strategy == InstallStrategy.IMAGE && state.target == InstallTarget.VENDOR_BOOT ->
                            R.string.install_success
                        state.strategy == InstallStrategy.IMAGE -> R.string.install_boot_success
                        state.strategy == InstallStrategy.DIRECT && state.target == InstallTarget.VENDOR_BOOT ->
                            R.string.install_direct_success
                        state.strategy == InstallStrategy.DIRECT -> R.string.install_initboot_direct_success
                        state.target == InstallTarget.VENDOR_BOOT -> R.string.install_inactive_success
                        else -> R.string.install_initboot_inactive_success
                    },
                )
            } else {
                context.getString(R.string.install_failed)
            }
        Toast.makeText(context, message, Toast.LENGTH_SHORT).show()
    }

    var confirmInstall by remember { mutableStateOf(false) }
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

    if (confirmInstall) {
        ConfirmInstallDialog(
            state = state,
            onDismiss = { confirmInstall = false },
            onConfirm = {
                confirmInstall = false
                installViewModel.install()
            },
        )
    }

    val logVisible = state.running || state.done
    val customTopBar: (@Composable (TopAppBarScrollBehavior) -> Unit)? =
        if (logVisible) {
            {
                LogTopBar(
                    title = stringResource(R.string.install_log_title),
                    onBack = if (state.running) null else leavePage,
                    onSaveLog = { LogUtils.saveLog(context, "nksu-install.log", state.message) },
                )
            }
        } else {
            null
        }

    Page(
        title = stringResource(if (logVisible) R.string.install_log_title else R.string.install_title),
        onBack = if (logVisible && state.running) null else leavePage,
        topBar = customTopBar,
        scrollable = !logVisible,
        floatingActionButton = {
            when {
                state.done && state.success == true && state.strategy != InstallStrategy.IMAGE -> {
                    LogDoneFab(
                        label = stringResource(R.string.reboot),
                        onClick = { scope.launch(Dispatchers.IO) { RootShell.reboot() } },
                    )
                }

                state.done -> {
                    LogDoneFab(
                        label = stringResource(R.string.install_done),
                        onClick = leavePage,
                    )
                }
            }
        },
        bottomBar = {
            if (!logVisible) {
                InstallBottomBar(
                    canInstall = state.canInstall,
                    onInstall = {
                        when (state.strategy) {
                            InstallStrategy.IMAGE -> installViewModel.install()
                            InstallStrategy.DIRECT, InstallStrategy.INACTIVE -> confirmInstall = true
                        }
                    },
                )
            }
        },
        contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 24.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        if (logVisible) {
            Box(modifier = Modifier.fillMaxSize()) {
                LogPane(text = state.message, running = state.running)
            }
        } else {
            ModeSummaryCard(state = state, onChange = leavePage)
            InstallInputSection(
                state = state,
                onPickVendorBoot = { pickVendorBoot.launch(arrayOf("*/*")) },
                onPickTargetBoot = { pickTargetBoot.launch(arrayOf("*/*")) },
                onPickBoot = { pickBoot.launch(arrayOf("*/*")) },
            )
            modeIssue(state)?.let { issue ->
                Text(
                    text = issue,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.error,
                )
            }
            KernelModuleSection(
                state = state,
                onSelectKmi = { showKmiDialog = true },
                onPickLocal = { pickLocalKo.launch(arrayOf("*/*")) },
                onClearLocal = installViewModel::clearLocalKo,
            )
            if (state.kernelLoaded && !state.hasKernelModule) {
                Text(
                    text = stringResource(R.string.install_ko_missing),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.error,
                )
            }
        }
    }
}

/** 当前模式的摘要，右侧「更改」返回模式选择页。 */
@Composable
private fun ModeSummaryCard(
    state: InstallUiState,
    onChange: () -> Unit,
) {
    PageSection(title = stringResource(R.string.install_selected_mode)) {
        CardGroup {
            item {
                ListRow(
                    icon = { Icon(strategyIcon(state.strategy), contentDescription = null) },
                    headline = { Text(strategyTitle(state.target, state.strategy)) },
                    supporting = { Text(strategyDescription(state.target, state.strategy)) },
                    trailing = {
                        Text(
                            text = stringResource(R.string.install_change_mode),
                            style = MaterialTheme.typography.labelLarge,
                            color = MaterialTheme.colorScheme.primary,
                        )
                    },
                    onClick = onChange,
                )
            }
        }
    }
}

/** 模式需要的输入：镜像选择，或直接安装的目标分区。 */
@Composable
private fun InstallInputSection(
    state: InstallUiState,
    onPickVendorBoot: () -> Unit,
    onPickTargetBoot: () -> Unit,
    onPickBoot: () -> Unit,
) {
    when {
        state.strategy == InstallStrategy.IMAGE && state.target == InstallTarget.VENDOR_BOOT -> {
            PageSection(title = stringResource(R.string.install_source_image)) {
                CardGroup {
                    item {
                        PickerRow(
                            icon = Icons.AutoMirrored.Rounded.InsertDriveFile,
                            title = stringResource(R.string.install_pick_vendor_boot),
                            summary = state.vendorBootName ?: stringResource(R.string.install_pick_vendor_boot_desc),
                            onClick = onPickVendorBoot,
                        )
                    }
                    // boot 镜像只在内核符号校验开启时才需要。
                    item(visible = state.verifyKernelSymbols) {
                        PickerRow(
                            icon = Icons.AutoMirrored.Rounded.InsertDriveFile,
                            title = stringResource(R.string.install_boot_image),
                            summary = state.bootName ?: stringResource(R.string.install_pick_boot_desc),
                            onClick = onPickBoot,
                        )
                    }
                }
            }
        }

        state.strategy == InstallStrategy.IMAGE && state.target == InstallTarget.INIT_BOOT -> {
            PageSection(title = stringResource(R.string.install_source_image)) {
                CardGroup {
                    item {
                        PickerRow(
                            icon = Icons.AutoMirrored.Rounded.InsertDriveFile,
                            title = stringResource(R.string.install_pick_target_boot),
                            summary = state.targetBootName ?: stringResource(R.string.install_pick_target_boot_desc),
                            onClick = onPickTargetBoot,
                        )
                    }
                }
            }
        }

        else -> {
            PageSection(title = stringResource(R.string.install_target_partition)) {
                CardGroup {
                    item {
                        ListRow(
                            icon = { Icon(Icons.Rounded.PhoneAndroid, contentDescription = null) },
                            headline = { Text(state.target.partitionName) },
                            supporting = {
                                Text(state.selectedPartition ?: stringResource(R.string.install_partition_missing))
                            },
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun PickerRow(
    icon: ImageVector,
    title: String,
    summary: String,
    onClick: () -> Unit,
) {
    ListRow(
        icon = { Icon(icon, contentDescription = null) },
        headline = { Text(title) },
        supporting = { Text(summary) },
        trailing = {
            Icon(
                imageVector = Icons.AutoMirrored.Rounded.KeyboardArrowRight,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f),
            )
        },
        onClick = onClick,
    )
}

/** 直接安装缺 root / 分区时的明确提示；镜像模式没有额外前置条件。 */
@Composable
private fun modeIssue(state: InstallUiState): String? =
    when {
        state.strategy == InstallStrategy.IMAGE -> null
        !state.rootKnown -> null
        !state.rootAvailable -> stringResource(R.string.install_requires_root_desc)
        state.strategy == InstallStrategy.INACTIVE && state.kernelLoaded && !state.isAbDevice ->
            stringResource(R.string.install_not_ab)
        state.selectedPartition == null -> stringResource(R.string.install_partition_missing)
        state.target == InstallTarget.VENDOR_BOOT &&
            state.verifyKernelSymbols &&
            state.selectedBootPartition == null -> stringResource(R.string.install_boot_partition_missing)
        else -> null
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
    val localSummary: (@Composable () -> Unit)? =
        if (local != null) {
            { Text(stringResource(R.string.install_selected_lkm, local.name)) }
        } else {
            null
        }

    PageSection(title = stringResource(R.string.install_select_ko)) {
        CardGroup {
            if (kmis.isNotEmpty()) {
                item {
                    ListRow(
                        icon = { Icon(Icons.Rounded.Memory, contentDescription = null) },
                        headline = { Text(stringResource(R.string.install_select_kmi)) },
                        supporting = {
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
                        trailing = {
                            Icon(
                                Icons.AutoMirrored.Rounded.KeyboardArrowRight,
                                contentDescription = null,
                                tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.4f),
                            )
                        },
                        onClick = onSelectKmi,
                    )
                }
            }

            item {
                ListRow(
                    icon = { Icon(Icons.AutoMirrored.Rounded.InsertDriveFile, contentDescription = null) },
                    headline = { Text(stringResource(R.string.install_use_local_lkm)) },
                    supporting = localSummary,
                    trailing = {
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
                    onClick = onPickLocal,
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
private fun ConfirmInstallDialog(
    state: InstallUiState,
    onDismiss: () -> Unit,
    onConfirm: () -> Unit,
) {
    val inactive = state.strategy == InstallStrategy.INACTIVE
    val titleRes =
        when {
            inactive && state.target == InstallTarget.VENDOR_BOOT -> R.string.install_inactive_confirm_title
            inactive -> R.string.install_inactive_initboot_confirm_title
            state.target == InstallTarget.VENDOR_BOOT -> R.string.install_direct_confirm_title
            else -> R.string.install_direct_initboot_confirm_title
        }
    val messageRes =
        when {
            inactive && state.target == InstallTarget.VENDOR_BOOT -> R.string.install_inactive_confirm_message
            inactive -> R.string.install_inactive_initboot_confirm_message
            state.target == InstallTarget.VENDOR_BOOT -> R.string.install_direct_confirm_message
            else -> R.string.install_direct_initboot_confirm_message
        }

    AppDialog(
        onDismissRequest = onDismiss,
        title = stringResource(titleRes),
        confirmText = stringResource(R.string.install_direct_confirm_ok),
        onConfirm = onConfirm,
        dismissText = stringResource(R.string.dialog_cancel),
    ) {
        Text(stringResource(messageRes, state.selectedPartition.orEmpty()))
    }
}

@Composable
private fun InstallBottomBar(
    canInstall: Boolean,
    onInstall: () -> Unit,
) {
    Surface(
        color = MaterialTheme.colorScheme.surfaceContainer,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Button(
            onClick = onInstall,
            enabled = canInstall,
            modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp),
        ) {
            Text(stringResource(R.string.install_start))
        }
    }
}
