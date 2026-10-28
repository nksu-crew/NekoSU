// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.navigation

import androidx.compose.animation.Crossfade
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.EaseOut
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.foundation.focusable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.dropShadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.graphics.shadow.Shadow as ComposeShadow
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.util.lerp
import com.kyant.backdrop.Backdrop
import com.kyant.backdrop.backdrops.layerBackdrop
import com.kyant.backdrop.backdrops.rememberCombinedBackdrop
import com.kyant.backdrop.backdrops.rememberLayerBackdrop
import com.kyant.backdrop.drawBackdrop
import com.kyant.backdrop.effects.blur
import com.kyant.backdrop.effects.lens
import com.kyant.backdrop.effects.vibrancy
import com.kyant.backdrop.highlight.Highlight
import com.kyant.backdrop.shadow.InnerShadow
import kotlinx.coroutines.launch
import kotlin.math.abs
import kotlin.math.roundToInt
import kotlin.math.sign

/** Floating glass bar metrics tuned to the iOS 26 floating tab bar. */
val GlassBottomBarHeight = 64.dp
val GlassBottomBarHorizontalPadding = 28.dp
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

private val GlassTabMinWidth = 76.dp
private val GlassPillHeight = 56.dp
private val GlassBarInnerPadding = 4.dp

/** Item scale driven by the bar press progress, read from a graphics layer. */
private val LocalGlassBarTabScale = staticCompositionLocalOf<() -> Float> { { 1f } }

/**
 * Liquid-glass bar modelled on KernelSU's floating bottom bar.
 *
 * The bar is a compact capsule sized by its tabs, scales up while pressed,
 * lets the finger drag the selection pill past the ends with a rubber-band
 * overscroll, and refracts the tab content through the pill. The caller must
 * record the page behind the bar with `Modifier.layerBackdrop(backdrop)`.
 */
