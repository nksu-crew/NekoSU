// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.navigation

import androidx.compose.animation.Crossfade
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
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
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.TransformOrigin
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import dev.chrisbanes.haze.ExperimentalHazeApi
import dev.chrisbanes.haze.HazeInput
import dev.chrisbanes.haze.HazePerformanceMode
import dev.chrisbanes.haze.HazeState
import dev.chrisbanes.haze.glass.GlassStyle
import dev.chrisbanes.haze.glass.hazeGlass
import kotlin.math.abs
import kotlin.math.roundToInt

/** Floating glass bar metrics tuned to the iOS 26 floating tab bar. */
val GlassBottomBarHeight = 64.dp
val GlassBottomBarHorizontalPadding = 20.dp
val GlassBottomBarVerticalPadding = 10.dp

/**
 * Bottom scroll clearance for tab content: bar height + vertical padding on
 * both sides + the floating gap between content and the bar.
 */
val GlassBottomBarContentPadding = 116.dp

/** Clearance for the stock Material 3 navigation bar (80 dp) plus a 16 dp gap. */
val StandardBottomBarContentPadding = 96.dp

/** Bottom scroll clearance selected by the current navigation bar style. */
val LocalBottomBarClearance = compositionLocalOf { GlassBottomBarContentPadding }

/**
 * Floating liquid-glass bottom bar built on Haze's Glass material.
 *
 * The caller must mark the content shown behind the bar with
 * `Modifier.hazeSource(hazeState)`.
 */
