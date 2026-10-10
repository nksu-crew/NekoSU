package me.nekosu.aqnya.util

import android.app.Activity
import android.os.Build
import android.view.Display

/**
 * 刷新率：MIUI/HyperOS 等 ROM 会按「每个 app」决定刷新率，没主动声明的应用常被
 * 压到 60Hz。这里让窗口显式偏好设备支持的最高刷新率，让管理器的界面保持高刷。
 */
object DisplayRefreshRate {
    /** 让 [activity] 的窗口偏好设备支持的最高刷新率；失败时静默忽略。 */
    fun requestHighest(activity: Activity) {
        val display = currentDisplay(activity) ?: return
        val best = runCatching { display.supportedModes.maxByOrNull { it.refreshRate } }.getOrNull() ?: return
        if (best.refreshRate <= 0f) return

        runCatching {
            activity.window.attributes =
                activity.window.attributes.apply {
                    preferredRefreshRate = best.refreshRate
                    // 只在同分辨率下切模式，避免顺带改变分辨率。
                    val current = display.mode
                    if (current == null || (best.width == current.width && best.height == current.height)) {
                        preferredDisplayModeId = best.modeId
                    }
                }
        }
    }

    private fun currentDisplay(activity: Activity): Display? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            activity.display
        } else {
            @Suppress("DEPRECATION")
            activity.windowManager.defaultDisplay
        }
}
