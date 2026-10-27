package me.nekosu.aqnya.feature.home.presentation

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import me.nekosu.aqnya.core.model.InstallStatus
import me.nekosu.aqnya.feature.home.data.HomeRepository
import me.nekosu.aqnya.feature.home.domain.HomeSnapshot

class HomeViewModel(
    app: Application,
    private val repository: HomeRepository = HomeRepository(app.applicationContext),
) : AndroidViewModel(app) {
    private val _uiState = MutableStateFlow(HomeUiState())
    val uiState: StateFlow<HomeUiState> = _uiState.asStateFlow()

    init {
        viewModelScope.launch {
            apply(repository.load())
        }
    }

    fun refresh() {
        if (_uiState.value.installStatus != InstallStatus.INSTALLED) return
        viewModelScope.launch {
            repository.refresh()
        }
    }

    private fun apply(snapshot: HomeSnapshot) {
        _uiState.value =
            HomeUiState(
                installStatus =
                    when {
                        !snapshot.installed -> InstallStatus.NOT_INSTALLED
                        !snapshot.versionMatched -> InstallStatus.NEED_UPDATE
                        else -> InstallStatus.INSTALLED
                    },
                managerVersion = snapshot.managerVersion,
                isGki = snapshot.isGki,
            )
    }
}

class HomeViewModelFactory(
    private val app: Application,
) : ViewModelProvider.Factory {
    @Suppress("UNCHECKED_CAST")
    override fun <T : ViewModel> create(modelClass: Class<T>): T = HomeViewModel(app, HomeRepository(app.applicationContext)) as T
}
