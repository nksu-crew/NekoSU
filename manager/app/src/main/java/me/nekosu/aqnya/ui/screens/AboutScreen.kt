package me.nekosu.aqnya.ui.screens

import android.content.Intent
import android.content.pm.PackageManager
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material.icons.automirrored.rounded.KeyboardArrowRight
import androidx.compose.material.icons.rounded.Code
import androidx.compose.material.icons.rounded.Description
import androidx.compose.material.icons.rounded.Gavel
import androidx.compose.material.icons.rounded.Info
import androidx.compose.material.icons.rounded.NewReleases
import androidx.compose.material.icons.rounded.OpenInNew
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBarDefaults.pinnedScrollBehavior
import androidx.compose.material3.rememberTopAppBarState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.painter.BitmapPainter
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalInspectionMode
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.core.graphics.drawable.toBitmap
import androidx.core.net.toUri
import androidx.navigation.NavHostController
import me.nekosu.aqnya.BuildConfig
import me.nekosu.aqnya.R
import me.nekosu.aqnya.core.navigation.AppRoute
import me.nekosu.aqnya.ui.component.AppListItem
import me.nekosu.aqnya.ui.component.AppTopAppBar
import me.nekosu.aqnya.ui.component.CardGroup
import me.nekosu.aqnya.ui.component.ModeLabel

private const val REPOSITORY_URL = "https://github.com/nksu-crew/NekoSU"

