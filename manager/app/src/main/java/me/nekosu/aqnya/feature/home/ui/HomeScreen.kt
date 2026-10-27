// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.feature.home.ui

import android.os.Build
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.KeyboardArrowRight
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SegmentedListItem
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import dev.chrisbanes.haze.hazeSource
import dev.chrisbanes.haze.rememberHazeState
import me.nekosu.aqnya.R
import me.nekosu.aqnya.core.model.InstallStatus
import me.nekosu.aqnya.feature.home.presentation.HomeViewModel
import me.nekosu.aqnya.ui.component.HazeTopAppBar
import me.nekosu.aqnya.ui.component.StatusCard
import me.nekosu.aqnya.ui.navigation.LocalBottomBarClearance

private data class InfoEntry(
    val label: String,
    val value: String,
    val monospace: Boolean = false,
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HomeScreenContent(
    installStatus: InstallStatus,
    isGki: Boolean,
    managerVersion: String,
    onInstallClick: () -> Unit,
    onAboutClick: () -> Unit = {},
) {
    val hazeState = rememberHazeState()
    Box(modifier = Modifier.fillMaxSize()) {
        Scaffold(
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
            topBar = {
                HazeTopAppBar(
                    state = hazeState,
                    title = stringResource(R.string.app_name),
                )
            },
        ) { innerPadding ->
            Column(
                modifier =
                    Modifier
                        .fillMaxSize()
                        .hazeSource(hazeState)
                        .verticalScroll(rememberScrollState())
                        .padding(innerPadding)
                        .padding(horizontal = 16.dp, vertical = 12.dp)
                        .padding(bottom = LocalBottomBarClearance.current),
                verticalArrangement = Arrangement.spacedBy(13.dp),
            ) {
                StatusCard(
                    state = installStatus,
                    isGki = isGki,
                    managerVersion = managerVersion,
                    onClick = onInstallClick,
                )

                SectionLabel(stringResource(R.string.install_device_info))
                DeviceInfoList()

                SectionLabel(stringResource(R.string.about))
                AboutItem(onClick = onAboutClick)
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HomeScreen(
    viewModel: HomeViewModel,
    onAboutClick: () -> Unit = {},
    onInstallClick: () -> Unit = {},
) {
    val state by viewModel.uiState.collectAsState()

    LaunchedEffect(state.installStatus) {
        if (state.installStatus == InstallStatus.INSTALLED) {
            viewModel.refresh()
        }
    }

    HomeScreenContent(
        installStatus = state.installStatus,
        isGki = state.isGki,
        managerVersion = state.managerVersion,
        onInstallClick = onInstallClick,
        onAboutClick = onAboutClick,
    )
}

@Composable
private fun SectionLabel(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(start = 16.dp, top = 4.dp),
    )
}

@Composable
private fun DeviceInfoList(modifier: Modifier = Modifier) {
    val entries =
        listOf(
            InfoEntry(
                label = stringResource(id = R.string.kernel_version),
                value = System.getProperty("os.version") ?: "Unavailable",
            ),
            InfoEntry(
                label = stringResource(id = R.string.android_version),
                value = Build.VERSION.RELEASE,
            ),
            InfoEntry(
                label = stringResource(id = R.string.device_model),
                value = "${Build.MANUFACTURER} ${Build.MODEL}",
            ),
            InfoEntry(
                label = stringResource(id = R.string.finger_print),
                value = Build.FINGERPRINT,
                monospace = true,
            ),
        )

    Column(
        modifier = modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(ListItemDefaults.SegmentedGap),
    ) {
        entries.forEachIndexed { index, entry ->
            SegmentedListItem(
                shapes = ListItemDefaults.segmentedShapes(index = index, count = entries.size),
                colors =
                    ListItemDefaults.segmentedColors(
                        containerColor = MaterialTheme.colorScheme.surfaceBright,
                    ),
                content = {
                    Text(
                        text = entry.label,
                        style = MaterialTheme.typography.bodyLarge,
                    )
                },
                supportingContent = {
                    Text(
                        text = entry.value,
                        style =
                            if (entry.monospace) {
                                MaterialTheme.typography.bodyMedium.copy(fontFamily = FontFamily.Monospace)
                            } else {
                                MaterialTheme.typography.bodyMedium
                            },
                    )
                },
            )
        }
    }
}

@Composable
private fun AboutItem(
    modifier: Modifier = Modifier,
    onClick: () -> Unit = {},
) {
    SegmentedListItem(
        onClick = onClick,
        modifier = modifier.fillMaxWidth(),
        shapes = ListItemDefaults.segmentedShapes(index = 0, count = 1),
        colors =
            ListItemDefaults.segmentedColors(
                containerColor = MaterialTheme.colorScheme.surfaceBright,
            ),
        content = {
            Text(
                text = stringResource(R.string.about_card_title),
                style = MaterialTheme.typography.titleMedium,
            )
        },
        supportingContent = {
            Text(
                text = stringResource(R.string.about_card_description),
                maxLines = 2,
            )
        },
        trailingContent = {
            Icon(
                imageVector = Icons.AutoMirrored.Rounded.KeyboardArrowRight,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        },
    )
}

@Preview(showBackground = true, name = "Installed")
@Composable
fun HomeScreenPreviewInstalled() {
    MaterialTheme {
        HomeScreenContent(
            installStatus = InstallStatus.INSTALLED,
            isGki = true,
            managerVersion = "1.0.0",
            onInstallClick = {},
            onAboutClick = {},
        )
    }
}

@Preview(showBackground = true, name = "Not Install")
@Composable
fun HomeScreenPreviewNotInstalled() {
    MaterialTheme {
        HomeScreenContent(
            installStatus = InstallStatus.NOT_INSTALLED,
            isGki = false,
            managerVersion = "1.0.0",
            onInstallClick = {},
            onAboutClick = {},
        )
    }
}
