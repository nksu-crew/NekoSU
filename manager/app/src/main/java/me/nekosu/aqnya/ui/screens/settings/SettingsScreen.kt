// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.screens.settings

import android.app.Activity
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.rememberTopAppBarState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.navigation.NavController
import dev.chrisbanes.haze.hazeSource
import dev.chrisbanes.haze.rememberHazeState
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch
import me.nekosu.aqnya.R
import me.nekosu.aqnya.core.navigation.AppRoute
import me.nekosu.aqnya.data.prefs.AppPreferences
import me.nekosu.aqnya.ui.component.HazeTopAppBar
import me.nekosu.aqnya.ui.navigation.LocalBottomBarClearance
import me.nekosu.aqnya.ui.screens.settings.enums.NavBarStyle
import me.nekosu.aqnya.ui.screens.settings.enums.ThemeColor
import me.nekosu.aqnya.ui.screens.settings.enums.ThemeMode
import me.nekosu.aqnya.util.LocaleHelper
import me.nekosu.aqnya.util.LogUtils

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(navController: NavController) {
    val mContext = LocalContext.current
    val scope = rememberCoroutineScope()
    val scrollBehavior = TopAppBarDefaults.pinnedScrollBehavior(rememberTopAppBarState())

    val themeValue by AppPreferences.themeModeFlow(mContext).collectAsState(initial = 0)
    val currentThemeMode = ThemeMode.fromValue(themeValue)

    val themeColorValue by AppPreferences.themeColorFlow(mContext).collectAsState(initial = 0)
    val currentThemeColor = ThemeColor.fromValue(themeColorValue)

    val amoledEnabled by AppPreferences.amoledFlow(mContext).collectAsState(initial = false)
    var blurEnabled by remember(mContext) {
        mutableStateOf(AppPreferences.isBlurEnabled(mContext))
    }
    LaunchedEffect(mContext) {
        AppPreferences.blurFlow(mContext).collect { blurEnabled = it }
    }

    val navBarStyleValue by AppPreferences.navBarStyleFlow(mContext).collectAsState(
        initial = AppPreferences.navBarStyle(mContext),
    )
    val currentNavBarStyle = NavBarStyle.fromValue(navBarStyleValue)

    val verifyKernelSymbols by AppPreferences.verifyKernelSymbolsFlow(mContext).collectAsState(
        initial = AppPreferences.verifyKernelSymbols(mContext),
    )

    val currentLang =
        LocaleHelper
            .savedLanguageTag(mContext)
            .ifBlank { "" }
    val currentLangLabel =
        LocaleHelper.availableLanguages
            .find { it.tag == currentLang }
            ?.let { stringResource(it.labelRes) }
            ?: stringResource(R.string.language_system)

    val hazeState = rememberHazeState()
    Scaffold(
        containerColor = MaterialTheme.colorScheme.surfaceContainer,
        modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        topBar = {
            HazeTopAppBar(
                state = hazeState,
                title = stringResource(R.string.settings_title),
                scrollBehavior = scrollBehavior,
                blurEnabled = blurEnabled,
            )
        },
        contentWindowInsets =
            WindowInsets.safeDrawing.only(
                WindowInsetsSides.Top + WindowInsetsSides.Horizontal,
            ),
    ) { innerPadding ->
        Column(
            modifier =
                Modifier
                    .fillMaxSize()
                    .hazeSource(hazeState)
                    .verticalScroll(rememberScrollState())
                    .padding(innerPadding)
                    .padding(top = 16.dp)
                    .padding(bottom = LocalBottomBarClearance.current)
                    .padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            LanguageSection(
                currentLangLabel = currentLangLabel,
                onLanguageChange = { tag ->
                    LocaleHelper.saveLanguageTag(mContext, tag)
                    (mContext as? Activity)?.let { LocaleHelper.applyLanguage(it) }
                },
            )

            AppearanceSection(
                currentThemeMode = currentThemeMode,
                currentThemeColor = currentThemeColor,
                amoledEnabled = amoledEnabled,
                blurEnabled = blurEnabled,
                currentNavBarStyle = currentNavBarStyle,
                onThemeChange = { mode ->
                    scope.launch { AppPreferences.setThemeMode(mContext, mode.value) }
                },
                onThemeColorChange = { color ->
                    scope.launch { AppPreferences.setThemeColor(mContext, color.value) }
                },
                onAmoledChange = { enabled ->
                    scope.launch { AppPreferences.setAmoled(mContext, enabled) }
                },
                onBlurChange = { enabled ->
                    blurEnabled = enabled
                    AppPreferences.setBlur(mContext, enabled)
                },
                onNavBarStyleChange = { style ->
                    scope.launch { AppPreferences.setNavBarStyle(mContext, style.value) }
                },
            )

            FeatureSection()

            ToolsSection(
                onExportLog = { LogUtils.exportLogs(mContext) },
                verifyKernelSymbols = verifyKernelSymbols,
                onVerifyKernelSymbolsChange = { enabled ->
                    AppPreferences.setVerifyKernelSymbols(mContext, enabled)
                },
            )

            AboutSection(
                onAboutClick = { navController.navigate(AppRoute.About) },
            )
        }
    }
}
