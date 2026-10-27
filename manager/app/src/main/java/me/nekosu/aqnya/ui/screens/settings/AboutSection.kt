// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.screens.settings

import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Info
import androidx.compose.material3.Icon
import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import me.nekosu.aqnya.R
import me.nekosu.aqnya.ui.component.AppListItem
import me.nekosu.aqnya.ui.component.CardGroup

@Composable
fun AboutSection(onAboutClick: () -> Unit) {
    CardGroup {
        item {
            AppListItem(
                title = stringResource(R.string.about),
                leading = { Icon(Icons.Rounded.Info, contentDescription = null) },
                onClick = onAboutClick,
            )
        }
    }
}
