package me.nekosu.aqnya.ui.screens

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import me.nekosu.aqnya.R
import me.nekosu.aqnya.ui.animation.AnimatedAlertDialog

/**
 * 安装对话框的可观察状态。
 */
data class InstallState(
    val vendorBootUri: Uri? = null,
    val vendorBootName: String? = null,
    val koCandidates: List<String> = emptyList(),
    val selectedKo: String? = null,
    val running: Boolean = false,
    val done: Boolean = false,
    val success: Boolean? = null,
    val message: String = "",
)

/**
 * 安装对话框: 选择 vendor_boot.img, 执行 patching, 展示进度与结果。
 */
@Composable
fun InstallDialog(
    show: Boolean,
    state: InstallState,
    onDismiss: () -> Unit,
    onPickVendorBoot: (Uri) -> Unit,
    onSelectKo: (String) -> Unit,
    onStart: () -> Unit,
) {
    val picker =
        rememberLauncherForActivityResult(
            contract = ActivityResultContracts.OpenDocument(),
        ) { uri ->
            if (uri != null) onPickVendorBoot(uri)
        }

    AnimatedAlertDialog(
        visible = show,
        onDismiss = { if (!state.running) onDismiss() },
        title = {
            Text(
                text = stringResource(R.string.install_title),
                style = TextStyle(fontSize = 16.sp, fontWeight = FontWeight.SemiBold),
            )
        },
        text = {
            Column(
                modifier =
                    Modifier
                        .verticalScroll(rememberScrollState())
                        .fillMaxWidth(),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Text(
                    text = stringResource(R.string.install_desc),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f),
                )

                // vendor_boot 选择
                Column {
                    Text(
                        text = stringResource(R.string.install_pick_vendor_boot),
                        style = MaterialTheme.typography.labelLarge,
                    )
                    Spacer(Modifier.height(4.dp))
                    OutlinedButton(
                        onClick = { picker.launch(arrayOf("*/*")) },
                        enabled = !state.running,
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Text(state.vendorBootName ?: stringResource(R.string.install_pick_vendor_boot))
                    }
                    Text(
                        text = stringResource(R.string.install_pick_vendor_boot_desc),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.outline,
                    )
                }

                // KMI 选择
                if (state.koCandidates.isNotEmpty()) {
                    Column {
                        Text(
                            text = stringResource(R.string.install_select_ko),
                            style = MaterialTheme.typography.labelLarge,
                        )
                        Spacer(Modifier.height(4.dp))
                        state.koCandidates.forEach { ko ->
                            val selected = ko == state.selectedKo
                            TextButton(
                                onClick = { onSelectKo(ko) },
                                enabled = !state.running,
                            ) {
                                Text(
                                    text = (if (selected) "● " else "○ ") + ko,
                                    fontFamily = FontFamily.Monospace,
                                    fontSize = 13.sp,
                                )
                            }
                        }
                    }
                }

                // 进度
                if (state.running) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        CircularProgressIndicator(modifier = Modifier.size(18.dp))
                        Spacer(Modifier.size(8.dp))
                        Text(stringResource(R.string.install_working))
                    }
                    LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                }

                if (state.message.isNotBlank()) {
                    Text(
                        text = state.message,
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                        color =
                            if (state.success == false) {
                                MaterialTheme.colorScheme.error
                            } else {
                                MaterialTheme.colorScheme.onSurface.copy(alpha = 0.8f)
                            },
                        modifier =
                            Modifier
                                .fillMaxWidth()
                                .heightIn(max = 180.dp)
                                .verticalScroll(rememberScrollState()),
                    )
                }
            }
        },
        confirmButton = {
            if (state.done) {
                Button(onClick = onDismiss) {
                    Text(stringResource(R.string.install_done))
                }
            } else {
                Button(
                    onClick = onStart,
                    enabled = !state.running && state.vendorBootUri != null && state.selectedKo != null,
                ) {
                    Text(stringResource(R.string.install_start))
                }
            }
        },
        dismissButton = {
            if (!state.done) {
                TextButton(onClick = onDismiss, enabled = !state.running) {
                    Text(stringResource(R.string.dialog_cancel))
                }
            }
        },
    )
}
