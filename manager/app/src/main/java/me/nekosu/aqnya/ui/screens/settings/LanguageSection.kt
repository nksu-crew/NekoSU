package me.nekosu.aqnya.ui.screens.settings

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Check
import androidx.compose.material.icons.rounded.Translate
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import me.nekosu.aqnya.R
import me.nekosu.aqnya.ui.component.CardGroup
import me.nekosu.aqnya.ui.component.ListRow
import me.nekosu.aqnya.ui.theme.ContinuousCornerShape
import me.nekosu.aqnya.util.LocaleHelper

@Composable
fun LanguageSection(
    currentLangLabel: String,
    onLanguageChange: (String) -> Unit,
) {
    var menuExpanded by remember { mutableStateOf(false) }

    CardGroup {
        item {
            ListRow(
                modifier = Modifier.clickable { menuExpanded = true },
                icon = { Icon(Icons.Rounded.Translate, contentDescription = null) },
                headline = {
                    Text(
                        stringResource(R.string.language_title),
                    )
                },
                supporting = { Text(currentLangLabel) },
                trailing = {
                    Box {
                        DropdownMenu(
                            expanded = menuExpanded,
                            onDismissRequest = { menuExpanded = false },
                            shape = ContinuousCornerShape(16.dp),
                        ) {
                            LocaleHelper.availableLanguages.forEach { lang ->
                                DropdownMenuItem(
                                    text = {
                                        Text(
                                            stringResource(lang.labelRes),
                                            fontWeight =
                                                if (currentLangLabel ==
                                                    stringResource(lang.labelRes)
                                                ) {
                                                    FontWeight.SemiBold
                                                } else {
                                                    FontWeight.Normal
                                                },
                                        )
                                    },
                                    onClick = {
                                        menuExpanded = false
                                        onLanguageChange(lang.tag)
                                    },
                                    trailingIcon = {
                                        if (currentLangLabel == stringResource(lang.labelRes)) {
                                            Icon(Icons.Rounded.Check, null, Modifier.size(20.dp))
                                        }
                                    },
                                )
                            }
                        }
                    }
                },
            )
        }
    }
}
