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
import me.nekosu.aqnya.util.RootShell
import me.nekosu.aqnya.util.VendorBootInstaller

/** 安装方式：选择本地镜像，或直接读写本机 vendor_boot 分区。 */
enum class InstallMethod { FILE, DIRECT }

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
    val method: InstallMethod = InstallMethod.FILE,
    val vendorBootUri: Uri? = null,
    val vendorBootName: String? = null,
    val directChecking: Boolean = false,
    val directTarget: String? = null,
    val rootAvailable: Boolean? = null,
    val running: Boolean = false,
    val done: Boolean = false,
    val success: Boolean? = null,
    val message: String = "",
) {
    /** 直接安装是否就绪：已取得 root 且找到 vendor_boot 分区。 */
    val directReady: Boolean get() = rootAvailable == true && directTarget != null

    val canInstall: Boolean
        get() =
            !running &&
                selectedKo != null &&
                when (method) {
                    InstallMethod.FILE -> vendorBootUri != null
                    InstallMethod.DIRECT -> directReady
                }
}

/**
 * vendor_boot 安装流程的状态持有者。
 *
 * 通过 [KernelInfo] 展示 JNI 查询到的内核版本 / KMI，并据此预选匹配的 nksu.ko。
 * 安装支持两种方式：
 *  - [InstallMethod.FILE]：用户提供 vendor_boot 镜像，仅生成补丁镜像导出到 Download；
 *  - [InstallMethod.DIRECT]：以 root 读取本机 vendor_boot、打补丁写回分区，无需手动刷入。
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

    fun selectMethod(method: InstallMethod) {
        if (_uiState.value.running || _uiState.value.method == method) return
        _uiState.update { it.copy(method = method, done = false, success = null, message = "") }
        if (method == InstallMethod.DIRECT) checkDirect()
    }

    /** 检测 root 与 vendor_boot 分区，供直接安装使用。 */
    fun checkDirect() {
        if (_uiState.value.directChecking) return
        _uiState.update { it.copy(directChecking = true, rootAvailable = null, directTarget = null) }
        viewModelScope.launch {
            val (root, target) =
                withContext(Dispatchers.IO) {
                    val hasRoot = RootShell.available()
                    val part = if (hasRoot) VendorBootInstaller.detectVendorBootPartition() else null
                    hasRoot to part
                }
            _uiState.update { it.copy(directChecking = false, rootAvailable = root, directTarget = target) }
        }
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
        val ko = current.selectedKo ?: return
        if (!current.canInstall) return

        _uiState.update { it.copy(running = true, done = false, success = null, message = "") }

        viewModelScope.launch {
            val log = StringBuilder()
            val exit =
                when (current.method) {
                    InstallMethod.FILE -> runFileInstall(current, ko, log)
                    InstallMethod.DIRECT -> runDirectInstall(ko, log)
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

    private suspend fun runFileInstall(
        current: InstallUiState,
        ko: String,
        log: StringBuilder,
    ): Int {
        val uri = current.vendorBootUri ?: return -1
        return runCatching {
            val staged = withContext(Dispatchers.IO) { VendorBootInstaller.stageVendorBoot(appContext, uri) }
            VendorBootInstaller.install(appContext, staged, ko) { line -> appendLog(log, line) }
        }.getOrElse { e ->
            appendError(log, e)
            -1
        }
    }

    private suspend fun runDirectInstall(
        ko: String,
        log: StringBuilder,
    ): Int =
        runCatching {
            VendorBootInstaller.directInstall(appContext, ko) { line -> appendLog(log, line) }
        }.getOrElse { e ->
            appendError(log, e)
            -1
        }

    private fun appendLog(
        log: StringBuilder,
        line: String,
    ) {
        log.append(line).append('\n')
        _uiState.update { it.copy(message = log.toString()) }
    }

    private fun appendError(
        log: StringBuilder,
        e: Throwable,
    ) {
        log.append("ERROR: ").append(e.message).append('\n')
        _uiState.update { it.copy(message = log.toString()) }
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
