// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.screens.settings

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.KeyboardArrowRight
import androidx.compose.material.icons.rounded.Check
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import me.nekosu.aqnya.ui.component.AppSwitch
import me.nekosu.aqnya.ui.component.ListRow
import me.nekosu.aqnya.ui.theme.ContinuousCornerShape
import me.nekosu.aqnya.ui.theme.ThemeSwatchColors

/** One entry of [SettingsSegmentedControl]. Built in composition so callers can resolve strings. */
data class SegmentOption<T>(
    val value: T,
    val label: String,
    val icon: ImageVector,
)

/** Expressive rounded container shared by every settings group. */
@Composable
fun SettingsCard(
    modifier: Modifier = Modifier,
    containerColor: Color = MaterialTheme.colorScheme.surfaceBright,
    content: @Composable ColumnScope.() -> Unit,
) {
    Surface(
        modifier = modifier.fillMaxWidth(),
        shape = ContinuousCornerShape(28.dp),
        color = containerColor,
        contentColor = MaterialTheme.colorScheme.onSurface,
    ) {
        Column(content = content)
    }
}

/** Tonal icon tile used by every settings row. */
@Composable
fun SettingsIcon(
    icon: ImageVector,
    modifier: Modifier = Modifier,
    containerColor: Color = MaterialTheme.colorScheme.secondaryContainer,
    contentColor: Color = MaterialTheme.colorScheme.onSecondaryContainer,
    tileSize: Dp = 42.dp,
    iconSize: Dp = 21.dp,
    cornerRadius: Dp = 15.dp,
) {
    Box(
        modifier =
            modifier
                .size(tileSize)
                .clip(ContinuousCornerShape(cornerRadius))
                .background(containerColor),
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            imageVector = icon,
            contentDescription = null,
            modifier = Modifier.size(iconSize),
            tint = contentColor,
        )
    }
}

@Composable
fun SettingsActionEntry(
    icon: ImageVector,
    title: String,
    summary: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    trailing: @Composable (() -> Unit)? = null,
) {
    ListRow(
        modifier = modifier.fillMaxWidth(),
        icon = { SettingsEntryIcon(icon) },
        headline = { SettingsEntryTitle(title) },
        supporting = { SettingsEntrySummary(summary) },
        trailing = {
            if (trailing != null) {
                trailing()
            } else {
                SettingsEntryChevron()
            }
        },
        onClick = onClick,
    )
}

@Composable
fun SettingsSwitchEntry(
    icon: ImageVector,
    title: String,
    summary: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    enabled: Boolean = true,
) {
    ListRow(
        modifier = Modifier.fillMaxWidth(),
        icon = { SettingsEntryIcon(icon) },
        headline = { SettingsEntryTitle(title) },
        supporting = { SettingsEntrySummary(summary) },
        trailing = { AppSwitch(checked = checked, onCheckedChange = null, enabled = enabled) },
        checked = checked,
        onCheckedChange = onCheckedChange,
        enabled = enabled,
    )
}

@Composable
fun SettingsChoiceEntry(
    icon: ImageVector,
    title: String,
    selected: Boolean,
    onClick: () -> Unit,
) {
    ListRow(
        modifier = Modifier.fillMaxWidth(),
        icon = { SettingsEntryIcon(icon) },
        headline = {
            Text(
                text = title,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal,
            )
        },
        trailing = {
            if (selected) {
                Icon(
                    imageVector = Icons.Rounded.Check,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary,
                )
            }
        },
        selected = selected,
        onClick = onClick,
    )
}

@Composable
private fun SettingsEntryIcon(icon: ImageVector) {
    SettingsIcon(
        icon = icon,
        tileSize = 48.dp,
        iconSize = 24.dp,
        cornerRadius = 16.dp,
    )
}

@Composable
private fun SettingsEntryTitle(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.titleMedium,
    )
}

@Composable
private fun SettingsEntrySummary(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        maxLines = 2,
        overflow = TextOverflow.Ellipsis,
    )
}

@Composable
private fun SettingsEntryChevron() {
    Icon(
        imageVector = Icons.AutoMirrored.Rounded.KeyboardArrowRight,
        contentDescription = null,
        tint = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}

/** Pill-shaped floating control using the expressive container hierarchy. */
@Composable
fun <T> SettingsSegmentedControl(
    options: List<SegmentOption<T>>,
    selected: T,
    onSelected: (T) -> Unit,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier = modifier.fillMaxWidth().selectableGroup(),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        options.forEach { option ->
            val isSelected = option.value == selected
            val background by animateColorAsState(
                targetValue =
                    if (isSelected) {
                        MaterialTheme.colorScheme.primary
                    } else {
                        MaterialTheme.colorScheme.surfaceContainerHighest
                    },
                animationSpec = tween(durationMillis = 220),
                label = "segmentBackground",
            )
            val foreground by animateColorAsState(
                targetValue =
                    if (isSelected) {
                        MaterialTheme.colorScheme.onPrimary
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    },
                animationSpec = tween(durationMillis = 220),
                label = "segmentForeground",
            )
            Box(
                modifier =
                    Modifier
                        .weight(1f)
                        .height(64.dp)
                        .clip(CircleShape)
                        .background(background)
                        .selectable(
                            selected = isSelected,
                            role = Role.RadioButton,
                            onClick = { onSelected(option.value) },
                        ),
                contentAlignment = Alignment.Center,
            ) {
                Icon(
                    imageVector = option.icon,
                    contentDescription = option.label,
                    modifier = Modifier.size(24.dp),
                    tint = foreground,
                )
            }
        }
    }
}

/** Large palette preview used by the theme color picker. */
@Composable
fun SettingsThemeSwatch(
    colors: ThemeSwatchColors,
    label: String,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val shape = ContinuousCornerShape(24.dp)
    val borderColor by animateColorAsState(
        targetValue = if (selected) MaterialTheme.colorScheme.primary else Color.Transparent,
        animationSpec = tween(durationMillis = 220),
        label = "swatchBorder",
    )

    Box(
        modifier =
            modifier
                .width(88.dp)
                .height(76.dp)
                .clip(shape)
                .selectable(
                    selected = selected,
                    role = Role.RadioButton,
                    onClick = onClick,
                ).semantics { contentDescription = label }
                .border(width = 3.dp, color = borderColor, shape = shape)
                .padding(4.dp),
    ) {
        Box(
            modifier =
                Modifier
                    .fillMaxSize()
                    .clip(ContinuousCornerShape(20.dp))
                    .background(Brush.verticalGradient(listOf(colors.top, colors.bottom))),
            contentAlignment = Alignment.Center,
        ) {
            if (selected) {
                Box(
                    modifier =
                        Modifier
                            .size(36.dp)
                            .clip(CircleShape)
                            .background(MaterialTheme.colorScheme.surface)
                            .border(width = 2.dp, color = colors.accent, shape = CircleShape),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(
                        imageVector = Icons.Rounded.Check,
                        contentDescription = null,
                        modifier = Modifier.size(18.dp),
                        tint = colors.accent,
                    )
                }
            } else {
                Column(
                    modifier = Modifier.size(36.dp).clip(CircleShape),
                ) {
                    Box(Modifier.weight(1f).fillMaxWidth().background(colors.accent))
                    Box(Modifier.weight(1f).fillMaxWidth().background(colors.accentVariant))
                }
            }
        }
    }
}
