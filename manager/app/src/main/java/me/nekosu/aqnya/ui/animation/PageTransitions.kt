// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.animation

import androidx.compose.animation.EnterTransition
import androidx.compose.animation.ExitTransition
import androidx.compose.animation.core.CubicBezierEasing
import androidx.compose.animation.core.tween
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally

/**
 * Compose counterpart of the Flutter manager's page transitions.
 *
 * Flutter uses `CupertinoPageTransitionsBuilder` for Android/iOS: the incoming
 * page slides in from the right while the outgoing page slides a third of the
 * width to the left. Popping reverses both. The Material route runs for 300 ms
 * with `Curves.fastEaseInToSlowEaseOut` on the primary route and
 * `Curves.linearToEaseOut` on the secondary route.
 */
object PageTransitions {
    private const val DurationMillis = 300

    /** Approximation of `Curves.fastEaseInToSlowEaseOut`. */
    private val primaryEasing = CubicBezierEasing(0.4f, 0f, 0.2f, 1f)

    /** Flutter's `Curves.linearToEaseOut`: Cubic(0.35, 0.91, 0.33, 0.97). */
    private val secondaryEasing = CubicBezierEasing(0.35f, 0.91f, 0.33f, 0.97f)

    fun enter(): EnterTransition =
        slideInHorizontally(
            animationSpec = tween(DurationMillis, easing = primaryEasing),
            initialOffsetX = { it },
        )

    fun exit(): ExitTransition =
        slideOutHorizontally(
            animationSpec = tween(DurationMillis, easing = secondaryEasing),
            targetOffsetX = { -it / 3 },
        )

    fun popEnter(): EnterTransition =
        slideInHorizontally(
            animationSpec = tween(DurationMillis, easing = secondaryEasing),
            initialOffsetX = { -it / 3 },
        )

    fun popExit(): ExitTransition =
        slideOutHorizontally(
            animationSpec = tween(DurationMillis, easing = primaryEasing),
            targetOffsetX = { it },
        )
}
