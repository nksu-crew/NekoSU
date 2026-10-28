// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.navigation

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.spring
import androidx.compose.foundation.MutatorMutex
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.runtime.snapshotFlow
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.pointer.AwaitPointerEventScope
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.PointerId
import androidx.compose.ui.input.pointer.PointerInputChange
import androidx.compose.ui.input.pointer.PointerInputScope
import androidx.compose.ui.input.pointer.changedToUpIgnoreConsumed
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChangeIgnoreConsumed
import androidx.compose.ui.input.pointer.util.VelocityTracker
import androidx.compose.ui.unit.IntSize
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlin.math.abs
import kotlin.time.TimeSource

/**
 * Spring-driven selection value with press progress, used by the floating glass bar.
 *
 * Ported from KernelSU's `FloatingBottomBar` (GPL-3.0-or-later), which adapts the
 * compose-miuix-ui `IosLiquidGlassNavigationBar` example.
 */
internal class DampedDragAnimation(
    private val animationScope: CoroutineScope,
    initialValue: Float,
    private val valueRange: ClosedRange<Float>,
    private val visibilityThreshold: Float,
    private val initialScale: Float,
    private val pressedScale: Float,
    private val canDrag: (Offset) -> Boolean = { true },
    private val onDragStarted: DampedDragAnimation.(position: Offset) -> Unit,
    private val onDragStopped: DampedDragAnimation.() -> Unit,
    private val onDragCancelled: DampedDragAnimation.() -> Unit = onDragStopped,
    private val onDrag: DampedDragAnimation.(size: IntSize, dragAmount: Offset) -> Unit,
) {
    private val valueAnimationSpec = spring(1f, 1000f, visibilityThreshold)
    private val velocityAnimationSpec = spring(0.5f, 300f, visibilityThreshold * 10f)
    private val pressProgressAnimationSpec = spring(1f, 1000f, 0.001f)
    private val scaleXAnimationSpec = spring(0.6f, 250f, 0.001f)
    private val scaleYAnimationSpec = spring(0.7f, 250f, 0.001f)

    private val valueAnimation = Animatable(initialValue, visibilityThreshold)
    private val velocityAnimation = Animatable(0f, 5f)
    private val pressProgressAnimation = Animatable(0f, 0.001f)
    private val scaleXAnimation = Animatable(initialScale, 0.001f)
    private val scaleYAnimation = Animatable(initialScale, 0.001f)

    private val mutatorMutex = MutatorMutex()

    private var pressJob: Job? = null
    private var releaseJob: Job? = null

    private val velocityTracker = VelocityTracker()
    private val startMark = TimeSource.Monotonic.markNow()

    val value: Float get() = valueAnimation.value
    val targetValue: Float get() = valueAnimation.targetValue
    val pressProgress: Float get() = pressProgressAnimation.value
    val scaleX: Float get() = scaleXAnimation.value
    val scaleY: Float get() = scaleYAnimation.value
    val velocity: Float get() = velocityAnimation.value

    val modifier: Modifier =
        Modifier.pointerInput(Unit) {
            inspectDragGestures(
                onDragStart = { down ->
                    onDragStarted(down.position)
                    press()
                },
                onDragEnd = {
                    onDragStopped()
                    release()
                },
                onDragCancel = {
                    onDragCancelled()
                    release()
                },
            ) { change, dragAmount ->
                val isInside = canDrag(change.position)
                val wasInside = canDrag(change.previousPosition)
                if (isInside && wasInside) {
                    onDrag(size, dragAmount)
                }
            }
        }

    fun press() {
        releaseJob?.cancel()
        pressJob?.cancel()
        velocityTracker.resetTracking()
        pressJob =
            animationScope.launch {
                launch { pressProgressAnimation.animateTo(1f, pressProgressAnimationSpec) }
                launch { scaleXAnimation.animateTo(pressedScale, scaleXAnimationSpec) }
                launch { scaleYAnimation.animateTo(pressedScale, scaleYAnimationSpec) }
            }
    }

    fun release() {
        releaseJob?.cancel()
        releaseJob =
            animationScope.launch {
                withFrameNanos { }
                if (value != targetValue) {
                    val threshold = (valueRange.endInclusive - valueRange.start) * 0.025f
                    snapshotFlow { valueAnimation.value }
                        .first { abs(it - valueAnimation.targetValue) < threshold }
                }
                launch { pressProgressAnimation.animateTo(0f, pressProgressAnimationSpec) }
                launch { scaleXAnimation.animateTo(initialScale, scaleXAnimationSpec) }
                launch { scaleYAnimation.animateTo(initialScale, scaleYAnimationSpec) }
            }
    }

    fun updateValue(value: Float) {
        val targetValue = value.coerceIn(valueRange)
        animationScope.launch(start = CoroutineStart.UNDISPATCHED) {
            valueAnimation.animateTo(targetValue, valueAnimationSpec) { updateVelocity() }
        }
    }

    fun animateToValue(value: Float) {
        animationScope.launch {
            mutatorMutex.mutate {
                press()
                val targetValue = value.coerceIn(valueRange)
                launch { valueAnimation.animateTo(targetValue, valueAnimationSpec) }
                if (velocity != 0f) {
                    launch { velocityAnimation.animateTo(0f, velocityAnimationSpec) }
                }
                release()
            }
        }
    }

    private fun updateVelocity() {
        velocityTracker.addPosition(
            startMark.elapsedNow().inWholeMilliseconds,
            Offset(value, 0f),
        )
        val span = (valueRange.endInclusive - valueRange.start).coerceAtLeast(1e-6f)
        val targetVelocity = velocityTracker.calculateVelocity().x / span
        animationScope.launch(start = CoroutineStart.UNDISPATCHED) {
            velocityAnimation.snapTo(targetVelocity)
        }
    }
}

