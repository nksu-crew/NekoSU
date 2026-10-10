package me.nekosu.aqnya.ui.component

import androidx.compose.foundation.layout.RowScope
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.TopAppBarScrollBehavior
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import dev.chrisbanes.haze.HazeInput
import dev.chrisbanes.haze.HazePerformanceMode
import dev.chrisbanes.haze.HazeProgressive
import dev.chrisbanes.haze.HazeState
import dev.chrisbanes.haze.blur.HazeBlurStyle
import dev.chrisbanes.haze.blur.HazeColorEffect
import dev.chrisbanes.haze.blur.hazeBlur

/** 顶栏渐变模糊的共享样式。 */
@Composable
fun rememberHazeAppBarStyle(): HazeBlurStyle {
    val scrim = MaterialTheme.colorScheme.surface.copy(alpha = 0.55f)
    return remember(scrim) {
        HazeBlurStyle {
            blurRadius(24.dp)
            colorEffects(listOf(HazeColorEffect.tint(scrim)))
            progressive(HazeProgressive.verticalGradient(startIntensity = 1f, endIntensity = 0f))
        }
    }
}

/**
 * 顶栏：下方内容滚过时铺一层自上而下渐隐的模糊。
 *
 * 页面需要把滚动内容用 `Modifier.hazeSource(state)` 标记，并让内容绘制到顶栏下方
 * （即把 `innerPadding` 顶部那部分当作滚动内容的内边距，而不是滚动视口的内边距）。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HazeTopAppBar(
    state: HazeState,
    title: @Composable () -> Unit,
    modifier: Modifier = Modifier,
    navigationIcon: @Composable () -> Unit = {},
    actions: @Composable RowScope.() -> Unit = {},
    scrollBehavior: TopAppBarScrollBehavior? = null,
) {
    val style = rememberHazeAppBarStyle()
    TopAppBar(
        title = title,
        modifier =
            modifier.hazeBlur(
                input = HazeInput.Sources(state),
                style = style,
                performanceMode = HazePerformanceMode.Performance,
            ),
        navigationIcon = navigationIcon,
        actions = actions,
        scrollBehavior = scrollBehavior,
        colors =
            TopAppBarDefaults.topAppBarColors(
                containerColor = Color.Transparent,
                scrolledContainerColor = Color.Transparent,
            ),
    )
}
