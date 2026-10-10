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
import me.nekosu.aqnya.util.getAppCommit
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
                _managerVersion.value = getAppVersion(appContext)
                _isGki.value = KernelInfo.isGki()

                if (!installed) {
                    _installStatus.value = InstallStatus.NOT_INSTALLED
                } else {
                    // LKM 与 APK 版本绑定：运行中的内核模块必须来自本 APK 的同一
                    // commit，否则管理器内打包的 ncore 会与已刷入的 LKM 不匹配
                    // （表现为模块卡在 modules_update、/data/adb/ksu 软链接缺失，
                    // 甚至启动失败）。旧内核没有 IOC_GET_VERSION，此时视为匹配。
                    ncore.ctl(3)
                    val moduleVersion = runCatching { ncore.moduleVersion() }.getOrNull()
                    val appCommit = runCatching { getAppCommit(appContext) }.getOrNull()
                    val matched =
                        moduleVersion.isNullOrBlank() ||
                            moduleVersion == "unknown" ||
                            appCommit.isNullOrBlank() ||
                            moduleVersion == appCommit

                    _installStatus.value =
                        if (matched) InstallStatus.INSTALLED else InstallStatus.NEED_UPDATE

                    // 只有版本一致时才把随包的 ncore 放到 /data/adb/nksu/ncore，
                    // 避免用一个不匹配的 ncore 覆盖能工作的旧版本。
                    if (matched) {
                        NcoreBoot.install(appContext)
                    }
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
