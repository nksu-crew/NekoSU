package me.nekosu.aqnya.ui.component

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.RowScope
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.TopAppBarScrollBehavior
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import dev.chrisbanes.haze.HazeInput
import dev.chrisbanes.haze.HazePerformanceMode
import dev.chrisbanes.haze.HazeProgressive
import dev.chrisbanes.haze.HazeState
import dev.chrisbanes.haze.blur.HazeBlurStyle
import dev.chrisbanes.haze.blur.HazeColorEffect
import dev.chrisbanes.haze.blur.hazeBlur
import me.nekosu.aqnya.data.prefs.AppPreferences

/** 顶栏渐变模糊的共享样式。 */
@Composable
fun rememberHazeAppBarStyle(blurEnabled: Boolean = true): HazeBlurStyle {
    // 提高一点不透明度，模糊透出的内容才不会糊成一团脏灰。
    val tint = MaterialTheme.colorScheme.surfaceContainer.copy(alpha = 0.78f)
    val solidTint = MaterialTheme.colorScheme.surfaceContainer
    return remember(tint, solidTint, blurEnabled) {
        HazeBlurStyle {
            if (!blurEnabled) {
                this.blurEnabled(false)
            }
            blurRadius(18.dp)
            // haze 默认会在模糊上叠 0.15 的颗粒噪点，浅色内容上看着很脏，这里关掉。
            noiseFactor(0f)
            colorEffects(listOf(HazeColorEffect.tint(tint)))
            if (blurEnabled) {
                progressive(HazeProgressive.verticalGradient(startIntensity = 1f, endIntensity = 0f))
            } else {
                // 关闭模糊时由 scrim delegate 绘制纯色背景，避免透明帧。
                fallbackColorEffect(HazeColorEffect.tint(solidTint))
            }
        }
    }
}

/** 同步读取当前值作为初值，避免进入页面时先闪一帧无模糊状态。 */
@Composable
fun rememberBlurEnabled(): Boolean {
    val context = LocalContext.current
    val flow = remember(context) { AppPreferences.blurFlow(context) }
    val initial = remember(context) { AppPreferences.isBlurEnabled(context) }
    return flow.collectAsState(initial = initial).value
}

/**
 * 顶栏：滚过时铺一层自上而下渐隐的模糊。
 *
 * 页面需要把滚动内容用 `Modifier.hazeSource(state)` 标记，并让内容绘制到顶栏下方。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HazeTopAppBar(
    state: HazeState,
    title: String,
    modifier: Modifier = Modifier,
    navigationIcon: @Composable () -> Unit = {},
    actions: @Composable RowScope.() -> Unit = {},
    scrollBehavior: TopAppBarScrollBehavior? = null,
    blurEnabled: Boolean? = null,
) {
    val enabled = blurEnabled ?: rememberBlurEnabled()
    val style = rememberHazeAppBarStyle(enabled)
    TopAppBar(
        title = { Text(title) },
        modifier =
            modifier
                .background(
                    if (enabled) {
                        Color.Transparent
                    } else {
                        MaterialTheme.colorScheme.surfaceContainer
                    },
                ).hazeBlur(
                    input = HazeInput.Sources(state),
                    style = style,
                    // Performance(=Fixed(0f)) 会把模糊输入压到最低分辨率，糊成块状反而显脏；
                    // 用默认的 Balanced 品质，配合更小的半径兼顾观感与开销。
                    performanceMode = HazePerformanceMode.Default,
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
