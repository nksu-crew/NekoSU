// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.navigation

import androidx.annotation.StringRes
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.List
import androidx.compose.material.icons.automirrored.rounded.List
import androidx.compose.material.icons.outlined.Extension
import androidx.compose.material.icons.outlined.Home
import androidx.compose.material.icons.outlined.Security
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material.icons.rounded.Extension
import androidx.compose.material.icons.rounded.Home
import androidx.compose.material.icons.rounded.Security
import androidx.compose.material.icons.rounded.Settings
import androidx.compose.ui.graphics.vector.ImageVector
import me.nekosu.aqnya.R
import me.nekosu.aqnya.core.navigation.AppRoute

sealed class BottomNavItem(
    val route: String,
    @StringRes val titleRes: Int,
    val selectedIcon: ImageVector,
    val unselectedIcon: ImageVector,
) {
    object Home : BottomNavItem(
        route = AppRoute.Home,
        titleRes = R.string.nav_home,
        selectedIcon = Icons.Rounded.Home,
        unselectedIcon = Icons.Outlined.Home,
    )

    object History : BottomNavItem(
        route = AppRoute.Apps,
        titleRes = R.string.nav_apps,
        selectedIcon = Icons.AutoMirrored.Rounded.List,
        unselectedIcon = Icons.AutoMirrored.Outlined.List,
    )

    object Modules : BottomNavItem(
        route = AppRoute.Modules,
        titleRes = R.string.nav_modules,
        selectedIcon = Icons.Rounded.Extension,
        unselectedIcon = Icons.Outlined.Extension,
    )

    object FmacRules : BottomNavItem(
        route = AppRoute.Rules,
        titleRes = R.string.nav_rules,
        selectedIcon = Icons.Rounded.Security,
        unselectedIcon = Icons.Outlined.Security,
    )

    object Settings : BottomNavItem(
        route = AppRoute.Settings,
        titleRes = R.string.nav_settings,
        selectedIcon = Icons.Rounded.Settings,
        unselectedIcon = Icons.Outlined.Settings,
    )

    companion object {
        fun items(showRules: Boolean): List<BottomNavItem> =
            buildList {
                add(Home)
                add(History)
                add(Modules)
                if (showRules) add(FmacRules)
                add(Settings)
            }
    }
}
