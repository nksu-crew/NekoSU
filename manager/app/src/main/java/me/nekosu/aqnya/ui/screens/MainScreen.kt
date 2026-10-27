// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.screens

import android.app.Application
import androidx.compose.animation.AnimatedContentTransitionScope
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.EnterTransition
import androidx.compose.animation.ExitTransition
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.tooling.preview.Preview
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavBackStackEntry
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import dev.chrisbanes.haze.hazeSource
import dev.chrisbanes.haze.rememberHazeState
import kotlinx.coroutines.delay
import me.nekosu.aqnya.core.navigation.AppRoute
import me.nekosu.aqnya.data.prefs.AppPreferences
import me.nekosu.aqnya.feature.home.presentation.HomeViewModel
import me.nekosu.aqnya.feature.home.presentation.HomeViewModelFactory
import me.nekosu.aqnya.feature.home.ui.HomeScreen
import me.nekosu.aqnya.ui.animation.PageTransitions
import me.nekosu.aqnya.ui.component.NonProductionRibbon
import me.nekosu.aqnya.ui.navigation.BottomNavItem
import me.nekosu.aqnya.ui.navigation.GlassBottomBarContentPadding
import me.nekosu.aqnya.ui.navigation.GlassBottomBarHorizontalPadding
import me.nekosu.aqnya.ui.navigation.GlassBottomBarVerticalPadding
import me.nekosu.aqnya.ui.navigation.GlassBottomNavigationBar
import me.nekosu.aqnya.ui.navigation.LocalBottomBarClearance
import me.nekosu.aqnya.ui.navigation.NormalBottomNavigationBar
import me.nekosu.aqnya.ui.navigation.StandardBottomBarContentPadding
import me.nekosu.aqnya.ui.screens.settings.enums.NavBarStyle
import me.nekosu.aqnya.ui.permission.AppPermission
import me.nekosu.aqnya.ui.permission.MiuiPermissionUtils
import me.nekosu.aqnya.ui.permission.rememberPermissionState
import me.nekosu.aqnya.ui.screens.install.InstallScreen
import me.nekosu.aqnya.ui.screens.modules.ModuleScreen
import me.nekosu.aqnya.ui.screens.settings.SettingsScreen

// Flutter's tab switcher cross-fades the page content over 70 ms. Routes that
// leave the tab set use the same Cupertino-style slide as pushed pages.
private const val TabFadeMillis = 70

private fun AnimatedContentTransitionScope<NavBackStackEntry>.isTabSwitch(
    navItems: List<BottomNavItem>,
): Boolean {
    val routes = navItems.map { it.route }
    return initialState.destination.route in routes && targetState.destination.route in routes
}

private fun tabTransitionEnter(
    navItems: List<BottomNavItem>,
): AnimatedContentTransitionScope<NavBackStackEntry>.() -> EnterTransition =
    {
        if (isTabSwitch(navItems)) {
            fadeIn(animationSpec = tween(TabFadeMillis, easing = LinearEasing))
        } else {
            PageTransitions.enter()
        }
    }

private fun tabTransitionExit(
    navItems: List<BottomNavItem>,
): AnimatedContentTransitionScope<NavBackStackEntry>.() -> ExitTransition =
    {
        if (isTabSwitch(navItems)) {
            fadeOut(animationSpec = tween(TabFadeMillis, easing = LinearEasing))
        } else {
            PageTransitions.exit()
        }
    }

private fun tabTransitionPopEnter(
    navItems: List<BottomNavItem>,
): AnimatedContentTransitionScope<NavBackStackEntry>.() -> EnterTransition =
    {
        if (isTabSwitch(navItems)) {
            fadeIn(animationSpec = tween(TabFadeMillis, easing = LinearEasing))
        } else {
            PageTransitions.popEnter()
        }
    }