@Composable
fun GlassBottomNavigationBar(
    backdrop: Backdrop,
    items: List<BottomNavItem>,
    selectedIndex: Int,
    onTabClick: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    require(items.isNotEmpty()) { "The glass bottom bar needs at least one tab" }

    val scheme = MaterialTheme.colorScheme
    val isDark = scheme.surfaceContainer.luminance() < 0.5f
    val barShape = CircleShape
    val containerColor = scheme.surfaceContainer.copy(alpha = 0.40f)
    // Kept opaque enough that the selected pill still reads as selected on top
    // of bright, busy backdrops once vibrancy and blur have been applied.
    val selectionSurface =
        if (isDark) Color.White.copy(alpha = 0.26f) else Color.Black.copy(alpha = 0.28f)
    val accentColor = scheme.primary
    val tabContentColor = scheme.onSurface

    val tabsBackdrop = rememberLayerBackdrop()
    val combinedBackdrop = rememberCombinedBackdrop(backdrop, tabsBackdrop)
    val density = LocalDensity.current
    val isLtr = LocalLayoutDirection.current == LayoutDirection.Ltr
    val animationScope = rememberCoroutineScope()

    var tabWidthPx by remember { mutableFloatStateOf(0f) }
    var totalWidthPx by remember { mutableFloatStateOf(0f) }

    val offsetAnimation = remember { Animatable(0f) }
    val rubberBandPx = with(density) { 4.dp.toPx() }
    val panelOffset by
        remember(rubberBandPx) {
            derivedStateOf {
                if (totalWidthPx == 0f) {
                    0f
                } else {
                    val fraction = (offsetAnimation.value / totalWidthPx).coerceIn(-1f, 1f)
                    rubberBandPx * fraction.sign * EaseOut.transform(abs(fraction))
                }
            }
        }

    var currentIndex by remember { mutableIntStateOf(selectedIndex) }
    val onTabClickUpdated by rememberUpdatedState(onTabClick)
    val lastIndex = items.lastIndex
    val innerPaddingPx = with(density) { GlassBarInnerPadding.toPx() }

    fun indexAt(positionX: Float): Int {
        if (tabWidthPx <= 0f) return currentIndex
        val logicalX = if (isLtr) positionX else totalWidthPx - positionX
        return ((logicalX - innerPaddingPx) / tabWidthPx).toInt().coerceIn(0, lastIndex)
    }

    val dragAnimation =
        remember(animationScope, items, density, isLtr) {
            DampedDragAnimation(
                animationScope = animationScope,
                initialValue = selectedIndex.toFloat(),
                valueRange = 0f..lastIndex.toFloat(),
                visibilityThreshold = 0.001f,
                initialScale = 1f,
                pressedScale = 78f / 56f,
                canDrag = { offset -> offset.x in 0f..totalWidthPx },
                onDragStarted = { position ->
                    updateValue(indexAt(position.x).toFloat())
                },
                onDragStopped = {
                    val targetIndex = targetValue.roundToInt().coerceIn(0, lastIndex)
                    if (currentIndex != targetIndex) {
                        currentIndex = targetIndex
                        onTabClickUpdated(targetIndex)
                    }
                    updateValue(targetIndex.toFloat())
                    animationScope.launch {
                        offsetAnimation.animateTo(0f, spring(1f, 300f, 0.5f))
                    }
                },
                onDragCancelled = {
                    updateValue(currentIndex.toFloat())
                    animationScope.launch {
                        offsetAnimation.animateTo(0f, spring(1f, 300f, 0.5f))
                    }
                },
                onDrag = { _, dragAmount ->
                    if (tabWidthPx > 0f && dragAmount.x != 0f) {
                        updateValue(
                            (targetValue + dragAmount.x / tabWidthPx * if (isLtr) 1f else -1f)
                                .coerceIn(0f, lastIndex.toFloat()),
                        )
                        animationScope.launch {
                            offsetAnimation.snapTo(offsetAnimation.value + dragAmount.x)
                        }
                    }
                },
            )
        }

    LaunchedEffect(selectedIndex, dragAnimation) {
        if (currentIndex != selectedIndex) {
            currentIndex = selectedIndex
            dragAnimation.animateToValue(selectedIndex.toFloat())
        }
    }

    fun activateTab(index: Int) {
        if (index !in items.indices) return
        if (currentIndex != index) {
            currentIndex = index
            onTabClickUpdated(index)
        }
        dragAnimation.animateToValue(index.toFloat())
    }

    val interactiveHighlight =
        remember(animationScope, tabWidthPx, dragAnimation, isLtr) {
            InteractiveHighlight(animationScope) { size, _ ->
                val centerX =
                    if (isLtr) {
                        (dragAnimation.value + 0.5f) * tabWidthPx
                    } else {
                        size.width - (dragAnimation.value + 0.5f) * tabWidthPx
                    }
                Offset(centerX, size.height / 2f)
            }
        }

    val barWidth = GlassTabMinWidth * items.size + GlassBarInnerPadding * 2

    Box(
        modifier =
            modifier
                .graphicsLayer { translationX = panelOffset }
                .width(barWidth)
                .height(GlassBottomBarHeight),
        contentAlignment = Alignment.CenterStart,
    ) {
        // The glass layer wraps the tab content so pressing also scales the
        // icons; the pill below stays outside and keeps its own press scale.
        Box(
            modifier =
                Modifier.fillMaxSize()
                    .layerBackdrop(tabsBackdrop)
                    .dropShadow(
                        shape = barShape,
                        shadow =
                            ComposeShadow(
                                radius = 10.dp,
                                color = Color.Black,
                                alpha = if (isDark) 0.2f else 0.1f,
                            ),
                    )
                    .drawBackdrop(
                        backdrop = backdrop,
                        shape = { barShape },
                        effects = {
                            padding = maxOf(padding, 40.dp.toPx())
                            vibrancy()
                            blur(4.dp.toPx())
                            lens(24.dp.toPx(), 24.dp.toPx())
                        },
                        highlight = { Highlight.Default.copy(alpha = 0.75f) },
                        layerBlock = {
                            val width = size.width.coerceAtLeast(1f)
                            val scale =
                                lerp(1f, 1f + 16.dp.toPx() / width, dragAnimation.pressProgress)
                            scaleX = scale
                            scaleY = scale
                        },
                        onDrawSurface = { drawRect(containerColor) },
                    ),
        ) {
            CompositionLocalProvider(
                LocalGlassBarTabScale provides { lerp(1f, 1.2f, dragAnimation.pressProgress) },
            ) {
                Row(
                    modifier =
                        Modifier.fillMaxSize()
                            .onGloballyPositioned { coordinates ->
                                totalWidthPx = coordinates.size.width.toFloat()
                                val contentWidthPx = totalWidthPx - innerPaddingPx * 2f
                                tabWidthPx = (contentWidthPx / items.size).coerceAtLeast(0f)
                            }
                            .selectableGroup()
                            .then(interactiveHighlight.modifier)
                            .then(interactiveHighlight.gestureModifier)
                            .then(dragAnimation.modifier)
                            .padding(GlassBarInnerPadding),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    items.forEachIndexed { index, item ->
                        GlassBottomBarItem(
                            item = item,
                            selected = index == selectedIndex,
                            selectedColor = accentColor,
                            unselectedColor = tabContentColor,
                            onSelect = { activateTab(index) },
                        )
                    }
                }
            }
        }

        // The pill samples the page plus the tab-content layer, so its lens can
        // refract the icons themselves, as in the KernelSU bar.
        if (tabWidthPx > 0f) {
            val tabWidthDp = with(density) { tabWidthPx.toDp() }
            Box(
                modifier =
                    Modifier.padding(horizontal = GlassBarInnerPadding)
                        .graphicsLayer {
                            val progressOffset = dragAnimation.value * tabWidthPx
                            translationX = if (isLtr) progressOffset else -progressOffset
                            scaleX = dragAnimation.scaleX
                            scaleY = dragAnimation.scaleY
                            val velocity = dragAnimation.velocity / 10f
                            scaleX /= 1f - (velocity * 0.75f).coerceIn(-0.2f, 0.2f)
                            scaleY *= 1f - (velocity * 0.25f).coerceIn(-0.2f, 0.2f)
                        }
                        .drawBackdrop(
                            backdrop = combinedBackdrop,
                            shape = { barShape },
                            effects = {
                                val progress = dragAnimation.pressProgress
                                lens(
                                    refractionHeight = 10.dp.toPx() * progress,
                                    refractionAmount = 14.dp.toPx() * progress,
                                    depthEffect = true,
                                    chromaticAberration = true,
                                )
                            },
                            highlight = {
                                Highlight.Default.copy(alpha = dragAnimation.pressProgress)
                            },
                            innerShadow = {
                                val progress = dragAnimation.pressProgress
                                InnerShadow(
                                    radius = 8.dp * progress,
                                    color = Color.Black.copy(alpha = 0.15f),
                                    alpha = progress,
                                )
                            },
                            onDrawSurface = {
                                val progress = dragAnimation.pressProgress
                                drawRect(selectionSurface, alpha = 1f - progress)
                                drawRect(Color.Black.copy(alpha = 0.03f * progress))
                            },
                        )
                        .height(GlassPillHeight)
                        .width(tabWidthDp),
            )
        }
    }
}

