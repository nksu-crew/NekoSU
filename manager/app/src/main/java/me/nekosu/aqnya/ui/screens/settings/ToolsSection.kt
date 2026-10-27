package me.nekosu.aqnya.ui.screens.settings

import androidx.compose.foundation.clickable
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.BugReport
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import me.nekosu.aqnya.R
import me.nekosu.aqnya.ui.component.CardGroup
import me.nekosu.aqnya.ui.component.ListRow

@Composable
fun ToolsSection(onExportLog: () -> Unit) {
    CardGroup {
        item {
            ListRow(
                modifier = Modifier.clickable { onExportLog() },
                icon = { Icon(Icons.Rounded.BugReport, contentDescription = null) },
                headline = {
                    Text(
                        stringResource(R.string.export_log),
                    )
                },
                supporting = { Text(stringResource(R.string.export_log_describe)) },
            )
        }
    }
}