/**
 * Drag inspector that reports the initial press as well, so a tap behaves like a
 * zero-length drag (down selects, up commits).
 */
internal suspend fun PointerInputScope.inspectDragGestures(
    onDragStart: (down: PointerInputChange) -> Unit = {},
    onDragEnd: (change: PointerInputChange) -> Unit = {},
    onDragCancel: () -> Unit = {},
    onDrag: (change: PointerInputChange, dragAmount: Offset) -> Unit,
) {
    awaitEachGesture {
        val down =
            awaitFirstDown(
                requireUnconsumed = false,
                pass = PointerEventPass.Initial,
            )
        onDragStart(down)
        onDrag(down, Offset.Zero)
        val upEvent =
            awaitDragOrUpEvent(
                pointerId = down.id,
                onDrag = { onDrag(it, it.positionChangeIgnoreConsumed()) },
            )
        if (upEvent != null) onDragEnd(upEvent) else onDragCancel()
    }
}

private suspend inline fun AwaitPointerEventScope.awaitDragOrUpEvent(
    pointerId: PointerId,
    onDrag: (PointerInputChange) -> Unit,
): PointerInputChange? {
    val isPointerUp =
        currentEvent.changes.firstOrNull { it.id == pointerId }?.pressed != true
    if (isPointerUp) return null
    var pointer = pointerId
    while (true) {
        val change = awaitDragOrUp(pointer) ?: return null
        if (change.changedToUpIgnoreConsumed()) {
            return change
        }
        onDrag(change)
        pointer = change.id
    }
}

private suspend inline fun AwaitPointerEventScope.awaitDragOrUp(pointerId: PointerId): PointerInputChange? {
    var pointer = pointerId
    while (true) {
        val event = awaitPointerEvent(PointerEventPass.Initial)
        val dragEvent = event.changes.firstOrNull { it.id == pointer } ?: return null
        if (dragEvent.changedToUpIgnoreConsumed()) {
            val otherDown = event.changes.firstOrNull { it.pressed }
            if (otherDown == null) {
                return dragEvent
            } else {
                pointer = otherDown.id
            }
        } else if (dragEvent.previousPosition != dragEvent.position) {
            return dragEvent
        }
    }
}
