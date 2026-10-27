// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.navigation

import androidx.compose.animation.Crossfade
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.TransformOrigin
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.DpOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.kyant.backdrop.Backdrop
import com.kyant.backdrop.drawBackdrop
import com.kyant.backdrop.effects.blur
import com.kyant.backdrop.effects.lens
import com.kyant.backdrop.effects.vibrancy
import com.kyant.backdrop.highlight.Highlight
import com.kyant.backdrop.shadow.InnerShadow
import com.kyant.backdrop.shadow.Shadow
import me.nekosu.aqnya.ui.theme.ContinuousCornerShape
import kotlin.math.abs

/** Floating glass bar metrics tuned to the iOS 26 floating tab bar. */
val GlassBottomBarHeight = 68.dp
val GlassBottomBarHorizontalPadding = 16.dp
val GlassBottomBarVerticalPadding = 10.dp

/**
 * Bottom scroll clearance for tab content: bar height + vertical padding on
 * both sides + the floating gap between content and the bar.
 */
val GlassBottomBarContentPadding = 120.dp

/** Clearance for the stock Material 3 navigation bar (80 dp) plus a 16 dp gap. */
val StandardBottomBarContentPadding = 96.dp

/** Bottom scroll clearance selected by the current navigation bar style. */
val LocalBottomBarClearance = compositionLocalOf { GlassBottomBarContentPadding }

/**
 * Floating liquid-glass bottom bar backed by the `io.github.kyant0:backdrop`
 * library.
 *
 * The caller must wrap the content shown behind the bar with
 * `Modifier.layerBackdrop(backdrop)` and build the backdrop with
 * `rememberLayerBackdrop`.
 */
@Composable
fun GlassBottomNavigationBar(
    backdrop: Backdrop,
    items: List<BottomNavItem>,
    selectedIndex: Int,
    onTabClick: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    val scheme = MaterialTheme.colorScheme
    val isDark = scheme.surfaceContainer.luminance() < 0.5f
    val barShape = remember { ContinuousCornerShape(GlassBottomBarHeight / 2) }
    val selectionShape = remember { ContinuousCornerShape(28.dp) }

    val barTop =
        (if (isDark) Color(0xFF16181D) else Color.White).copy(
            alpha = if (isDark) 0.58f else 0.52f,
        )
    val barBottom =
        (if (isDark) Color(0xFF090A0D) else Color(0xFFF4F4F8)).copy(
            alpha = if (isDark) 0.68f else 0.42f,
        )
    val selectionTop = scheme.primaryContainer.copy(alpha = if (isDark) 0.55f else 0.62f)
    val selectionBottom = scheme.primaryContainer.copy(alpha = if (isDark) 0.35f else 0.42f)

    BoxWithConstraints(
        modifier =
            modifier
                .height(GlassBottomBarHeight)
                .fillMaxWidth()
                .drawBackdrop(
                    backdrop = backdrop,
                    shape = { barShape },
                    effects = {
                        vibrancy()
                        blur(12.dp.toPx())
                        lens(28.dp.toPx(), 36.dp.toPx(), depthEffect = true)
                    },
                    highlight = {
                        Highlight.Default.copy(
                            width = 0.7.dp,
                            blurRadius = 0.35.dp,
                            alpha = if (isDark) 0.55f else 0.9f,
                        )
                    },
                    shadow = {
                        Shadow(
                            radius = 24.dp,
                            offset = DpOffset(0.dp, 10.dp),
                            color = Color.Black.copy(alpha = if (isDark) 0.34f else 0.16f),
                        )
                    },
                    innerShadow = {
                        InnerShadow(
                            radius = 16.dp,
                            offset = DpOffset(0.dp, 4.dp),
                            color = Color.Black.copy(alpha = if (isDark) 0.28f else 0.10f),
                            alpha = 0.7f,
                        )
                    },
                    onDrawSurface = {
                        drawRect(
                            brush =
                                Brush.verticalGradient(
                                    colors = listOf(barTop, barBottom),
                                ),
                        )
                        drawRect(
                            brush =
                                Brush.verticalGradient(
                                    colors =
                                        listOf(
                                            Color.White.copy(alpha = if (isDark) 0.05f else 0.18f),
                                            Color.Transparent,
                                        ),
                                    endY = size.height * 0.5f,
                                ),
                        )
                    },
                )
                .clip(barShape)
                .padding(4.dp),
    ) {
        val tabWidth = maxWidth / items.size
        val animatedIndex by animateFloatAsState(
            targetValue = selectedIndex.toFloat(),
            animationSpec = spring(dampingRatio = 0.72f, stiffness = 420f),
            label = "glassTabIndicator",
        )
        // Stretch the glass while it travels between tabs, then let it settle
        // back into a capsule.  This is the "liquid" part of the selection.
        val stretch =
            1f + (abs(animatedIndex - selectedIndex) * 0.08f).coerceAtMost(0.14f)

        Box(
            modifier =
                Modifier
                    .align(Alignment.CenterStart)
                    .offset(x = tabWidth * animatedIndex + 4.dp)
                    .width(tabWidth - 8.dp)
                    .height(maxHeight - 8.dp)
                    .graphicsLayer {
                        scaleX = stretch
                        scaleY = 1f - (stretch - 1f) * 0.35f
                        transformOrigin = TransformOrigin.Center
                    }
                    .drawBackdrop(
                        backdrop = backdrop,
                        shape = { selectionShape },
                        effects = {
                            vibrancy()
                            blur(6.dp.toPx())
                            lens(16.dp.toPx(), 22.dp.toPx(), chromaticAberration = true)
                        },
                        highlight = {
                            Highlight.Default.copy(
                                width = 0.5.dp,
                                blurRadius = 0.25.dp,
                                alpha = 0.85f,
                            )
                        },
                        shadow = {
                            Shadow(
                                radius = 12.dp,
                                offset = DpOffset(0.dp, 3.dp),
                                color = Color.Black.copy(alpha = if (isDark) 0.22f else 0.10f),
                            )
                        },
                        innerShadow = {
                            InnerShadow(
                                radius = 10.dp,
                                offset = DpOffset(0.dp, 2.dp),
                                color = Color.Black.copy(alpha = if (isDark) 0.22f else 0.10f),
                                alpha = 0.8f,
                            )
                        },
                        onDrawSurface = {
                            drawRect(
                                brush =
                                    Brush.verticalGradient(
                                        colors = listOf(selectionTop, selectionBottom),
                                    ),
                            )
                            drawRect(
                                brush =
                                    Brush.verticalGradient(
                                        colors =
                                            listOf(
                                                Color.White.copy(alpha = 0.16f),
                                                Color.Transparent,
                                            ),
                                        endY = size.height * 0.45f,
                                    ),
                            )
                        },
                    ),
        )

        Row(
            modifier = Modifier.fillMaxSize(),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            items.forEachIndexed { index, item ->
                GlassBottomBarItem(
                    item = item,
                    selected = index == selectedIndex,
                    selectedColor = scheme.primary,
                    unselectedColor = scheme.onSurfaceVariant.copy(alpha = 0.9f),
                    modifier = Modifier.weight(1f).fillMaxHeight(),
                    onClick = { onTabClick(index) },
                )
            }
        }
    }
}

