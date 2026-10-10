package me.nekosu.aqnya.ui.screens.modules

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.EaseInOutCubic
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.outlined.Delete
import androidx.compose.material.icons.outlined.PlayArrow
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.layout.SubcomposeLayout
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import me.nekosu.aqnya.R
import me.nekosu.aqnya.util.ModuleInfo
import kotlin.math.roundToInt

/** 描述收起时最多显示的行数，超出后可点击卡片展开。 */
private const val DESCRIPTION_MAX_LINES = 3

/**
 * 单个模块的卡片 —— 参照 KernelSU 管理器的 `ModuleItem` 重写。
 *
 * 布局自顶向下：
 *  - 模块名 / 版本 / 作者，右侧为启用开关；
 *  - 描述文本，点击卡片可展开 / 收起；
 *  - 元模块、待更新等标签；
 *  - 分隔线；
 *  - 底部操作行：执行 action（若模块提供）、移除 / 撤销，其余操作收进溢出菜单。
 *
 * 与 KernelSU 一致，「移除」只是标记下次启动时删除，可通过撤销按钮恢复；
 * 立即卸载是破坏性操作，仍保留在溢出菜单中。
 */
@Composable
internal fun ModuleCard(
    module: ModuleInfo,
    onToggle: (Boolean) -> Unit,
    onAction: () -> Unit,
    onRemove: () -> Unit,
    onUndoRemove: () -> Unit,
    onUninstall: () -> Unit,
) {
    val haptic = LocalHapticFeedback.current
    var expanded by rememberSaveable(module.id) { mutableStateOf(false) }
    var menuOpen by remember { mutableStateOf(false) }

    val hasDescription = module.description.isNotBlank()
    val pendingRemoval = module.remove
    val actionEnabled = !pendingRemoval && module.enabled
    val textDecoration = if (pendingRemoval) TextDecoration.LineThrough else null

    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(20.dp),
        colors =
            CardDefaults.cardColors(
                containerColor = MaterialTheme.colorScheme.surfaceContainer,
            ),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
    ) {
        Column(
            modifier =
                Modifier
                    .fillMaxWidth()
                    .then(if (hasDescription) Modifier.clickable { expanded = !expanded } else Modifier)
                    .padding(start = 16.dp, top = 14.dp, end = 16.dp, bottom = 10.dp),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.Top,
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = module.title,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold,
                        textDecoration = textDecoration,
                    )
                    if (module.version.isNotBlank()) {
                        Text(
                            text = "${stringResource(R.string.modules_version)}: ${module.version}",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            textDecoration = textDecoration,
                        )
                    }
                    if (module.author.isNotBlank()) {
                        Text(
                            text = "${stringResource(R.string.modules_author)}: ${module.author}",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            textDecoration = textDecoration,
                        )
                    }
                }

                Spacer(Modifier.width(12.dp))

                Switch(
                    checked = module.enabled,
                    enabled = !pendingRemoval,
                    onCheckedChange = {
                        haptic.performHapticFeedback(HapticFeedbackType.VirtualKey)
                        onToggle(it)
                    },
                )
            }

            if (hasDescription) {
                Spacer(Modifier.height(8.dp))
                ExpandableDescriptionText(
                    text = module.description,
                    expanded = expanded,
                    maxLinesLimit = DESCRIPTION_MAX_LINES,
                    modifier = Modifier.fillMaxWidth(),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    style = MaterialTheme.typography.bodyMedium,
                    textDecoration = textDecoration,
                )
            }

            if (module.metamodule || module.update) {
                Row(
                    modifier = Modifier.padding(top = 8.dp),
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    if (module.metamodule) {
                        ModuleTag(stringResource(R.string.modules_metamodule), MaterialTheme.colorScheme.primary)
                    }
                    if (module.update) {
                        ModuleTag(stringResource(R.string.modules_update), MaterialTheme.colorScheme.tertiary)
                    }
                }
            }

            HorizontalDivider(
                modifier = Modifier.padding(top = 10.dp, bottom = 4.dp),
                thickness = Dp.Hairline,
            )

            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                AnimatedVisibility(
                    visible = actionEnabled,
                    enter = fadeIn(),
                    exit = fadeOut(),
                ) {
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        if (module.hasActionScript) {
                            FilledTonalButton(
                                onClick = onAction,
                                modifier = Modifier.defaultMinSize(minWidth = 52.dp, minHeight = 32.dp),
                                contentPadding = ButtonDefaults.TextButtonContentPadding,
                            ) {
                                Icon(
                                    modifier = Modifier.size(20.dp),
                                    imageVector = Icons.Outlined.PlayArrow,
                                    contentDescription = null,
                                )
                                Text(
                                    modifier = Modifier.padding(start = 7.dp),
                                    text = stringResource(R.string.modules_action),
                                    style = MaterialTheme.typography.labelMedium,
                                )
                            }
                        }
                    }
                }

                Spacer(Modifier.weight(1f))

                FilledTonalButton(
                    onClick = { if (pendingRemoval) onUndoRemove() else onRemove() },
                    modifier = Modifier.defaultMinSize(minWidth = 52.dp, minHeight = 32.dp),
                    contentPadding = ButtonDefaults.TextButtonContentPadding,
                ) {
                    if (pendingRemoval) {
                        Icon(
                            modifier = Modifier.size(20.dp),
                            imageVector = Icons.Outlined.Refresh,
                            contentDescription = null,
                        )
                    } else {
                        Icon(
                            modifier = Modifier.size(20.dp),
                            imageVector = Icons.Outlined.Delete,
                            contentDescription = null,
                        )
                    }
                    Text(
                        modifier = Modifier.padding(start = 7.dp),
                        text = stringResource(if (pendingRemoval) R.string.modules_undo else R.string.modules_remove),
                        style = MaterialTheme.typography.labelMedium,
                    )
                }

                Spacer(Modifier.width(4.dp))

                Box {
                    IconButton(onClick = { menuOpen = true }) {
                        Icon(
                            Icons.Filled.MoreVert,
                            contentDescription = stringResource(R.string.cd_more),
                        )
                    }
                    DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
                        DropdownMenuItem(
                            text = {
                                Text(
                                    text = stringResource(R.string.modules_action_uninstall),
                                    color = MaterialTheme.colorScheme.error,
                                )
                            },
                            onClick = {
                                menuOpen = false
                                onUninstall()
                            },
                        )
                    }
                }
            }
        }
    }
}

