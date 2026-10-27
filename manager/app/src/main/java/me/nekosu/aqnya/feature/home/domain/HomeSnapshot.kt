package me.nekosu.aqnya.feature.home.domain

data class HomeSnapshot(
    val installed: Boolean,
    val versionMatched: Boolean,
    val managerVersion: String,
    val isGki: Boolean,
)
