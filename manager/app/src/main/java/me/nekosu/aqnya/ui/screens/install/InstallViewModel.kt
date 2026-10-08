package me.nekosu.aqnya.ui.screens.install

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.nekosu.aqnya.util.KernelInfo
import me.nekosu.aqnya.util.VendorBootInstaller

/**
 * 安装页面的可观察状态。
 */
data class InstallUiState(
    val kernelVersion: String? = null,
    val isGki: Boolean = false,
    val currentKmi: String? = null,
    val candidates: List<String> = emptyList(),
    val selectedKo: String? = null,
    val recommendedKo: String? = null,
    val vendorBootUri: Uri? = null,
    val vendorBootName: String? = null,
    val running: Boolean = false,
    val done: Boolean = false,
    val success: Boolean? = null,
    val message: String = "",
) {
    val canInstall: Boolean get() = vendorBootUri != null && selectedKo != null && !running
}

/**
 * vendor_boot 安装流程的状态持有者。
 *
 * 通过 [KernelInfo] 展示 JNI 查询到的内核版本 / KMI，并据此预选匹配的 nksu.ko。
 */
class InstallViewModel(app: Application) : AndroidViewModel(app) {
    private val _uiState = MutableStateFlow(InstallUiState())
    val uiState: StateFlow<InstallUiState> = _uiState.asStateFlow()

    private val appContext = app.applicationContext

    /** 载入内核信息与可用的内核模块，只执行一次。 */
    fun load() {
        if (_uiState.value.candidates.isNotEmpty()) return
        viewModelScope.launch {
            val loaded =
                withContext(Dispatchers.IO) {
                    val currentKmi = KernelInfo.currentKmi()
                    val candidates =
                        runCatching { VendorBootInstaller.koCandidates(appContext) }
                            .getOrDefault(emptyList())
                    LoadedKernelInfo(
                        kernelVersion = KernelInfo.version(),
                        isGki = KernelInfo.isGki(),
                        currentKmi = currentKmi,
                        candidates = candidates,
                        recommendedKo = KernelInfo.matchKo(candidates, currentKmi),
                    )
                }
            _uiState.update { current ->
                current.copy(
                    kernelVersion = loaded.kernelVersion,
                    isGki = loaded.isGki,
                    currentKmi = loaded.currentKmi,
                    candidates = loaded.candidates,
                    recommendedKo = loaded.recommendedKo,
                    selectedKo = current.selectedKo ?: loaded.recommendedKo,
                )
            }
        }
    }

    fun selectKo(name: String) {
        if (_uiState.value.running) return
        _uiState.update { it.copy(selectedKo = name) }
    }

    fun selectVendorBoot(uri: Uri) {
        if (_uiState.value.running) return
        val name =
            uri.lastPathSegment
                ?.substringAfterLast('/')
                ?.takeIf { it.isNotBlank() }
                ?: uri.toString()
        _uiState.update {
            it.copy(
                vendorBootUri = uri,
                vendorBootName = name,
                done = false,
                success = null,
                message = "",
            )
        }
    }

    fun install() {
        val current = _uiState.value
        if (current.running || current.done) return
        val uri = current.vendorBootUri ?: return
        val ko = current.selectedKo ?: return

        _uiState.update { it.copy(running = true, done = false, success = null, message = "") }

        viewModelScope.launch {
            val log = StringBuilder()
            val exit =
                runCatching {
                    val staged =
                        withContext(Dispatchers.IO) {
                            VendorBootInstaller.stageVendorBoot(appContext, uri)
                        }
                    VendorBootInstaller.install(appContext, staged, ko) { line ->
                        log.append(line).append('\n')
                        _uiState.update { it.copy(message = log.toString()) }
                    }
                }.getOrElse { e ->
                    log.append("ERROR: ").append(e.message).append('\n')
                    _uiState.update { it.copy(message = log.toString()) }
                    -1
                }

            val ok = exit == 0
            _uiState.update {
                it.copy(
                    running = false,
                    done = true,
                    success = ok,
                    message = log.toString(),
                )
            }
        }
    }

    private data class LoadedKernelInfo(
        val kernelVersion: String?,
        val isGki: Boolean,
        val currentKmi: String?,
        val candidates: List<String>,
        val recommendedKo: String?,
    )
}

class InstallViewModelFactory(
    private val app: Application,
) : ViewModelProvider.Factory {
    @Suppress("UNCHECKED_CAST")
    override fun <T : ViewModel> create(modelClass: Class<T>): T = InstallViewModel(app) as T
}
