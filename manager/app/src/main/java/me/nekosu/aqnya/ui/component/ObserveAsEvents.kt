package me.nekosu.aqnya.ui.component

import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import kotlinx.coroutines.flow.Flow

/**
 * One-shot event collection bound to the current composition. Pair with
 * `Channel(BUFFERED).receiveAsFlow()` so events emitted while nothing is collecting are buffered.
 */
@Composable
fun <T> ObserveAsEvents(
    events: Flow<T>,
    onEvent: suspend (T) -> Unit,
) {
    LaunchedEffect(events) {
        events.collect { onEvent(it) }
    }
}
