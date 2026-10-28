// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.screens.settings

import androidx.compose.foundation.clickable
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.BugReport
import androidx.compose.material.icons.rounded.Security
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import me.nekosu.aqnya.R
import me.nekosu.aqnya.ui.component.AppSwitch
import me.nekosu.aqnya.ui.component.CardGroup
import me.nekosu.aqnya.ui.component.ListRow

@Composable
fun ToolsSection(
    onExportLog: () -> Unit,
    verifyKernelSymbols: Boolean,
    onVerifyKernelSymbolsChange: (Boolean) -> Unit,
) {
    CardGroup {
        item {
            ListRow(
                modifier =
                    Modifier.toggleable(
                        value = verifyKernelSymbols,
                        role = Role.Switch,
                        onValueChange = onVerifyKernelSymbolsChange,
                    ),
                icon = { Icon(Icons.Rounded.Security, contentDescription = null) },
                headline = {
                    Text(
                        stringResource(R.string.settings_verify_kernel_symbols),
                    )
                },
                supporting = { Text(stringResource(R.string.settings_verify_kernel_symbols_summary)) },
                trailing = {
                    AppSwitch(
                        checked = verifyKernelSymbols,
                        onCheckedChange = onVerifyKernelSymbolsChange,
                    )
                },
            )
        }

        item {
            ListRow(
                modifier = Modifier.clickable { onExportLog() },
                icon = { Icon(Icons.Rounded.BugReport, contentDescription = null) },
                headline = {
                    Text(
                        stringResource(R.string.export_log),
                    )
                },
                supporting = { Text(stringResource(R.string.export_log_describe)) },
            )
        }
    }
}
