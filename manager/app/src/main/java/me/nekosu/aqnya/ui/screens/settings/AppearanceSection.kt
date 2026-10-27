// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.screens.settings

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.BlurOn
import androidx.compose.material.icons.rounded.Check
import androidx.compose.material.icons.rounded.DarkMode
import androidx.compose.material.icons.rounded.Palette
import androidx.compose.material.icons.rounded.PhoneAndroid
import androidx.compose.material.icons.rounded.ViewAgenda
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import me.nekosu.aqnya.R
import me.nekosu.aqnya.ui.component.AppListItem
import me.nekosu.aqnya.ui.component.AppSwitch
import me.nekosu.aqnya.ui.component.CardGroup
import me.nekosu.aqnya.ui.component.ListRow
import me.nekosu.aqnya.ui.screens.settings.enums.NavBarStyle
import me.nekosu.aqnya.ui.screens.settings.enums.ThemeColor
import me.nekosu.aqnya.ui.screens.settings.enums.ThemeMode
import me.nekosu.aqnya.ui.theme.ContinuousCornerShape

@Composable
fun AppearanceSection(
    currentThemeMode: ThemeMode,
    currentThemeColor: ThemeColor,
    amoledEnabled: Boolean,
    blurEnabled: Boolean,
    currentNavBarStyle: NavBarStyle,
    onThemeChange: (ThemeMode) -> Unit,
    onThemeColorChange: (ThemeColor) -> Unit,
    onAmoledChange: (Boolean) -> Unit,
    onBlurChange: (Boolean) -> Unit,
    onNavBarStyleChange: (NavBarStyle) -> Unit,
) {
    var themeMenuExpanded by remember { mutableStateOf(false) }
    var themeColorMenuExpanded by remember { mutableStateOf(false) }
    var navBarStyleMenuExpanded by remember { mutableStateOf(false) }

    CardGroup {
        item {
            AppListItem(
                title = stringResource(R.string.theme_title),
                summary = stringResource(currentThemeMode.titleRes),
                leading = { Icon(Icons.Rounded.DarkMode, contentDescription = null) },
                onClick = { themeMenuExpanded = true },
                trailing = {
                    Box {
                        DropdownMenu(
                            expanded = themeMenuExpanded,
                            onDismissRequest = { themeMenuExpanded = false },
                            shape = ContinuousCornerShape(16.dp),
                        ) {
                            ThemeMode.entries.forEach { mode ->
                                DropdownMenuItem(
                                    text = {
                                        Text(
                                            stringResource(mode.titleRes),
                                            fontWeight = if (currentThemeMode == mode) FontWeight.SemiBold else FontWeight.Normal,
                                        )
                                    },
                                    onClick = {
                                        themeMenuExpanded = false
                                        onThemeChange(mode)
                                    },
                                    trailingIcon = {
                                        if (currentThemeMode == mode) {
                                            Icon(Icons.Rounded.Check, null, Modifier.size(20.dp))
                                        }
                                    },
                                )
                            }
                        }
                    }
                },
            )
        }

        item {
            AppListItem(
                title = stringResource(R.string.settings_theme_color),
                summary = currentThemeColor.label,
                leading = { Icon(Icons.Rounded.Palette, contentDescription = null) },
                onClick = { themeColorMenuExpanded = true },
                trailing = {
                    Box {
                        DropdownMenu(
                            expanded = themeColorMenuExpanded,
                            onDismissRequest = { themeColorMenuExpanded = false },
                            shape = ContinuousCornerShape(16.dp),
                        ) {
                            ThemeColor.entries.forEach { color ->
                                DropdownMenuItem(
                                    text = {
                                        Text(
                                            color.label,
                                            fontWeight =
                                                if (currentThemeColor ==
                                                    color
                                                ) {
                                                    FontWeight.SemiBold
                                                } else {
                                                    FontWeight.Normal
                                                },
                                        )
                                    },
                                    onClick = {
                                        themeColorMenuExpanded = false
                                        onThemeColorChange(color)
                                    },
                                    trailingIcon = {
                                        if (currentThemeColor == color) Icon(Icons.Rounded.Check, null, Modifier.size(20.dp))
                                    },
                                )
                            }
                        }
                    }
                },
            )
        }

        item {
            AppListItem(
                title = stringResource(R.string.settings_nav_bar_style),
                summary = stringResource(currentNavBarStyle.titleRes),
                leading = { Icon(Icons.Rounded.ViewAgenda, contentDescription = null) },
                onClick = { navBarStyleMenuExpanded = true },
                trailing = {
                    Box {
                        DropdownMenu(
                            expanded = navBarStyleMenuExpanded,
                            onDismissRequest = { navBarStyleMenuExpanded = false },
                            shape = ContinuousCornerShape(16.dp),
                        ) {
                            NavBarStyle.entries.forEach { style ->
                                DropdownMenuItem(
                                    text = {
                                        Text(
                                            stringResource(style.titleRes),
                                            fontWeight =
                                                if (currentNavBarStyle ==
                                                    style
                                                ) {
                                                    FontWeight.SemiBold
                                                } else {
                                                    FontWeight.Normal
                                                },
                                        )
                                    },
                                    onClick = {
                                        navBarStyleMenuExpanded = false
                                        onNavBarStyleChange(style)
                                    },
                                    trailingIcon = {
                                        if (currentNavBarStyle == style) {
                                            Icon(Icons.Rounded.Check, null, Modifier.size(20.dp))
                                        }
                                    },
                                )
                            }
                        }
                    }
                },
            )
        }

        item {
            ListRow(
                modifier = Modifier.toggleable(value = amoledEnabled, role = Role.Switch, onValueChange = onAmoledChange),
                icon = { Icon(Icons.Rounded.PhoneAndroid, contentDescription = null) },
                headline = {
                    Text(
                        stringResource(R.string.settings_amoled_black),
                    )
                },
                supporting = { Text(stringResource(R.string.settings_amoled_black_summary)) },
                trailing = { AppSwitch(checked = amoledEnabled, onCheckedChange = onAmoledChange) },
            )
        }

        item {
            ListRow(
                modifier = Modifier.toggleable(value = blurEnabled, role = Role.Switch, onValueChange = onBlurChange),
                icon = { Icon(Icons.Rounded.BlurOn, contentDescription = null) },
                headline = {
                    Text(
                        stringResource(R.string.settings_blur),
                    )
                },
                supporting = { Text(stringResource(R.string.settings_blur_summary)) },
                trailing = { AppSwitch(checked = blurEnabled, onCheckedChange = onBlurChange) },
            )
        }
    }
}
