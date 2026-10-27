package me.nekosu.aqnya.core.navigation

object AppRoute {
    const val Home = "home"
    const val Apps = "app"
    const val Modules = "modules"
    const val Rules = "rules"
    const val Settings = "settings"
    const val About = "about"
    const val OpenSource = "open_source"
    const val Install = "install"
    const val AppDetailPattern = "app_detail/{packageName}"

    fun appDetail(packageName: String): String = "app_detail/$packageName"
}
