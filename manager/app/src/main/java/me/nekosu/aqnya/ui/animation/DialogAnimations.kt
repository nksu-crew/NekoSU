// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.animation

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.EnterTransition
import androidx.compose.animation.ExitTransition
import androidx.compose.animation.core.CubicBezierEasing
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.material3.AlertDialog
import androidx.compose.runtime.Composable

/**
 * Dialog animation configuration, matching the Flutter manager's dialogs.
 *
 * Flutter's `showDialog` (DialogRoute) cross-fades the dialog over 150 ms with
 * `Curves.easeOut` on both enter and exit; there is no scale animation.
 */
object DialogAnimations {
    private const val DurationMillis = 150

    /** Flutter's `Curves.easeOut`: Cubic(0.0, 0.0, 0.58, 1.0). */
    private val easing = CubicBezierEasing(0f, 0f, 0.58f, 1f)

    fun createEnterAnimation(): EnterTransition =
        fadeIn(animationSpec = tween(DurationMillis, easing = easing))

    fun createExitAnimation(): ExitTransition =
        fadeOut(animationSpec = tween(DurationMillis, easing = easing))
}

/**
 * 带动画的 AlertDialog
 */
@Composable
fun AnimatedAlertDialog(
    visible: Boolean,
    onDismiss: () -> Unit,
    title: @Composable () -> Unit,
    text: @Composable () -> Unit,
    confirmButton: @Composable () -> Unit,
    dismissButton: @Composable (() -> Unit)? = null,
) {
    AnimatedVisibility(
        visible = visible,
        enter = DialogAnimations.createEnterAnimation(),
        exit = DialogAnimations.createExitAnimation(),
    ) {
        AlertDialog(
            onDismissRequest = onDismiss,
            title = title,
            text = text,
            confirmButton = confirmButton,
            dismissButton = dismissButton?.let { btn -> { btn() } },
        )
    }
}
