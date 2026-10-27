package me.nekosu.aqnya.ui.component

import androidx.compose.foundation.clickable
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedListItem
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip

@Composable
fun AppListItem(
    title: String,
    modifier: Modifier = Modifier,
    summary: String? = null,
    leading: @Composable (() -> Unit)? = null,
    trailing: @Composable (() -> Unit)? = null,
    onClick: (() -> Unit)? = null,
) {
    val shapes = currentListItemShapes()
    SegmentedListItem(
        // Clip to the segmented shape *before* the clickable: the item's
        // Surface applies its rounded shape inside this modifier chain, so
        // without the clip the press ripple is drawn over the square bounds.
        modifier =
            modifier.then(
                if (onClick != null) Modifier.clip(shapes.shape).clickable { onClick() } else Modifier,
            ),
        shapes = shapes,
        colors = ListItemDefaults.segmentedColors(containerColor = MaterialTheme.colorScheme.surfaceBright),
        leadingContent = leading,
        supportingContent = summary?.let { text -> { Text(text) } },
        trailingContent = trailing,
        content = { Text(title) },
    )
}
