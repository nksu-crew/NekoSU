package me.nekosu.aqnya.ui.animation

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.layout.Box
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import me.nekosu.aqnya.ui.navigation.BottomNavItem

/**
 * 带动画的底部导航栏
 */
@Composable
fun AnimatedBottomNavBar(
    visible: Boolean,
    items: List<BottomNavItem>,
    selectedIndex: Int,
    onTabClick: (Int) -> Unit,
    modifier: Modifier = Modifier,
    content: @Composable (List<BottomNavItem>, Int, (Int) -> Unit) -> Unit,
) {
    FadeSlideNavBar(
        visible = visible,
        modifier = modifier,
    ) {
        content(items, selectedIndex, onTabClick)
    }
}

@Composable
private fun FadeSlideNavBar(
    visible: Boolean,
    modifier: Modifier = Modifier,
    content: @Composable () -> Unit,
) {
    val motionScheme = MaterialTheme.motionScheme
    val target = if (visible) 1f else 0f

    val animatedAlpha by animateFloatAsState(
        targetValue = target,
        animationSpec =
            if (visible) {
                motionScheme.defaultEffectsSpec<Float>()
            } else {
                motionScheme.fastEffectsSpec<Float>()
            },
        label = "fadeSlideNavBarAlpha",
    )

    val animatedTranslation by animateFloatAsState(
        targetValue = target,
        animationSpec =
            if (visible) {
                motionScheme.defaultSpatialSpec<Float>()
            } else {
                motionScheme.fastSpatialSpec<Float>()
            },
        label = "fadeSlideNavBarTranslation",
    )

    Box(
        modifier =
            modifier.graphicsLayer {
                alpha = animatedAlpha
                translationY = (1f - animatedTranslation) * size.height
            },
    ) {
        content()
    }
}
