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
import me.nekosu.aqnya.BuildConfig
import me.nekosu.aqnya.core.Ncore
import me.nekosu.aqnya.data.installer.KsudBoot
import me.nekosu.aqnya.data.kernel.KernelInfo
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
                val installed = Ncore.ctl(1) == 0
                _managerVersion.value = getAppVersion(appContext)
                _isGki.value = KernelInfo.isGki()

                if (!installed) {
                    _installStatus.value = InstallStatus.NOT_INSTALLED
                } else {
                    // LKM 版本比对：运行中的内核模块版本由仓库根的 version
                    // 手动维护，并与本 APK 打包时的同一文件值绑定。不一致时说明
                    // 已刷入的 LKM 与随包 ksud 不匹配（表现为模块卡在
                    // modules_update、/data/adb/ksu 软链接缺失，甚至启动失败）。
                    // 旧内核没有 IOC_GET_VERSION、或版本为 unknown 时视为匹配。
                    Ncore.ctl(3)
                    val moduleVersion = runCatching { Ncore.moduleVersion() }.getOrNull()
                    val expectedVersion = BuildConfig.NKSU_VERSION
                    val matched =
                        moduleVersion.isNullOrBlank() ||
                            moduleVersion == "unknown" ||
                            expectedVersion == "unknown" ||
                            moduleVersion == expectedVersion

                    _installStatus.value =
                        if (matched) InstallStatus.INSTALLED else InstallStatus.NEED_UPDATE

                    // 只有版本一致时才把随包的 ksud 放到 /data/adb/nksu/ksud，
                    // 避免用一个不匹配的 ksud 覆盖能工作的旧版本。
                    if (matched) {
                        KsudBoot.install(appContext)
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
                Ncore.ctl(3)
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
