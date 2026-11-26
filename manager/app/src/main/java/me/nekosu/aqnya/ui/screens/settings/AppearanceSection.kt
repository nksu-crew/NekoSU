// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.screens.settings

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.BlurOn
import androidx.compose.material.icons.rounded.BrightnessAuto
import androidx.compose.material.icons.rounded.Check
import androidx.compose.material.icons.rounded.DarkMode
import androidx.compose.material.icons.rounded.LightMode
import androidx.compose.material.icons.rounded.PhoneAndroid
import androidx.compose.material.icons.rounded.ViewAgenda
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import me.nekosu.aqnya.R
import me.nekosu.aqnya.ui.component.CardGroup
import me.nekosu.aqnya.ui.screens.settings.enums.NavBarStyle
import me.nekosu.aqnya.ui.screens.settings.enums.ThemeColor
import me.nekosu.aqnya.ui.screens.settings.enums.ThemeMode
import me.nekosu.aqnya.ui.theme.ContinuousCornerShape
import me.nekosu.aqnya.ui.theme.ThemeSwatchColors
import me.nekosu.aqnya.ui.theme.themeSwatchColors

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
    var navBarMenuExpanded by remember { mutableStateOf(false) }

    val themeModeOptions =
        ThemeMode.entries.map { mode ->
            SegmentOption(
                value = mode,
                label = stringResource(mode.titleRes),
                icon = mode.icon(),
            )
        }
    val isDark = MaterialTheme.colorScheme.surface.luminance() < 0.5f
    Column(
        modifier = Modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        SettingsCard {
            Column(
                modifier = Modifier.padding(vertical = 18.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Column(
                    modifier = Modifier.padding(horizontal = 16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    Text(
                        text = stringResource(R.string.settings_theme_mode),
                        style = MaterialTheme.typography.labelLarge,
                        fontWeight = FontWeight.Medium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    SettingsSegmentedControl(
                        options = themeModeOptions,
                        selected = currentThemeMode,
                        onSelected = onThemeChange,
                    )
                }

                Row(
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        text = stringResource(R.string.settings_theme_color),
                        style = MaterialTheme.typography.labelLarge,
                        fontWeight = FontWeight.Medium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.weight(1f),
                    )
                    Text(
                        text = currentThemeColor.label,
                        style = MaterialTheme.typography.labelLarge,
                        color = MaterialTheme.colorScheme.primary,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }

                Row(
                    modifier = Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Spacer(Modifier.width(16.dp))
                    ThemeColor.entries.forEach { color ->
                        val swatchColors =
                            if (color == ThemeColor.MATERIAL_YOU) {
                                ThemeSwatchColors(
                                    top = MaterialTheme.colorScheme.primaryContainer,
                                    bottom = MaterialTheme.colorScheme.tertiaryContainer,
                                    accent = MaterialTheme.colorScheme.primary,
                                    accentVariant = MaterialTheme.colorScheme.secondary,
                                )
                            } else {
                                themeSwatchColors(color.value, isDark)
                            }
                        SettingsThemeSwatch(
                            colors = swatchColors,
                            label = color.label,
                            selected = currentThemeColor == color,
                            onClick = { onThemeColorChange(color) },
                        )
                    }
                    Spacer(Modifier.width(16.dp))
                }
            }
        }

        CardGroup {
            item {
                Box {
                    SettingsActionEntry(
                        icon = Icons.Rounded.ViewAgenda,
                        title = stringResource(R.string.settings_nav_bar_style),
                        summary = stringResource(currentNavBarStyle.titleRes),
                        onClick = { navBarMenuExpanded = true },
                    )
                    DropdownMenu(
                        expanded = navBarMenuExpanded,
                        onDismissRequest = { navBarMenuExpanded = false },
                        shape = ContinuousCornerShape(20.dp),
                    ) {
                        NavBarStyle.entries.forEach { style ->
                            val selected = style == currentNavBarStyle
                            DropdownMenuItem(
                                text = {
                                    Text(
                                        text = stringResource(style.titleRes),
                                        fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal,
                                    )
                                },
                                leadingIcon = {
                                    Icon(
                                        imageVector = style.icon(),
                                        contentDescription = null,
                                    )
                                },
                                trailingIcon = {
                                    if (selected) {
                                        Icon(
                                            imageVector = Icons.Rounded.Check,
                                            contentDescription = null,
                                        )
                                    }
                                },
                                onClick = {
                                    navBarMenuExpanded = false
                                    onNavBarStyleChange(style)
                                },
                            )
                        }
                    }
                }
            }
            item {
                SettingsSwitchEntry(
                    icon = Icons.Rounded.PhoneAndroid,
                    title = stringResource(R.string.settings_amoled_black),
                    summary = stringResource(R.string.settings_amoled_black_summary),
                    checked = amoledEnabled,
                    onCheckedChange = onAmoledChange,
                )
            }
            item {
                SettingsSwitchEntry(
                    icon = Icons.Rounded.BlurOn,
                    title = stringResource(R.string.settings_blur),
                    summary = stringResource(R.string.settings_blur_summary),
                    checked = blurEnabled,
                    onCheckedChange = onBlurChange,
                )
            }
        }
    }
}

private fun ThemeMode.icon(): ImageVector =
    when (this) {
        ThemeMode.SYSTEM -> Icons.Rounded.BrightnessAuto
        ThemeMode.LIGHT -> Icons.Rounded.LightMode
        ThemeMode.DARK -> Icons.Rounded.DarkMode
    }

private fun NavBarStyle.icon(): ImageVector =
    when (this) {
        NavBarStyle.STANDARD -> Icons.Rounded.ViewAgenda
        NavBarStyle.GLASS -> Icons.Rounded.BlurOn
    }
