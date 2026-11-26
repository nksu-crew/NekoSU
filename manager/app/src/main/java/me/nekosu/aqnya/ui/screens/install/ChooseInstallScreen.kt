// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.screens.install

import android.app.Application
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.KeyboardArrowRight
import androidx.compose.material.icons.rounded.Lock
import androidx.compose.material.icons.rounded.Memory
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavController
import me.nekosu.aqnya.R
import me.nekosu.aqnya.core.model.InstallStrategy
import me.nekosu.aqnya.core.model.InstallTarget
import me.nekosu.aqnya.core.navigation.AppRoute
import me.nekosu.aqnya.ui.component.CardGroup
import me.nekosu.aqnya.ui.component.CardItem
import me.nekosu.aqnya.ui.component.ListRow
import me.nekosu.aqnya.ui.component.ModeLabel
import me.nekosu.aqnya.ui.component.Page
import me.nekosu.aqnya.ui.component.PageSection
import me.nekosu.aqnya.ui.theme.ContinuousCornerShape

/**
 * 安装模式选择页。
 *
 * 按目标分区分成 GKI（vendor_boot）与非 GKI（init_boot）两组，每组提供镜像、
 * 直接安装、非活动 slot 三种策略。设备/内核信息与 root 状态集中在顶部卡片，
 * 不可用的策略保留在列表中并说明原因（而不是静默隐藏），选择后进入安装页。
 */
@Composable
fun ChooseInstallScreen(navController: NavController) {
    val context = LocalContext.current
    val installViewModel: InstallViewModel =
        viewModel(factory = InstallViewModelFactory(context.applicationContext as Application))
    val state by installViewModel.uiState.collectAsState()

    LaunchedEffect(Unit) { installViewModel.load() }

    Page(
        title = stringResource(R.string.install_title),
        onBack = { navController.popBackStack() },
        contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 32.dp),
        verticalArrangement = Arrangement.spacedBy(20.dp),
    ) {
        DeviceSummaryCard(state)

        InstallTargetSection(
            target = InstallTarget.VENDOR_BOOT,
            state = state,
            recommended = state.kernelLoaded && state.isGki,
            onSelect = { strategy -> navController.navigate(AppRoute.install(InstallTarget.VENDOR_BOOT, strategy)) },
        )

        InstallTargetSection(
            target = InstallTarget.INIT_BOOT,
            state = state,
            recommended = state.kernelLoaded && !state.isGki,
            onSelect = { strategy -> navController.navigate(AppRoute.install(InstallTarget.INIT_BOOT, strategy)) },
        )

        Text(
            text = stringResource(R.string.install_mode_hint),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(horizontal = 4.dp),
        )
    }
}

/** 顶部设备卡片：GKI / 非 GKI、内核与 KMI、root 状态。 */
@Composable
private fun DeviceSummaryCard(state: InstallUiState) {
    CardItem(
        shape = ContinuousCornerShape(28.dp),
        containerColor = MaterialTheme.colorScheme.surfaceBright,
    ) {
        Column(
            modifier = Modifier.fillMaxWidth().padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(14.dp),
            ) {
                Box(
                    modifier =
                        Modifier
                            .size(48.dp)
                            .background(MaterialTheme.colorScheme.primaryContainer, CircleShape),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(
                        imageVector = Icons.Rounded.Memory,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.onPrimaryContainer,
                    )
                }
                Column(
                    modifier = Modifier.weight(1f),
                    verticalArrangement = Arrangement.spacedBy(2.dp),
                ) {
                    Text(
                        text = stringResource(R.string.install_device_info),
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold,
                    )
                    Text(
                        text =
                            listOfNotNull(state.kernelVersion, state.currentKmi)
                                .filter { it.isNotBlank() }
                                .joinToString(" · ")
                                .ifBlank { "—" },
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                if (state.kernelLoaded) {
                    ModeLabel(
                        label =
                            stringResource(
                                if (state.isGki) R.string.install_mode_gki else R.string.install_mode_non_gki,
                            ),
                        color = MaterialTheme.colorScheme.onPrimaryContainer,
                        containerColor = MaterialTheme.colorScheme.primaryContainer,
                    )
                }
            }

            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)

            Row(horizontalArrangement = Arrangement.spacedBy(28.dp)) {
                SummaryStat(
                    label = stringResource(R.string.install_current_kmi),
                    value = state.currentKmi ?: "—",
                    modifier = Modifier.weight(1f),
                )
                SummaryStat(
                    label = stringResource(R.string.install_root),
                    value =
                        when {
                            !state.rootKnown -> stringResource(R.string.install_checking_root)
                            state.rootAvailable -> stringResource(R.string.install_root_available)
                            else -> stringResource(R.string.install_root_missing)
                        },
                    highlighted = state.rootAvailable,
                    modifier = Modifier.weight(1f),
                )
            }
        }
    }
}

@Composable
private fun SummaryStat(
    label: String,
    value: String,
    modifier: Modifier = Modifier,
    highlighted: Boolean = false,
) {
    Column(modifier = modifier, verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = FontWeight.SemiBold,
            color = if (highlighted) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface,
        )
    }
}

@Composable
private fun InstallTargetSection(
    target: InstallTarget,
    state: InstallUiState,
    recommended: Boolean,
    onSelect: (InstallStrategy) -> Unit,
) {
    val recommendedLabel: (@Composable () -> Unit)? =
        if (recommended) {
            {
                ModeLabel(
                    label = stringResource(R.string.install_recommended),
                    color = MaterialTheme.colorScheme.onPrimaryContainer,
                    containerColor = MaterialTheme.colorScheme.primaryContainer,
                )
            }
        } else {
            null
        }

    PageSection(
        title =
            stringResource(
                if (target == InstallTarget.VENDOR_BOOT) {
                    R.string.install_mode_vendor_boot
                } else {
                    R.string.install_mode_init_boot
                },
            ),
        trailing = recommendedLabel,
    ) {
        CardGroup {
            InstallStrategy.entries.forEach { strategy ->
                item {
                    InstallStrategyRow(
                        target = target,
                        strategy = strategy,
                        state = state,
                        onClick = { onSelect(strategy) },
                    )
                }
            }
        }
    }
}

@Composable
private fun InstallStrategyRow(
    target: InstallTarget,
    strategy: InstallStrategy,
    state: InstallUiState,
    onClick: () -> Unit,
) {
    val ready = state.isModeAvailable(target, strategy)
    val summary =
        when {
            !ready ->
                when {
                    !state.rootKnown -> stringResource(R.string.install_checking_root)
                    !state.rootAvailable -> stringResource(R.string.install_requires_root)
                    strategy == InstallStrategy.INACTIVE && state.kernelLoaded && !state.isAbDevice ->
                        stringResource(R.string.install_not_ab)
                    target == InstallTarget.VENDOR_BOOT &&
                        state.partitionFor(target, strategy) != null &&
                        state.verifyKernelSymbols &&
                        state.bootPartitionFor(target, strategy) == null ->
                        stringResource(R.string.install_boot_partition_missing)
                    else -> stringResource(R.string.install_partition_missing)
                }
            else -> strategyDescription(target, strategy)
        }

    ListRow(
        icon = { Icon(strategyIcon(strategy), contentDescription = null) },
        headline = { Text(strategyTitle(target, strategy)) },
        supporting = { Text(summary) },
        trailing = {
            Icon(
                imageVector = if (ready) Icons.AutoMirrored.Rounded.KeyboardArrowRight else Icons.Rounded.Lock,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = if (ready) 0.4f else 0.7f),
            )
        },
        onClick = if (ready) onClick else null,
        enabled = ready,
    )
}