@OptIn(ExperimentalHazeApi::class)
@Composable
fun GlassBottomNavigationBar(
    hazeState: HazeState,
    items: List<BottomNavItem>,
    selectedIndex: Int,
    onTabClick: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    val scheme = MaterialTheme.colorScheme
    val isDark = scheme.surfaceContainer.luminance() < 0.5f
    val barShape = remember { RoundedCornerShape(GlassBottomBarHeight / 2) }
    val selectionShape = remember { RoundedCornerShape(28.dp) }

    /*
     * Two Haze Glass surfaces, as in KernelSU: the bar and a neutral selected
     * pill.  The pill is not a colour block; the accent stays on the icon and
     * label, and the glass provides the rim and refraction.
     */
    val barStyle =
        remember(isDark, scheme.surfaceContainer) {
            GlassStyle.regular.then {
                shape(barShape)
                backgroundColor(scheme.surfaceContainer.copy(alpha = 0.40f))
                tint(Color.Transparent)
                edgeShadow(Color.Black.copy(alpha = if (isDark) 0.22f else 0.10f))
                specularIntensity(0.85f)
                lightPosition(Alignment.TopCenter)
                chromaticAberrationStrength(0.08f)
            }
        }

    val selectionStyle =
        remember(isDark) {
            GlassStyle.clear.then {
                shape(selectionShape)
                backgroundColor(
                    if (isDark) {
                        Color.White.copy(alpha = 0.10f)
                    } else {
                        Color.Black.copy(alpha = 0.10f)
                    },
                )
                tint(Color.Transparent)
                edgeShadow(Color.Black.copy(alpha = if (isDark) 0.22f else 0.10f))
                specularIntensity(0.90f)
                lightPosition(Alignment.TopCenter)
                chromaticAberrationStrength(0f)
            }
        }

    BoxWithConstraints(
        modifier =
            modifier
                .height(GlassBottomBarHeight)
                .fillMaxWidth()
                .shadow(
                    elevation = 16.dp,
                    shape = barShape,
                    clip = false,
                    ambientColor = Color.Black.copy(alpha = if (isDark) 0.22f else 0.10f),
                    spotColor = Color.Black.copy(alpha = if (isDark) 0.28f else 0.16f),
                )
                .clip(barShape)
                .hazeGlass(
                    input = HazeInput.Sources(hazeState),
                    style = barStyle,
                    performanceMode = HazePerformanceMode.Default,
                ),
    ) {
        val tabWidth = (maxWidth - 8.dp) / items.size
        val density = LocalDensity.current
        val tabWidthPx = with(density) { tabWidth.toPx() }
        var dragOffset by remember { mutableFloatStateOf(0f) }
        val dragOffsetDp = with(density) { dragOffset.toDp() }
        val animatedIndex by animateFloatAsState(
            targetValue = selectedIndex.toFloat(),
            animationSpec = spring(dampingRatio = 0.66f, stiffness = 500f),
            label = "glassTabIndicator",
        )
        // KernelSU keeps the pill at tab width and lets the glass stretch
        // with the spring instead of growing a coloured selection block.
        val travel = abs(animatedIndex - selectedIndex).coerceIn(0f, 1f)
        val stretch = 1f + travel * 0.06f

        Box(
            modifier =
                Modifier
                    .align(Alignment.CenterStart)
                    .offset(x = 4.dp + tabWidth * animatedIndex + dragOffsetDp)
                    .width(tabWidth)
                    .height(maxHeight - 8.dp)
                    .graphicsLayer {
                        scaleX = stretch
                        scaleY = 1f - (stretch - 1f) * 0.45f
                        transformOrigin = TransformOrigin.Center
                    }
                    .shadow(
                        elevation = 10.dp,
                        shape = selectionShape,
                        clip = false,
                        ambientColor = Color.Black.copy(alpha = if (isDark) 0.20f else 0.08f),
                        spotColor = Color.Black.copy(alpha = if (isDark) 0.28f else 0.12f),
                    )
                    .clip(selectionShape)
                    .hazeGlass(
                        input = HazeInput.Sources(hazeState),
                        style = selectionStyle,
                        performanceMode = HazePerformanceMode.Default,
                    ),
        )

        Row(
            modifier =
                Modifier
                    .fillMaxSize()
                    .padding(horizontal = 4.dp)
                    .pointerInput(tabWidthPx, selectedIndex) {
                        detectHorizontalDragGestures(
                            onDragStart = { dragOffset = 0f },
                            onDragCancel = { dragOffset = 0f },
                            onDragEnd = {
                                val steps =
                                    if (tabWidthPx > 0f) {
                                        (dragOffset / tabWidthPx).roundToInt()
                                    } else {
                                        0
                                    }
                                val target =
                                    (selectedIndex + steps)
                                        .coerceIn(0, items.lastIndex)
                                dragOffset = 0f
                                if (target != selectedIndex) onTabClick(target)
                            },
                            onHorizontalDrag = { change, amount ->
                                change.consume()
                                dragOffset =
                                    (dragOffset + amount).coerceIn(
                                        -tabWidthPx * items.lastIndex,
                                        tabWidthPx * items.lastIndex,
                                    )
                            },
                        )
                    },
            verticalAlignment = Alignment.CenterVertically,
        ) {
            items.forEachIndexed { index, item ->
                GlassBottomBarItem(
                    item = item,
                    selected = index == selectedIndex,
                    selectedColor = scheme.primary,
                    unselectedColor = scheme.onSurface.copy(alpha = 0.86f),
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
    val contentColor by animateColorAsState(
        targetValue = if (selected) selectedColor else unselectedColor,
        animationSpec = tween(180),
        label = "glassTabContentColor",
    )
    val labelWeight = if (selected) FontWeight.SemiBold else FontWeight.Medium

    val pressScale by animateFloatAsState(
        targetValue = if (pressed) 1.15f else 1f,
        animationSpec = spring(dampingRatio = 0.5f, stiffness = 500f),
        label = "glassTabPressScale",
    )

    Column(
        modifier =
            modifier.selectable(
                selected = selected,
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
                            scaleX = pressScale
                            scaleY = pressScale
                            translationY = if (selected) -1.dp.toPx() else 0f
                        },
            )
        }

        Spacer(Modifier.height(4.dp))

        Text(
            text = stringResource(item.titleRes),
            color = contentColor,
            style = MaterialTheme.typography.labelSmall,
            fontSize = 11.sp,
            fontWeight = labelWeight,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}
