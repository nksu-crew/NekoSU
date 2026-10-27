// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.feature.home.presentation

import me.nekosu.aqnya.core.model.InstallStatus

data class HomeUiState(
    val installStatus: InstallStatus = InstallStatus.NOT_INSTALLED,
    val managerVersion: String = "",
    val isGki: Boolean = false,
)
