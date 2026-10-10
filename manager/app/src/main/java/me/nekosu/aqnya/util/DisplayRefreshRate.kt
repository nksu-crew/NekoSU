package me.nekosu.aqnya.util

import android.app.Activity

/**
 * MIUI/HyperOS 会按应用决定刷新率，没主动声明的应用常被压到 60Hz。
 * 这里让窗口显式偏好设备支持的最高刷新率。
 */
object DisplayRefreshRate {
    fun requestHighest(activity: Activity) {
        val display = activity.display ?: return
        val best = display.supportedModes.maxByOrNull { it.refreshRate } ?: return
        if (best.refreshRate <= 0f) return

        activity.window.attributes =
            activity.window.attributes.apply {
                preferredRefreshRate = best.refreshRate
                // 只在同分辨率下切模式，避免顺带改变分辨率。
                val current = display.mode
                if (current == null ||
                    (
                        best.physicalWidth == current.physicalWidth &&
                            best.physicalHeight == current.physicalHeight
                    )
                ) {
                    preferredDisplayModeId = best.modeId
                }
            }
    }
}
