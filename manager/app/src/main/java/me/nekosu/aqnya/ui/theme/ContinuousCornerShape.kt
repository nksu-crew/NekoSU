// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.theme

import androidx.compose.foundation.shape.CornerBasedShape
import androidx.compose.foundation.shape.CornerSize
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.geometry.toRect
import androidx.compose.ui.graphics.Outline
import androidx.compose.ui.graphics.RectangleShape
import androidx.compose.ui.graphics.asComposePath
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import androidx.compose.ui.util.lerp
import androidx.graphics.shapes.CornerRounding
import androidx.graphics.shapes.RoundedPolygon
import androidx.graphics.shapes.rectangle
import androidx.graphics.shapes.toPath

/**
 * 边角从直边平滑过渡、而非以正圆弧相接的圆角矩形，即 Apple 风格的连续圆角（squircle）。
 *
 * 轮廓由 androidx.graphics.shapes 的平滑圆角生成：当某条边过短时它会先放弃平滑再退回圆角，
 * 因此高度一半的圆角仍能收成半圆。参照 Duck-Detector-Refactoring 的 ContinuousCornerShape。
 */
class ContinuousCornerShape(
    topStart: CornerSize,
    topEnd: CornerSize,
    bottomEnd: CornerSize,
    bottomStart: CornerSize,
) : CornerBasedShape(topStart, topEnd, bottomEnd, bottomStart) {
    override fun createOutline(
        size: Size,
        topStart: Float,
        topEnd: Float,
        bottomEnd: Float,
        bottomStart: Float,
        layoutDirection: LayoutDirection,
    ): Outline {
        if (size.isEmpty() || topStart + topEnd + bottomEnd + bottomStart == 0f) {
            return Outline.Rectangle(size.toRect())
        }
        val polygon = continuousRectangle(size, topStart, topEnd, bottomEnd, bottomStart, layoutDirection)
        return Outline.Generic(polygon.toPath().asComposePath())
    }

    override fun copy(
        topStart: CornerSize,
        topEnd: CornerSize,
        bottomEnd: CornerSize,
        bottomStart: CornerSize,
    ): ContinuousCornerShape = ContinuousCornerShape(topStart, topEnd, bottomEnd, bottomStart)

    override fun lerp(
        other: Any?,
        t: Float,
    ): Any? {
        val target =
            when (other) {
                null, RectangleShape -> ContinuousCornerShape(0.dp)
                is ContinuousCornerShape -> other
                else -> return null
            }
        return ContinuousCornerShape(
            topStart = lerp(topStart, target.topStart, t),
            topEnd = lerp(topEnd, target.topEnd, t),
            bottomEnd = lerp(bottomEnd, target.bottomEnd, t),
            bottomStart = lerp(bottomStart, target.bottomStart, t),
        )
    }

    override fun toString(): String =
        "ContinuousCornerShape(topStart = $topStart, topEnd = $topEnd, bottomEnd = $bottomEnd, " +
            "bottomStart = $bottomStart)"

    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is ContinuousCornerShape) return false
        return topStart == other.topStart &&
            topEnd == other.topEnd &&
            bottomEnd == other.bottomEnd &&
            bottomStart == other.bottomStart
    }

    override fun hashCode(): Int {
        var result = topStart.hashCode()
        result = 31 * result + topEnd.hashCode()
        result = 31 * result + bottomEnd.hashCode()
        result = 31 * result + bottomStart.hashCode()
        return result
    }
}

fun ContinuousCornerShape(radius: Dp): ContinuousCornerShape = ContinuousCornerShape(CornerSize(radius))

fun ContinuousCornerShape(corner: CornerSize): ContinuousCornerShape = ContinuousCornerShape(corner, corner, corner, corner)

fun ContinuousCornerShape(
    topStart: Dp = 0.dp,
    topEnd: Dp = 0.dp,
    bottomEnd: Dp = 0.dp,
    bottomStart: Dp = 0.dp,
): ContinuousCornerShape =
    ContinuousCornerShape(
        topStart = CornerSize(topStart),
        topEnd = CornerSize(topEnd),
        bottomEnd = CornerSize(bottomEnd),
        bottomStart = CornerSize(bottomStart),
    )

// Figma 的圆角平滑度以 60% 作为最接近 Apple 连续圆角的取值。
private const val CONTINUOUS_CORNER_SMOOTHING: Float = 0.6f

/** 位于原点的 [size] 矩形，每个角按各自半径做像素级圆角并平滑。 */
private fun continuousRectangle(
    size: Size,
    topStart: Float,
    topEnd: Float,
    bottomEnd: Float,
    bottomStart: Float,
    layoutDirection: LayoutDirection,
): RoundedPolygon {
    val ltr = layoutDirection == LayoutDirection.Ltr
    val topLeft = if (ltr) topStart else topEnd
    val topRight = if (ltr) topEnd else topStart
    val bottomRight = if (ltr) bottomEnd else bottomStart
    val bottomLeft = if (ltr) bottomStart else bottomEnd
    return RoundedPolygon.rectangle(
        width = size.width,
        height = size.height,
        // graphics-shapes 的矩形顶点自右下角开始列举。
        perVertexRounding =
            listOf(bottomRight, bottomLeft, topLeft, topRight).map { radius ->
                CornerRounding(radius = radius, smoothing = CONTINUOUS_CORNER_SMOOTHING)
            },
        centerX = size.width / 2f,
        centerY = size.height / 2f,
    )
}

private fun lerp(
    start: CornerSize,
    stop: CornerSize,
    fraction: Float,
): CornerSize =
    object : CornerSize {
        override fun toPx(
            shapeSize: Size,
            density: Density,
        ): Float =
            lerp(
                start.toPx(shapeSize, density),
                stop.toPx(shapeSize, density),
                fraction,
            )
    }
