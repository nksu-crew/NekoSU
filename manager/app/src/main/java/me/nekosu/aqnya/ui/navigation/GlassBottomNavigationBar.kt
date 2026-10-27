// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.navigation

import androidx.compose.animation.Crossfade
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
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
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.BlurredEdgeTreatment
import androidx.compose.ui.draw.blur
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.TransformOrigin
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.luminance
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
     * The bar is a single Haze Glass surface.  The selection is a clear
     * liquid-glass highlight rather than a coloured block or a second blur
     * pass: that keeps the refraction coherent and leaves the accent colour
     * on the icon and label only.
     */
    val barStyle =
        remember(isDark) {
            GlassStyle.regular.then {
                shape(barShape)
                backgroundColor(
                    if (isDark) {
                        Color(0xFF0B0D11).copy(alpha = 0.26f)
                    } else {
                        Color.White.copy(alpha = 0.18f)
                    },
                )
                tint(
                    if (isDark) {
                        Color.White.copy(alpha = 0.03f)
                    } else {
                        Color.White.copy(alpha = 0.06f)
                    },
                )
                edgeShadow(Color.Black.copy(alpha = if (isDark) 0.32f else 0.10f))
                specularIntensity(if (isDark) 0.52f else 0.70f)
                lightPosition(Alignment.TopCenter)
                chromaticAberrationStrength(0.07f)
            }
        }

    val barBorder =
        remember(isDark) {
            Brush.verticalGradient(
                0f to Color.White.copy(alpha = if (isDark) 0.22f else 0.48f),
                1f to Color.White.copy(alpha = if (isDark) 0.02f else 0.08f),
            )
        }

    // Follow liquid_glass_widgets: the resting indicator is a neutral 10%
    // glass pill.  The accent colour lives in the selected icon glow, not in a
    // solid selection block.
    val indicatorColor =
        remember(isDark) {
            if (isDark) Color.White.copy(alpha = 0.10f) else Color.Black.copy(alpha = 0.10f)
        }

    val selectionBorder =
        remember(isDark) {
            Brush.verticalGradient(
                0f to Color.White.copy(alpha = if (isDark) 0.34f else 0.62f),
                1f to Color.White.copy(alpha = 0.04f),
            )
        }

    BoxWithConstraints(
        modifier =
            modifier
                .height(GlassBottomBarHeight)
                .fillMaxWidth()
                .shadow(
                    elevation = 24.dp,
                    shape = barShape,
                    clip = false,
                    ambientColor = Color.Black.copy(alpha = if (isDark) 0.30f else 0.12f),
                    spotColor = Color.Black.copy(alpha = if (isDark) 0.38f else 0.18f),
                )
                .clip(barShape)
                .hazeGlass(
                    input = HazeInput.Sources(hazeState),
                    style = barStyle,
                    performanceMode = HazePerformanceMode.Default,
                )
                .border(1.dp, barBorder, barShape),
    ) {
        val tabWidth = maxWidth / items.size
        val animatedIndex by animateFloatAsState(
            targetValue = selectedIndex.toFloat(),
            animationSpec = spring(dampingRatio = 0.66f, stiffness = 500f),
            label = "glassTabIndicator",
        )
        // Stretch the glass while it travels between tabs, then let it settle
        // back into a capsule.  This is the "liquid" part of the selection.
        val travel = abs(animatedIndex - selectedIndex).coerceIn(0f, 1f)
        val stretch = 1f + travel * 0.04f

        Box(
            modifier =
                Modifier
                    .align(Alignment.CenterStart)
                    .offset(x = tabWidth * animatedIndex + 4.dp - 12.dp * travel)
                    .width(tabWidth - 8.dp + 24.dp * travel)
                    .height(maxHeight - 8.dp + 16.dp * travel)
                    .graphicsLayer {
                        scaleX = stretch
                        scaleY = 1f - (stretch - 1f) * 0.5f
                        transformOrigin = TransformOrigin.Center
                    }
                    .shadow(
                        elevation = 8.dp,
                        shape = selectionShape,
                        clip = false,
                        ambientColor = Color.Black.copy(alpha = if (isDark) 0.18f else 0.08f),
                        spotColor = Color.Black.copy(alpha = if (isDark) 0.24f else 0.12f),
                    )
                    .clip(selectionShape)
                    .background(indicatorColor)
                    .drawWithContent {
                        drawContent()
                        drawRect(
                            brush =
                                Brush.verticalGradient(
                                    0f to Color.White.copy(alpha = if (isDark) 0.12f else 0.28f),
                                    0.55f to Color.Transparent,
                                    endY = size.height * 0.55f,
                                ),
                        )
                    }
                    .border(1.dp, selectionBorder, selectionShape),
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
                    glowColor = scheme.primary,
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
    glowColor: Color,
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

    val iconScale by animateFloatAsState(
        targetValue = if (selected) 1.12f else 1f,
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
        Box(
            modifier = Modifier.size(34.dp),
            contentAlignment = Alignment.Center,
        ) {
            if (selected) {
                Box(
                    modifier =
                        Modifier
                            .size(32.dp)
                            .blur(
                                radius = 26.dp,
                                edgeTreatment = BlurredEdgeTreatment.Unbounded,
                            ).background(glowColor.copy(alpha = 0.45f), CircleShape),
                )
            }

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