@Composable
private fun RowScope.GlassBottomBarItem(
    item: BottomNavItem,
    selected: Boolean,
    selectedColor: Color,
    unselectedColor: Color,
    onSelect: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val tabScale = LocalGlassBarTabScale.current
    val contentColor by
        animateColorAsState(
            targetValue = if (selected) selectedColor else unselectedColor,
            animationSpec = tween(180),
            label = "glassTabContentColor",
        )
    val select = onSelect

    Column(
        modifier =
            modifier
                .semantics(mergeDescendants = true) {
                    this.selected = selected
                    role = Role.Tab
                    onClick {
                        select()
                        true
                    }
                }
                .onKeyEvent { event ->
                    val activationKey =
                        event.key == Key.Enter ||
                            event.key == Key.NumPadEnter ||
                            event.key == Key.Spacebar
                    if (activationKey) {
                        if (event.type == KeyEventType.KeyUp) select()
                        true
                    } else {
                        false
                    }
                }
                .focusable()
                .fillMaxHeight()
                .weight(1f)
                .graphicsLayer {
                    val scale = tabScale()
                    scaleX = scale
                    scaleY = scale
                },
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(1.dp, Alignment.CenterVertically),
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
                modifier = Modifier.size(24.dp),
            )
        }

        Text(
            text = stringResource(item.titleRes),
            color = contentColor,
            style = MaterialTheme.typography.labelSmall,
            fontSize = 11.sp,
            lineHeight = 14.sp,
            fontWeight = FontWeight.Medium,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}
