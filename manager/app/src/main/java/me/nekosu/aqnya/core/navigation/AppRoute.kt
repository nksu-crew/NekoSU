// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.core.navigation

import me.nekosu.aqnya.core.model.InstallStrategy
import me.nekosu.aqnya.core.model.InstallTarget

object AppRoute {
    const val Home = "home"
    const val Apps = "app"
    const val Modules = "modules"
    const val Rules = "rules"
    const val Settings = "settings"
    const val SettingsAppearance = "settings/appearance"
    const val SettingsFeatures = "settings/features"
    const val SettingsTools = "settings/tools"
    const val SettingsLanguage = "settings/language"
    const val About = "about"
    const val OpenSource = "open_source"
    const val InstallPattern = "install/{target}/{strategy}"
    const val ChooseInstall = "ChooseInstall"
    const val AppDetailPattern = "app_detail/{packageName}"

    fun appDetail(packageName: String): String = "app_detail/$packageName"

    fun install(
        target: InstallTarget,
        strategy: InstallStrategy,
    ): String = "install/${target.name}/${strategy.name}"
}
