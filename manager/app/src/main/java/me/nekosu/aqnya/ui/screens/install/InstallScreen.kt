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
import androidx.compose.material.icons.filled.Android
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Error
import androidx.compose.material.icons.filled.InsertDriveFile
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.Numbers
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
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.rememberTopAppBarState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
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

    LaunchedEffect(Unit) { installViewModel.load() }

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
                onInstall = installViewModel::install,
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
                )
                KernelModuleSection(state = state, onSelect = installViewModel::selectKo)
                if (state.candidates.isEmpty()) {
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
) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        SectionTitle(stringResource(R.string.install_method))
        CardGroup {
            CardItem(index = 0, total = 1) {
                ListItem(
                    modifier =
                        Modifier
                            .fillMaxWidth()
                            .clickable(enabled = !state.running) { onPick() },
                    colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                    leadingContent = { RadioButton(selected = true, onClick = null) },
                    headlineContent = { Text(stringResource(R.string.install_method_file)) },
                    supportingContent = {
                        Text(state.vendorBootName ?: stringResource(R.string.install_pick_vendor_boot_desc))
                    },
                    trailingContent = {
                        Icon(Icons.Filled.InsertDriveFile, contentDescription = null)
                    },
                )
            }
        }
    }
}

@Composable
private fun KernelModuleSection(
    state: InstallUiState,
    onSelect: (String) -> Unit,
) {
    if (state.candidates.isEmpty()) return
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        SectionTitle(stringResource(R.string.install_select_ko))
        CardGroup {
            state.candidates.forEachIndexed { index, ko ->
                CardItem(index = index, total = state.candidates.size) {
                    val selected = ko == state.selectedKo
                    val recommended = ko == state.recommendedKo
                    ListItem(
                        modifier =
                            Modifier
                                .fillMaxWidth()
                                .clickable(enabled = !state.running) { onSelect(ko) },
                        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                        leadingContent = {
                            RadioButton(selected = selected, onClick = { onSelect(ko) })
                        },
                        headlineContent = {
                            Text(
                                text = KernelInfo.kmiOf(ko),
                                fontWeight = FontWeight.SemiBold,
                            )
                        },
                        supportingContent = {
                            Text(
                                text =
                                    if (recommended) {
                                        "${stringResource(R.string.install_recommended)} · $ko"
                                    } else {
                                        ko
                                    },
                                color =
                                    if (recommended) {
                                        MaterialTheme.colorScheme.primary
                                    } else {
                                        MaterialTheme.colorScheme.onSurfaceVariant
                                    },
                            )
                        },
                    )
                }
            }
        }
    }
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
                            state.success == true -> stringResource(R.string.install_success)
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
