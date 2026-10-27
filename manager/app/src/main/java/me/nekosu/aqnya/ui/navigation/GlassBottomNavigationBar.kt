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
import androidx.compose.ui.draw.shadow
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
     * Use Haze's material engine rather than stacking hand-made blur,
     * gradients and borders.  It supplies the optical refraction, specular
     * rim, edge shadow and Fresnel response that make the glass read as a
     * physical surface; the explicit writes only tune it to this theme.
     */
    val barStyle =
        remember(isDark) {
            GlassStyle.regular.then {
                shape(barShape)
                backgroundColor(
                    if (isDark) {
                        Color(0xFF0B0D11).copy(alpha = 0.34f)
                    } else {
                        Color.White.copy(alpha = 0.26f)
                    },
                )
                tint(
                    if (isDark) {
                        Color.White.copy(alpha = 0.03f)
                    } else {
                        Color.White.copy(alpha = 0.07f)
                    },
                )
                edgeShadow(Color.Black.copy(alpha = if (isDark) 0.34f else 0.10f))
                specularIntensity(if (isDark) 0.38f else 0.55f)
                lightPosition(Alignment.TopCenter)
                chromaticAberrationStrength(0.05f)
            }
        }

    val selectionStyle =
        remember(isDark, scheme.primary, scheme.primaryContainer) {
            GlassStyle.clear.then {
                shape(selectionShape)
                backgroundColor(
                    scheme.primaryContainer.copy(
                        alpha = if (isDark) 0.30f else 0.38f,
                    ),
                )
                tint(scheme.primary.copy(alpha = if (isDark) 0.16f else 0.09f))
                edgeShadow(Color.Black.copy(alpha = if (isDark) 0.28f else 0.08f))
                specularIntensity(if (isDark) 0.42f else 0.60f)
                lightPosition(Alignment.TopCenter)
                chromaticAberrationStrength(0.08f)
            }
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
                .hazeGlass(
                    input = HazeInput.Sources(hazeState),
                    style = barStyle,
                    performanceMode = HazePerformanceMode.Default,
                )
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
                    .shadow(
                        elevation = 10.dp,
                        shape = selectionShape,
                        clip = false,
                        ambientColor = Color.Black.copy(alpha = if (isDark) 0.20f else 0.08f),
                        spotColor = Color.Black.copy(alpha = if (isDark) 0.26f else 0.12f),
                    )
                    .hazeGlass(
                        input = HazeInput.Sources(hazeState),
                        style = selectionStyle,
                        performanceMode = HazePerformanceMode.Default,
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
