// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.component

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material.icons.rounded.Save
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExtendedFloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import me.nekosu.aqnya.R

/**
 * 整屏日志视图：与 KernelSU 相同，直接把输出以等宽字体铺满屏幕并自动滚到底部，
 * 不再套卡片 / 圆角框。模块安装与 nksu 安装共用同一套界面。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LogTopBar(
    title: String,
    onBack: (() -> Unit)?,
    onSaveLog: () -> Unit,
) {
    TopAppBar(
        title = { Text(title) },
        colors =
            TopAppBarDefaults.topAppBarColors(
                containerColor = MaterialTheme.colorScheme.surfaceContainer,
                scrolledContainerColor = MaterialTheme.colorScheme.surfaceContainer,
            ),
        navigationIcon = {
            if (onBack != null) {
                IconButton(onClick = onBack) {
                    Icon(Icons.AutoMirrored.Rounded.ArrowBack, contentDescription = stringResource(R.string.cd_back))
                }
            }
        },
        actions = {
            IconButton(onClick = onSaveLog) {
                Icon(Icons.Rounded.Save, contentDescription = stringResource(R.string.cd_save))
            }
        },
    )
}

@Composable
fun LogPane(
    text: String,
    running: Boolean,
) {
    val scrollState = rememberScrollState()

    // 日志按行不断追加：直接跳到末尾即可，不必每来一行都重启一次滚动动画。
    LaunchedEffect(text) {
        scrollState.scrollTo(scrollState.maxValue)
    }

    Box(modifier = Modifier.fillMaxSize()) {
        // 安装走一次性 root 命令，日志要等结束才回填；此时先给一个进度指示。
        if (text.isBlank() && running) {
            CircularProgressIndicator(
                modifier = Modifier.align(Alignment.Center).size(36.dp),
                strokeWidth = 2.5.dp,
            )
        }

        Column(
            modifier = Modifier.fillMaxSize().verticalScroll(scrollState),
        ) {
            SelectionContainer {
                Text(
                    text = text,
                    modifier = Modifier.fillMaxWidth().padding(8.dp),
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                )
            }
            // 给底部按钮留出空间，避免遮住最后几行日志。
            Spacer(Modifier.height(88.dp))
        }
    }
}

/** 日志结束后的「关闭」按钮，对应 KernelSU 的 extended FAB。 */
@Composable
fun LogDoneFab(
    label: String,
    onClick: () -> Unit,
) {
    ExtendedFloatingActionButton(
        onClick = onClick,
        content = { Text(label) },
    )
}
