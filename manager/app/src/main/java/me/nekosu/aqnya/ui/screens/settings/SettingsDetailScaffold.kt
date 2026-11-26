// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.screens.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.runtime.Composable
import androidx.compose.ui.unit.dp
import me.nekosu.aqnya.ui.component.Page

/** Shared scaffold for the pushed settings pages: top bar with Back plus a scrollable body. */
@Composable
fun SettingsDetailScaffold(
    title: String,
    onBack: () -> Unit,
    content: @Composable ColumnScope.() -> Unit,
) {
    Page(
        title = title,
        onBack = onBack,
        contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 16.dp, bottom = 32.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
        content = content,
    )
}
