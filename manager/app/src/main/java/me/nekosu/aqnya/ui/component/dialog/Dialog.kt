package me.nekosu.aqnya.ui.component.dialog

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties

interface ConfirmDialogHandle {
    fun showConfirm(
        title: String,
        content: String? = null,
        markdown: Boolean = false,
        html: Boolean = false,
        confirm: String? = null,
        dismiss: String? = null,
    )
}

interface LoadingDialogHandle {
    suspend fun <R> withLoading(block: suspend () -> R): R

    fun showLoading()
}

@Stable
private class ConfirmDialogState {
    var visible by mutableStateOf(false)
    var title by mutableStateOf("")
    var content by mutableStateOf<String?>(null)
    var confirm by mutableStateOf<String?>(null)
    var dismiss by mutableStateOf<String?>(null)
}

@Composable
fun rememberConfirmDialog(
    onConfirm: () -> Unit,
    onDismiss: () -> Unit,
): ConfirmDialogHandle {
    val state = remember { ConfirmDialogState() }
    val currentOnConfirm by rememberUpdatedState(onConfirm)
    val currentOnDismiss by rememberUpdatedState(onDismiss)

    if (state.visible) {
        AlertDialog(
            onDismissRequest = {
                state.visible = false
                currentOnDismiss()
            },
            title = { Text(state.title) },
            text = state.content?.let { body -> { Text(body) } },
            confirmButton = {
                TextButton(
                    onClick = {
                        state.visible = false
                        currentOnConfirm()
                    },
                ) {
                    Text(state.confirm ?: stringResource(android.R.string.ok))
                }
            },
            dismissButton = {
                TextButton(
                    onClick = {
                        state.visible = false
                        currentOnDismiss()
                    },
                ) {
                    Text(state.dismiss ?: stringResource(android.R.string.cancel))
                }
            },
        )
    }

    return remember(state) {
        object : ConfirmDialogHandle {
            override fun showConfirm(
                title: String,
                content: String?,
                markdown: Boolean,
                html: Boolean,
                confirm: String?,
                dismiss: String?,
            ) {
                state.title = title
                state.content = content
                state.confirm = confirm
                state.dismiss = dismiss
                state.visible = true
            }
        }
    }
}

@Stable
private class LoadingDialogState {
    var visible by mutableStateOf(false)
}

@Composable
fun rememberLoadingDialog(): LoadingDialogHandle {
    val state = remember { LoadingDialogState() }

    if (state.visible) {
        Dialog(
            onDismissRequest = {},
            properties =
                DialogProperties(
                    dismissOnClickOutside = false,
                    dismissOnBackPress = false,
                ),
        ) {
            Surface(modifier = Modifier.size(100.dp), shape = MaterialTheme.shapes.extraLarge) {
                Box(contentAlignment = Alignment.Center) {
                    CircularProgressIndicator()
                }
            }
        }
    }

    return remember(state) {
        object : LoadingDialogHandle {
            override fun showLoading() {
                state.visible = true
            }

            override suspend fun <R> withLoading(block: suspend () -> R): R {
                state.visible = true
                return try {
                    block()
                } finally {
                    state.visible = false
                }
            }
        }
    }
}