private fun tabTransitionPopExit(
    navItems: List<BottomNavItem>,
): AnimatedContentTransitionScope<NavBackStackEntry>.() -> ExitTransition =
    {
        if (isTabSwitch(navItems)) {
            fadeOut(animationSpec = tween(TabFadeMillis, easing = LinearEasing))
        } else {
            PageTransitions.popExit()
        }
    }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainScreen() {
    val navController = rememberNavController()
    val context = LocalContext.current

    val navBarStyleValue by AppPreferences.navBarStyleFlow(context).collectAsState(
        initial = AppPreferences.navBarStyle(context),
    )
    val navBarStyle = NavBarStyle.fromValue(navBarStyleValue)
    val glassStyle = navBarStyle == NavBarStyle.GLASS
    val bottomBarClearance =
        if (navBarStyle == NavBarStyle.STANDARD) {
            StandardBottomBarContentPadding
        } else {
            GlassBottomBarContentPadding
        }

    val homeViewModel: HomeViewModel =
        viewModel(
            factory = HomeViewModelFactory(context.applicationContext as Application),
        )
    val showRules by AppPreferences.showRulesFlow(context).collectAsState(initial = false)
    val navItems = remember(showRules) { BottomNavItem.items(showRules) }

    val miuiAppsPermState = rememberPermissionState(AppPermission.MIUI_GET_INSTALLED_APPS)

    val navBackStackEntry by navController.currentBackStackEntryAsState()
    val currentRoute = navBackStackEntry?.destination?.route
    val bottomBarRoutes = remember(navItems) { navItems.map { it.route }.toSet() }
    val showBottomBar = currentRoute in bottomBarRoutes
    val selectedIndex = navItems.indexOfFirst { it.route == currentRoute }.coerceAtLeast(0)

    LaunchedEffect(currentRoute) {
        if (currentRoute == BottomNavItem.Home.route) {
            delay(300)
            homeViewModel.refresh()
        }
    }

    LaunchedEffect(Unit) {
        if (MiuiPermissionUtils.isSupportedOnThisDevice(context) &&
            !MiuiPermissionUtils.isGranted(context)
        ) {
            miuiAppsPermState.launchRequest()
        }
    }

    val onTabClick: (Int) -> Unit = { index ->
        val target = navItems.getOrNull(index)?.route
        if (target != null && target != currentRoute) {
            navController.navigate(target) {
                // 回到 Tab 根，保存/恢复各 Tab 的滚动状态等
                popUpTo(BottomNavItem.Home.route) {
                    saveState = true
                }
                launchSingleTop = true
                restoreState = true
            }
        }
    }

    val hazeState = rememberHazeState()

    CompositionLocalProvider(LocalBottomBarClearance provides bottomBarClearance) {
        Box(Modifier.fillMaxSize()) {
            Scaffold(
                containerColor = MaterialTheme.colorScheme.surfaceContainer,
                contentWindowInsets =
                    WindowInsets.safeDrawing.only(
                        WindowInsetsSides.Bottom + WindowInsetsSides.Horizontal,
                    ),
            ) { innerPadding ->
                Box(
                    modifier =
                        Modifier
                            .fillMaxSize()
                            .padding(innerPadding)
                            .then(if (glassStyle) Modifier.hazeSource(hazeState) else Modifier),
                ) {
                    NavHost(
                        navController = navController,
                        startDestination = BottomNavItem.Home.route,
                        modifier = Modifier.fillMaxSize(),
                        enterTransition = { PageTransitions.enter() },
                        exitTransition = { PageTransitions.exit() },
                        popEnterTransition = { PageTransitions.popEnter() },
                        popExitTransition = { PageTransitions.popExit() },
                    ) {
                        composable(
                            route = BottomNavItem.Home.route,
                            enterTransition = tabTransitionEnter(navItems),
                            exitTransition = tabTransitionExit(navItems),
                            popEnterTransition = tabTransitionPopEnter(navItems),
                            popExitTransition = tabTransitionPopExit(navItems),
                        ) {
                            HomeScreen(
                                viewModel = homeViewModel,
                                onInstallClick = { navController.navigate(AppRoute.Install) },
                            )
                        }

                        composable(
                            route = BottomNavItem.History.route,
                            enterTransition = tabTransitionEnter(navItems),
                            exitTransition = tabTransitionExit(navItems),
                            popEnterTransition = tabTransitionPopEnter(navItems),
                            popExitTransition = tabTransitionPopExit(navItems),
                        ) {
                            AppListScreen(
                                navController = navController,
                                extraBottomPadding = bottomBarClearance,
                            )
                        }

                        composable(
                            route = BottomNavItem.Modules.route,
                            enterTransition = tabTransitionEnter(navItems),
                            exitTransition = tabTransitionExit(navItems),
                            popEnterTransition = tabTransitionPopEnter(navItems),
                            popExitTransition = tabTransitionPopExit(navItems),
                        ) {
                            ModuleScreen()
                        }

                        composable(
                            route = BottomNavItem.Settings.route,
                            enterTransition = tabTransitionEnter(navItems),
                            exitTransition = tabTransitionExit(navItems),
                            popEnterTransition = tabTransitionPopEnter(navItems),
                            popExitTransition = tabTransitionPopExit(navItems),
                        ) {
                            SettingsScreen(navController)
                        }

                        composable(
                            route = AppRoute.About,
                        ) {
                            AboutScreen(navController)
                        }

                        composable(
                            route = AppRoute.OpenSource,
                        ) {
                            OpenSourceScreen(navController)
                        }

                        composable(
                            route = AppRoute.Install,
                        ) {
                            InstallScreen(navController)
                        }

                        composable(AppRoute.AppDetailPattern) { backStackEntry ->
                            val pkg = backStackEntry.arguments?.getString("packageName")
                            if (pkg == null) {
                                LaunchedEffect(Unit) { navController.popBackStack() }
                                return@composable
                            }
                            val appViewModel: AppViewModel = viewModel(factory = AppViewModelFactory(context.applicationContext))
                            val app = appViewModel.allApps.find { it.packageName == pkg }

                            if (app != null) {
                                AppDetailScreen(
                                    app = app,
                                    config = appViewModel.appConfigs[pkg],
                                    onSave = { appViewModel.setAppConfig(app, it) },
                                    onBack = { navController.popBackStack() },
                                )
                            } else {
                                LaunchedEffect(Unit) {
                                    appViewModel.loadApps()
                                    if (appViewModel.allApps.none { it.packageName == pkg }) {
                                        navController.popBackStack()
                                    }
                                }
                                Box(
                                    modifier = Modifier.fillMaxSize(),
                                    contentAlignment = Alignment.Center,
                                ) {
                                    LoadingState()
                                }
                            }
                        }
                    }

                    NonProductionRibbon(modifier = Modifier.align(Alignment.TopEnd))
                }
            }

            AnimatedVisibility(
                visible = showBottomBar,
                enter = fadeIn(animationSpec = tween(150)),
                exit = fadeOut(animationSpec = tween(100)),
                modifier = Modifier.align(Alignment.BottomCenter),
            ) {
                when (navBarStyle) {
                    NavBarStyle.STANDARD -> {
                        NormalBottomNavigationBar(
                            items = navItems,
                            selectedIndex = selectedIndex,
                            onTabClick = onTabClick,
                        )
                    }

                    NavBarStyle.GLASS -> {
                        GlassBottomNavigationBar(
                            hazeState = hazeState,
                            items = navItems,
                            selectedIndex = selectedIndex,
                            onTabClick = onTabClick,
                            modifier =
                                Modifier
                                    .navigationBarsPadding()
                                    .windowInsetsPadding(
                                        WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal),
                                    )
                                    .padding(
                                        horizontal = GlassBottomBarHorizontalPadding,
                                        vertical = GlassBottomBarVerticalPadding,
                                    ),
                        )
                    }
                }
            }
        }
    }
}

@Preview(showBackground = true)
@Composable
fun MainScreenPreview() {
    MainScreen()
}
