// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.screens.install

import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.InsertDriveFile
import androidx.compose.material.icons.rounded.PhoneAndroid
import androidx.compose.material.icons.rounded.SwapHoriz
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import me.nekosu.aqnya.R
import me.nekosu.aqnya.core.model.InstallStrategy
import me.nekosu.aqnya.core.model.InstallTarget

/** 安装模式的图标 / 标题 / 说明，供选择页与安装页共用。 */
internal fun strategyIcon(strategy: InstallStrategy): ImageVector =
    when (strategy) {
        InstallStrategy.IMAGE -> Icons.AutoMirrored.Rounded.InsertDriveFile
        InstallStrategy.DIRECT -> Icons.Rounded.PhoneAndroid
        InstallStrategy.INACTIVE -> Icons.Rounded.SwapHoriz
    }

@Composable
internal fun strategyTitle(
    target: InstallTarget,
    strategy: InstallStrategy,
): String =
    when (strategy) {
        InstallStrategy.IMAGE ->
            stringResource(
                if (target == InstallTarget.VENDOR_BOOT) R.string.install_method_file else R.string.install_method_boot,
            )
        InstallStrategy.DIRECT -> stringResource(R.string.install_method_direct)
        InstallStrategy.INACTIVE -> stringResource(R.string.install_method_inactive)
    }

@Composable
internal fun strategyDescription(
    target: InstallTarget,
    strategy: InstallStrategy,
): String =
    when (strategy) {
        InstallStrategy.IMAGE ->
            stringResource(
                if (target == InstallTarget.VENDOR_BOOT) {
                    R.string.install_strategy_image_vendor_desc
                } else {
                    R.string.install_strategy_image_init_desc
                },
            )
        InstallStrategy.DIRECT -> stringResource(R.string.install_method_direct_desc, target.partitionName)
        InstallStrategy.INACTIVE -> stringResource(R.string.install_method_inactive_desc, target.partitionName)
    }
