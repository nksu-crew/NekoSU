package me.nekosu.aqnya.ui.component

import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight

/** 确认对话框。按钮文案为 null 时不显示该按钮。 */
@Composable
fun AppDialog(
    onDismissRequest: () -> Unit,
    title: String,
    modifier: Modifier = Modifier,
    confirmText: String? = null,
    onConfirm: (() -> Unit)? = null,
    dismissText: String? = null,
    content: @Composable () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismissRequest,
        modifier = modifier,
        title = {
            Text(
                text = title,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold,
            )
        },
        text = content,
        confirmButton = {
            if (confirmText != null) {
                TextButton(onClick = { onConfirm?.invoke() }) {
                    Text(confirmText)
                }
            }
        },
        dismissButton =
            dismissText?.let { label ->
                {
                    TextButton(onClick = onDismissRequest) {
                        Text(label)
                    }
                }
            },
    )
}
