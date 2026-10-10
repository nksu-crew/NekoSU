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

/**
 * Haze 顶栏模糊的共享样式：自上而下逐渐消散的模糊，再叠一层表面色，
 * 避免标题糊在滚动内容上。
 *
 * 配合 `Modifier.hazeSource(state)` 使用，见 [HazeTopAppBar]。
 */
@Composable
fun rememberHazeAppBarStyle(): HazeBlurStyle {
    val scrim = MaterialTheme.colorScheme.surface.copy(alpha = 0.55f)
    // 不支持 RenderEffect 模糊的设备（API < 31）走这个不透明兜底，避免顶栏透明后标题压在内容上。
    val fallback = MaterialTheme.colorScheme.surface.copy(alpha = 0.95f)
    return remember(scrim, fallback) {
        HazeBlurStyle {
            blurRadius(24.dp)
            colorEffects(listOf(HazeColorEffect.tint(scrim)))
            fallbackColorEffect(HazeColorEffect.tint(fallback))
            progressive(
                HazeProgressive.verticalGradient(
                    startIntensity = 1f,
                    endIntensity = 0f,
                ),
            )
        }
    }
}

/**
 * 顶栏：下方内容滚过时，用 Haze 在栏上铺一层 [rememberHazeAppBarStyle] 的渐变模糊。
 *
 * 想看到效果，页面需要做两件事：
 * 1. 用 `Modifier.hazeSource(state)` 标记滚动内容，并复用同一个 [state]；
 * 2. 让内容绘制到顶栏下方——即把 `innerPadding` 顶部那部分当作滚动内容的
 *    内边距，而不是滚动视口的内边距，否则内容永远不会经过顶栏。
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
                // 顶栏模糊每帧都要抓一层全屏内容，用低保真档换帧率。
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
