package me.nekosu.aqnya.ui.screens

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.nekosu.aqnya.ncore
import me.nekosu.aqnya.util.KernelInfo
import me.nekosu.aqnya.util.NcoreBoot
import me.nekosu.aqnya.util.getAppVersion

class HomeViewModel(
    app: Application,
) : AndroidViewModel(app) {
    private val _installStatus = MutableStateFlow(InstallStatus.NOT_INSTALLED)
    val installStatus: StateFlow<InstallStatus> = _installStatus

    private val _managerVersion = MutableStateFlow("")
    val managerVersion: StateFlow<String> = _managerVersion

    private val _isGki = MutableStateFlow(false)
    val isGki: StateFlow<Boolean> = _isGki

    private val appContext = app.applicationContext

    init {
        viewModelScope.launch {
            withContext(Dispatchers.IO) {
                val installed = ncore.ctl(1) == 0
                _installStatus.value = if (installed) InstallStatus.INSTALLED else InstallStatus.NOT_INSTALLED
                _managerVersion.value = getAppVersion(appContext)
                _isGki.value = KernelInfo.isGki()

                // nksu 已生效：确保启动用的 ncore 位于 /data/adb/nksu/ncore。
                if (installed) {
                    NcoreBoot.install(appContext)
                }
            }
            refresh()
        }
    }

    fun refresh() {
        if (_installStatus.value != InstallStatus.INSTALLED) return
        viewModelScope.launch {
            withContext(Dispatchers.IO) {
                ncore.ctl(3)
            }
        }
    }
}

class HomeViewModelFactory(
    private val app: Application,
) : ViewModelProvider.Factory {
    @Suppress("UNCHECKED_CAST")
    override fun <T : ViewModel> create(modelClass: Class<T>): T = HomeViewModel(app) as T
}
