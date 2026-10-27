// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya

import android.os.Bundle
import android.util.Log
import android.view.MotionEvent
import androidx.activity.ComponentActivity

/**
 * Common entry point for activities that must not accept taps through another
 * window.  Hiding untrusted overlays is the first line of defence; filtering
 * obscured motion events also covers overlays the system still reports to us.
 */
abstract class SecureComponentActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.setHideOverlayWindows(true)
    }

    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        if ((ev.flags and OBSCURED_FLAGS) != 0) {
            if (ev.actionMasked == MotionEvent.ACTION_DOWN) {
                Log.w(TAG, "blocked touch while the window is obscured")
            }
            return true
        }
        return super.dispatchTouchEvent(ev)
    }

    private companion object {
        const val TAG = "NekosuTapGuard"
        const val OBSCURED_FLAGS =
            MotionEvent.FLAG_WINDOW_IS_OBSCURED or MotionEvent.FLAG_WINDOW_IS_PARTIALLY_OBSCURED
    }
}
