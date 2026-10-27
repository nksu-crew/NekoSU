// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.component

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.ListItemShapes
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedListItem
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import me.nekosu.aqnya.ui.theme.ContinuousCornerShape

val LocalListItemShapes = compositionLocalOf<ListItemShapes?> { null }

@Composable
fun currentListItemShapes(): ListItemShapes = LocalListItemShapes.current ?: ListItemDefaults.segmentedShapes(index = 0, count = 1)

fun groupShape(
    index: Int,
    total: Int,
    radius: Dp = 20.dp,
    innerRadius: Dp = 4.dp,
): Shape =
    when {
        total <= 1 ->
            ContinuousCornerShape(radius)

        index == 0 ->
            ContinuousCornerShape(
                topStart = radius,
                topEnd = radius,
                bottomStart = innerRadius,
                bottomEnd = innerRadius,
            )

        index == total - 1 ->
            ContinuousCornerShape(
                topStart = innerRadius,
                topEnd = innerRadius,
                bottomStart = radius,
                bottomEnd = radius,
            )

        else ->
            ContinuousCornerShape(innerRadius)
    }

@DslMarker
annotation class CardGroupDsl

internal class CardGroupEntry(
    val content: @Composable () -> Unit,
)

@CardGroupDsl
class CardGroupScope internal constructor() {
    internal val entries = mutableListOf<CardGroupEntry>()

    fun item(
        visible: Boolean = true,
        content: @Composable () -> Unit,
    ) {
        if (visible) entries += CardGroupEntry(content)
    }
}

@Composable
fun CardGroup(
    modifier: Modifier = Modifier,
    content: CardGroupScope.() -> Unit,
) {
    val entries = CardGroupScope().apply(content).entries
    if (entries.isEmpty()) return

    Column(
        modifier = modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(ListItemDefaults.SegmentedGap),
    ) {
        entries.forEachIndexed { index, entry ->
            CompositionLocalProvider(
                LocalListItemShapes provides ListItemDefaults.segmentedShapes(index = index, count = entries.size),
            ) {
                entry.content()
            }
        }
    }
}

@Composable
fun CardItem(
    modifier: Modifier = Modifier,
    shape: Shape? = null,
    containerColor: Color = MaterialTheme.colorScheme.surfaceBright,
    content: @Composable () -> Unit,
) {
    Surface(
        modifier = modifier.fillMaxWidth(),
        shape = shape ?: LocalListItemShapes.current?.shape ?: ContinuousCornerShape(20.dp),
        color = containerColor,
        content = content,
    )
}

@Composable
fun ListRow(
    modifier: Modifier = Modifier,
    icon: @Composable () -> Unit,
    headline: @Composable () -> Unit,
    supporting: @Composable (() -> Unit)? = null,
    trailing: @Composable () -> Unit = {},
) {
    val shapes = currentListItemShapes()
    SegmentedListItem(
        // Clip to the segmented shape first: callers attach clickable()/
        // toggleable() through `modifier`, and the item's Surface applies its
        // rounded shape only inside this chain -- so the press ripple would
        // otherwise spill over the square corners.
        modifier = Modifier.clip(shapes.shape).then(modifier),
        shapes = shapes,
        colors = ListItemDefaults.segmentedColors(containerColor = MaterialTheme.colorScheme.surfaceBright),
        leadingContent = icon,
        supportingContent = supporting,
        trailingContent = trailing,
        content = headline,
    )
}