@Composable
fun AboutScreen(navController: NavHostController) {
    val context = LocalContext.current
    val isInPreview = LocalInspectionMode.current
    val versionName =
        remember(context, isInPreview) {
            if (isInPreview) {
                context.getString(R.string.about_preview_version)
            } else {
                try {
                    context.packageManager.getPackageInfo(context.packageName, 0).versionName
                        ?: context.getString(R.string.about_unknown_version)
                } catch (_: PackageManager.NameNotFoundException) {
                    context.getString(R.string.about_unknown_version)
                }
            }
        }
    val channel = BuildConfig.NKSU_CHANNEL.replaceFirstChar { it.uppercase() }

    AboutScreenContent(
        versionName = versionName,
        channel = channel,
        onBack = { navController.popBackStack() },
        onRepositoryClick = {
            context.startActivity(Intent(Intent.ACTION_VIEW, REPOSITORY_URL.toUri()))
        },
        onOpenSourceClick = { navController.navigate(AppRoute.OpenSource) },
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AboutScreenContent(
    versionName: String,
    channel: String,
    onBack: () -> Unit,
    onRepositoryClick: () -> Unit,
    onOpenSourceClick: () -> Unit,
) {
    val scrollBehavior = pinnedScrollBehavior(rememberTopAppBarState())

    Scaffold(
        containerColor = MaterialTheme.colorScheme.surfaceContainer,
        topBar = {
            AppTopAppBar(
                title = stringResource(R.string.about),
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Rounded.ArrowBack, contentDescription = stringResource(R.string.cd_back))
                    }
                },
                scrollBehavior = scrollBehavior,
            )
        },
        modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        contentWindowInsets =
            WindowInsets.safeDrawing
                .only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal),
    ) { innerPadding ->
        Column(
            modifier =
                Modifier
                    .fillMaxSize()
                    .padding(innerPadding)
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = 16.dp)
                    .padding(top = 12.dp, bottom = 96.dp),
            verticalArrangement = Arrangement.spacedBy(24.dp),
        ) {
            AboutHero()

            AboutSection(title = stringResource(R.string.about_information)) {
                CardGroup {
                    item {
                        AboutInfoItem(
                            title = stringResource(R.string.manager_version),
                            summary = versionName,
                            icon = Icons.Rounded.Info,
                        )
                    }
                    item {
                        AboutInfoItem(
                            title = stringResource(R.string.about_channel),
                            icon = Icons.Rounded.NewReleases,
                            trailing = {
                                ModeLabel(
                                    label = channel.uppercase(),
                                    color = MaterialTheme.colorScheme.onSecondaryContainer,
                                    containerColor = MaterialTheme.colorScheme.secondaryContainer,
                                )
                            },
                        )
                    }
                    item {
                        AboutInfoItem(
                            title = stringResource(R.string.about_license),
                            icon = Icons.Rounded.Gavel,
                            trailing = { AboutValueText(stringResource(R.string.about_license_short)) },
                        )
                    }
                }
            }

            AboutSection(title = stringResource(R.string.about_project)) {
                CardGroup {
                    item {
                        AboutLinkItem(
                            title = stringResource(R.string.about_repository),
                            summary = stringResource(R.string.about_repository_summary),
                            icon = Icons.Rounded.Code,
                            trailingIcon = Icons.Rounded.OpenInNew,
                            onClick = onRepositoryClick,
                        )
                    }
                    item {
                        AboutLinkItem(
                            title = stringResource(R.string.about_open_source),
                            summary = stringResource(R.string.about_open_source_summary),
                            icon = Icons.Rounded.Description,
                            trailingIcon = Icons.AutoMirrored.Rounded.KeyboardArrowRight,
                            onClick = onOpenSourceClick,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun AboutHero(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val density = LocalDensity.current
    val icon =
        remember(context, density) {
            val drawable = ContextCompat.getDrawable(context, R.mipmap.ic_launcher)!!
            val sizePx = with(density) { 96.dp.roundToPx() }
            BitmapPainter(drawable.toBitmap(sizePx, sizePx).asImageBitmap())
        }
    Column(
        modifier = modifier.fillMaxWidth().padding(vertical = 16.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Image(
            painter = icon,
            contentDescription = null,
            contentScale = ContentScale.Fit,
            modifier = Modifier.size(96.dp),
        )

        Spacer(Modifier.height(16.dp))

        Text(
            text = stringResource(R.string.app_name),
            style = MaterialTheme.typography.headlineMedium,
            color = MaterialTheme.colorScheme.onSurface,
            textAlign = TextAlign.Center,
        )

        Spacer(Modifier.height(4.dp))

        Text(
            text = stringResource(R.string.about_tagline),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
        )
    }
}

@Composable
private fun AboutSection(
    title: String,
    modifier: Modifier = Modifier,
    content: @Composable () -> Unit,
) {
    Column(
        modifier = modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(
            text = title,
            style = MaterialTheme.typography.titleSmall,
            color = MaterialTheme.colorScheme.primary,
            modifier = Modifier.padding(start = 16.dp),
        )
        content()
    }
}

@Composable
private fun AboutInfoItem(
    title: String,
    icon: ImageVector,
    modifier: Modifier = Modifier,
    summary: String? = null,
    trailing: (@Composable () -> Unit)? = null,
) {
    AppListItem(
        title = title,
        modifier = modifier.fillMaxWidth(),
        summary = summary,
        leading = {
            Icon(
                imageVector = icon,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        },
        trailing = trailing,
    )
}

@Composable
private fun AboutValueText(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.labelLarge,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
    )
}

@Composable
private fun AboutLinkItem(
    title: String,
    summary: String,
    icon: ImageVector,
    trailingIcon: ImageVector,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    AppListItem(
        title = title,
        modifier = modifier.fillMaxWidth(),
        summary = summary,
        leading = {
            Icon(
                imageVector = icon,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary,
            )
        },
        trailing = {
            Icon(
                imageVector = trailingIcon,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        },
        onClick = onClick,
    )
}

@Preview(showBackground = true)
@Composable
fun AboutScreenPreview() {
    MaterialTheme {
        AboutScreenContent(
            versionName = "1.0.0-canary.123+abcdef123456",
            channel = "Canary",
            onBack = {},
            onRepositoryClick = {},
            onOpenSourceClick = {},
        )
    }
}
