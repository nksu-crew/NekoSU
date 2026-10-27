package me.nekosu.aqnya.ui.component

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.CheckCircle
import androidx.compose.material.icons.rounded.SystemUpdate
import androidx.compose.material.icons.rounded.Warning
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import me.nekosu.aqnya.R
import me.nekosu.aqnya.core.model.InstallStatus
import me.nekosu.aqnya.ui.theme.ContinuousCornerShape

@Composable
fun StatusCard(
    state: InstallStatus,
    isGki: Boolean,
    managerVersion: String,
    onClick: () -> Unit,
) {
    val isWorking = state == InstallStatus.INSTALLED
    val isUpdate = state == InstallStatus.NEED_UPDATE || state == InstallStatus.NEED_REBOOT

    val containerColor =
        when {
            isWorking -> MaterialTheme.colorScheme.secondaryContainer
            isUpdate -> MaterialTheme.colorScheme.tertiaryContainer
            else -> MaterialTheme.colorScheme.errorContainer
        }
    val contentColor =
        when {
            isWorking -> MaterialTheme.colorScheme.onSecondaryContainer
            isUpdate -> MaterialTheme.colorScheme.onTertiaryContainer
            else -> MaterialTheme.colorScheme.onErrorContainer
        }
    val icon =
        when {
            isWorking -> Icons.Rounded.CheckCircle
            isUpdate -> Icons.Rounded.SystemUpdate
            else -> Icons.Rounded.Warning
        }
    val title =
        when {
            isWorking -> stringResource(R.string.running)
            isUpdate -> stringResource(R.string.status_card_need_update)
            else -> stringResource(R.string.status_card_not_installed)
        }

    AppCard(
        modifier = Modifier.fillMaxWidth(),
        onClick = onClick,
        shape = ContinuousCornerShape(28.dp),
        containerColor = containerColor,
        contentColor = contentColor,
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(20.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(icon, contentDescription = title, modifier = Modifier.size(32.dp))

            Spacer(Modifier.width(16.dp))

            Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleMediumEmphasized,
                )
                Text(
                    text =
                        if (isWorking) {
                            managerVersion
                        } else {
                            stringResource(R.string.status_card_click_to_install)
                        },
                    style = MaterialTheme.typography.bodyMedium,
                    color = contentColor.copy(alpha = 0.8f),
                )
            }

            ModeLabel(
                label = if (isGki) "GKI" else "LKM",
                color = contentColor,
                containerColor = contentColor.copy(alpha = 0.16f),
            )
        }
    }
}

@Composable
fun ModeLabel(
    label: String,
    modifier: Modifier = Modifier,
    color: Color = MaterialTheme.colorScheme.onPrimary,
    containerColor: Color = MaterialTheme.colorScheme.primary,
) {
    Box(
        modifier =
            modifier
                .background(color = containerColor, shape = RoundedCornerShape(50))
                .padding(horizontal = 9.dp, vertical = 4.dp),
    ) {
        Text(
            text = label,
            style =
                MaterialTheme.typography.labelMedium.copy(
                    fontWeight = FontWeight.SemiBold,
                ),
            color = color,
            maxLines = 1,
        )
    }
}