@Composable
private fun GlassBottomBarItem(
    item: BottomNavItem,
    selected: Boolean,
    selectedColor: Color,
    unselectedColor: Color,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    val interactionSource = remember { MutableInteractionSource() }
    val pressed by interactionSource.collectIsPressedAsState()
    val contentColor = if (selected) selectedColor else unselectedColor
    val labelWeight = if (selected) FontWeight.SemiBold else FontWeight.Medium

    val iconScale by animateFloatAsState(
        targetValue = if (selected) 1.08f else 1f,
        animationSpec = spring(dampingRatio = 0.7f, stiffness = 400f),
        label = "glassTabIconScale",
    )
    val pressScale by animateFloatAsState(
        targetValue = if (pressed) 0.88f else 1f,
        animationSpec = spring(dampingRatio = 0.5f, stiffness = 500f),
        label = "glassTabPressScale",
    )

    Column(
        modifier =
            modifier.clickable(
                interactionSource = interactionSource,
                indication = null,
                role = Role.Tab,
                onClick = onClick,
            ),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Crossfade(
            targetState = selected,
            animationSpec = tween(180),
            label = "glassTabIcon",
        ) { isSelected ->
            Icon(
                imageVector = if (isSelected) item.selectedIcon else item.unselectedIcon,
                contentDescription = stringResource(item.titleRes),
                tint = contentColor,
                modifier =
                    Modifier
                        .size(24.dp)
                        .graphicsLayer {
                            scaleX = iconScale * pressScale
                            scaleY = iconScale * pressScale
                            translationY = if (selected) -1.dp.toPx() else 0f
                        },
            )
        }

        Spacer(Modifier.height(3.dp))

        Text(
            text = stringResource(item.titleRes),
            color = contentColor,
            style = MaterialTheme.typography.labelSmall,
            fontSize = 10.5.sp,
            fontWeight = labelWeight,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}
