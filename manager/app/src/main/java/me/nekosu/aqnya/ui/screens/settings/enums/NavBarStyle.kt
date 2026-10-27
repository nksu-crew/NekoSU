// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.screens.settings.enums

import androidx.annotation.StringRes
import me.nekosu.aqnya.R

enum class NavBarStyle(
    @param:StringRes val titleRes: Int,
    val value: Int,
) {
    STANDARD(R.string.nav_bar_style_standard, 0),
    GLASS(R.string.nav_bar_style_glass, 1),
    ;

    companion object {
        fun fromValue(value: Int) = entries.find { it.value == value } ?: STANDARD
    }
}