/** 模块卡片上的小标签（元模块 / 待更新）。 */
@Composable
private fun ModuleTag(
    text: String,
    color: Color,
) {
    Surface(
        shape = RoundedCornerShape(6.dp),
        color = color.copy(alpha = 0.15f),
    ) {
        Text(
            text = text,
            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
            style = MaterialTheme.typography.labelSmall,
            color = color,
        )
    }
}

private enum class DescriptionSlot {
    Collapsed,
    Expanded,
}

/**
 * 可展开的描述文本，移植自 KernelSU 的 `ExpandableDescriptionText`。
 *
 * 用 [SubcomposeLayout] 同时测量收起与展开两种高度，据此在二者之间做高度动画，
 * 只有真正溢出时才会变化，[expanded] 切换时平滑过渡。
 */
@Composable
private fun ExpandableDescriptionText(
    text: String,
    expanded: Boolean,
    maxLinesLimit: Int,
    modifier: Modifier = Modifier,
    color: Color = Color.Unspecified,
    style: TextStyle = LocalTextStyle.current,
    textDecoration: TextDecoration? = null,
) {
    val progress = remember(text) { Animatable(if (expanded) 1f else 0f) }

    LaunchedEffect(expanded) {
        val target = if (expanded) 1f else 0f
        if (progress.targetValue != target) {
            val spec =
                if (expanded) {
                    tween<Float>(durationMillis = 280, easing = FastOutSlowInEasing)
                } else {
                    tween<Float>(durationMillis = 320, easing = EaseInOutCubic)
                }
            progress.animateTo(target, animationSpec = spec)
        }
    }

    SubcomposeLayout(modifier = modifier.clipToBounds()) { constraints ->
        val collapsedPlaceable =
            subcompose(DescriptionSlot.Collapsed) {
                Text(
                    text = text,
                    color = color,
                    style = style,
                    textDecoration = textDecoration,
                    maxLines = maxLinesLimit,
                    overflow = TextOverflow.Ellipsis,
                )
            }.first().measure(constraints)

        val expandedPlaceable =
            subcompose(DescriptionSlot.Expanded) {
                Text(
                    text = text,
                    color = color,
                    style = style,
                    textDecoration = textDecoration,
                    maxLines = Int.MAX_VALUE,
                    overflow = TextOverflow.Clip,
                )
            }.first().measure(constraints)

        val collapsedHeight = collapsedPlaceable.height
        val expandedHeight = expandedPlaceable.height
        val canExpand = expandedHeight > collapsedHeight

        if (!canExpand) {
            val width = collapsedPlaceable.width.coerceIn(constraints.minWidth, constraints.maxWidth)
            val height = collapsedHeight.coerceIn(constraints.minHeight, constraints.maxHeight)
            layout(width, height) {
                collapsedPlaceable.place(0, 0)
            }
        } else {
            val currentProgress = progress.value
            val currentHeight =
                (collapsedHeight + (expandedHeight - collapsedHeight) * currentProgress)
                    .roundToInt()
                    .coerceIn(constraints.minHeight, constraints.maxHeight)
            val placeableToUse = if (currentProgress == 0f) collapsedPlaceable else expandedPlaceable
            val width = placeableToUse.width.coerceIn(constraints.minWidth, constraints.maxWidth)

            layout(width, currentHeight) {
                placeableToUse.place(0, 0)
            }
        }
    }
}
