package me.nekosu.aqnya.ui.component

import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect

/**
 * Scrolls [listState] back to the top whenever any of [keys] changes. Used for sort/filter/search
 * changes so the list doesn't appear to jump while the data is re-ordered.
 */
@Composable
fun ScrollToTopOnChange(
    listState: LazyListState,
    vararg keys: Any?,
    onScrolledToTop: () -> Unit = {},
    @Suppress("UNUSED_PARAMETER") isBusy: () -> Boolean = { false },
    @Suppress("UNUSED_PARAMETER") observedList: () -> Any?,
) {
    LaunchedEffect(*keys) {
        listState.scrollToItem(0)
        onScrolledToTop()
    }
}
