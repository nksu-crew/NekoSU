// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.component

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.TopAppBarScrollBehavior
import androidx.compose.material3.rememberTopAppBarState
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import me.nekosu.aqnya.R

/** [Page] 的默认尺寸，页面只在需要时覆盖。 */
object PageDefaults {
    /** 内容区四周边距；与顶栏/底栏的实际高度无关。 */
    val ContentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 24.dp)

    /** 内容块之间的默认间距。 */
    val ContentSpacing = 16.dp

    /** 同一分组内标题与卡片的间距。 */
    val SectionSpacing = 8.dp
}

/** 页面内容安全区；底部交给外层 Scaffold / 底栏处理。 */
@Composable
private fun pageContentWindowInsets(): WindowInsets =
    WindowInsets.safeDrawing.only(
        WindowInsetsSides.Top + WindowInsetsSides.Horizontal,
    )

/**
 * 通用页面骨架：顶栏、返回键、可选 FAB / 底栏、安全区与统一内容间距。
 *
 * 默认把 [content] 放进可滚动 Column；整屏内容（例如安装日志）把 [scrollable]
 * 设为 false，此时内容区不滚动、也不加 [contentPadding]，由页面自行铺满。
 *
 * @param title 默认顶栏标题；传了 [topBar] 时忽略。
 * @param onBack 非空时默认顶栏显示返回键。
 * @param topBar 完整替换默认顶栏；用于日志页等特殊状态。
 * @param bottomBar 固定在底部的内容（通常是主操作按钮）。
 * @param snackbarHost 可选 SnackbarHost。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun Page(
    title: String,
    modifier: Modifier = Modifier,
    subtitle: String = "",
    onBack: (() -> Unit)? = null,
    actions: @Composable RowScope.() -> Unit = {},
    topBar: (@Composable (TopAppBarScrollBehavior) -> Unit)? = null,
    floatingActionButton: @Composable () -> Unit = {},
    bottomBar: @Composable () -> Unit = {},
    snackbarHost: @Composable () -> Unit = {},
    scrollable: Boolean = true,
    contentPadding: PaddingValues = PageDefaults.ContentPadding,
    verticalArrangement: Arrangement.Vertical = Arrangement.spacedBy(PageDefaults.ContentSpacing),
    contentWindowInsets: WindowInsets = pageContentWindowInsets(),
    content: @Composable ColumnScope.() -> Unit,
) {
    val scrollBehavior = TopAppBarDefaults.pinnedScrollBehavior(rememberTopAppBarState())

    Scaffold(
        containerColor = MaterialTheme.colorScheme.surfaceContainer,
        modifier = modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        topBar = {
            if (topBar != null) {
                topBar(scrollBehavior)
            } else {
                PageTopBar(
                    title = title,
                    subtitle = subtitle,
                    onBack = onBack,
                    actions = actions,
                    scrollBehavior = scrollBehavior,
                )
            }
        },
        floatingActionButton = floatingActionButton,
        bottomBar = bottomBar,
        snackbarHost = snackbarHost,
        contentWindowInsets = contentWindowInsets,
    ) { innerPadding ->
        Column(
            modifier =
                if (scrollable) {
                    Modifier
                        .fillMaxSize()
                        .padding(innerPadding)
                        .verticalScroll(rememberScrollState())
                        .padding(contentPadding)
                } else {
                    Modifier
                        .fillMaxSize()
                        .padding(innerPadding)
                },
            verticalArrangement = verticalArrangement,
            content = content,
        )
    }
}

/** 默认顶栏：标题、可选副标题与返回键。 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PageTopBar(
    title: String,
    subtitle: String = "",
    onBack: (() -> Unit)? = null,
    actions: @Composable RowScope.() -> Unit = {},
    scrollBehavior: TopAppBarScrollBehavior? = null,
) {
    AppTopAppBar(
        title = title,
        subtitle = subtitle,
        navigationIcon = {
            if (onBack != null) {
                IconButton(onClick = onBack) {
                    Icon(
                        imageVector = Icons.AutoMirrored.Rounded.ArrowBack,
                        contentDescription = stringResource(R.string.cd_back),
                    )
                }
            }
        },
        actions = actions,
        scrollBehavior = scrollBehavior,
    )
}

/**
 * 带标题的页面分组，标题样式与设置页/首页的小节标签一致。
 *
 * @param trailing 标题行右侧的附加内容（例如「推荐」标签）。
 */
@Composable
fun PageSection(
    title: String,
    modifier: Modifier = Modifier,
    trailing: @Composable (() -> Unit)? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    Column(
        modifier = modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(PageDefaults.SectionSpacing),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleSmall,
                color = MaterialTheme.colorScheme.primary,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.weight(1f),
            )
            trailing?.invoke()
        }
        content()
    }
}

@Preview(showBackground = true)
@Composable
private fun PagePreview() {
    MaterialTheme {
        Page(
            title = "Page",
            subtitle = "Subtitle",
            onBack = {},
        ) {
            PageSection(title = "Section") {
                CardGroup {
                    item {
                        ListRow(
                            icon = {},
                            headline = { Text("A row") },
                            supporting = { Text("Supporting text") },
                            onClick = {},
                        )
                    }
                }
            }
        }
    }
}
