package me.nekosu.aqnya.ui.screens.install

import android.app.Application
import android.content.Context
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
import me.nekosu.aqnya.util.LocaleHelper
import me.nekosu.aqnya.util.RootShell
import me.nekosu.aqnya.util.VendorBootInstaller

/**
 * 安装方式，与 KernelSU 的安装页对应：
 *  - [FILE]     选择镜像（不需要 root，仅生成补丁镜像）
 *  - [DIRECT]   直接安装到当前 slot（需要 root + GKI）
 *  - [INACTIVE] 安装到非活动 slot（需要 root + GKI + A/B）
 */
enum class InstallMethod { FILE, DIRECT, INACTIVE }

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
    val rootChecking: Boolean = false,
    val rootChecked: Boolean = false,
    val rootAvailable: Boolean = false,
    val directTarget: String? = null,
    val inactiveTarget: String? = null,
    val running: Boolean = false,
    val done: Boolean = false,
    val success: Boolean? = null,
    val message: String = "",
) {
    val directReady: Boolean get() = rootAvailable && directTarget != null

    val inactiveReady: Boolean get() = rootAvailable && inactiveTarget != null

    val canInstall: Boolean
        get() =
            !running &&
                selectedKo != null &&
                when (method) {
                    InstallMethod.FILE -> vendorBootUri != null
                    InstallMethod.DIRECT -> directReady
                    InstallMethod.INACTIVE -> inactiveReady
                }
}

/**
 * vendor_boot 安装流程的状态持有者。
 *
 * 通过 [KernelInfo] 展示 JNI 查询到的内核版本 / KMI，并据此预选匹配的 nksu.ko。
 * 安装方式与 KernelSU 对齐：选择镜像、直接安装、安装到非活动 slot；后两者
 * 只有在检测到 root（且为 GKI 设备）时才显示。
 */
class InstallViewModel(app: Application) : AndroidViewModel(app) {
    private val _uiState = MutableStateFlow(InstallUiState())
    val uiState: StateFlow<InstallUiState> = _uiState.asStateFlow()

    private val appContext = app.applicationContext

    /**
     * 按应用内当前所选语言解析字符串，供安装日志/错误本地化使用。
     *
     * 语言切换只会重建 Activity 而不会重建进程，因此这里每次安装时重新读取
     * 已保存的语言标签，避免沿用旧的 [android.content.res.Resources] 配置。
     */
    private fun i18nContext(): Context =
        LocaleHelper.wrap(appContext, LocaleHelper.savedLanguageTag(appContext))

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
            checkRoot()
        }
    }

    /** 检测 root 与可用的 vendor_boot 分区（当前 / 非活动 slot）。 */
    fun checkRoot() {
        if (_uiState.value.rootChecking) return
        _uiState.update { it.copy(rootChecking = true) }
        viewModelScope.launch {
            val (root, current, inactive) =
                withContext(Dispatchers.IO) {
                    val hasRoot = RootShell.available()
                    val currentPart = if (hasRoot) VendorBootInstaller.detectVendorBootPartition() else null
                    val inactivePart = if (hasRoot) VendorBootInstaller.detectInactiveVendorBootPartition() else null
                    Triple(hasRoot, currentPart, inactivePart)
                }
            _uiState.update { state ->
                val method =
                    when (state.method) {
                        InstallMethod.DIRECT -> if (root && current != null) state.method else InstallMethod.FILE
                        InstallMethod.INACTIVE -> if (root && inactive != null) state.method else InstallMethod.FILE
                        InstallMethod.FILE -> state.method
                    }
                state.copy(
                    rootChecking = false,
                    rootChecked = true,
                    rootAvailable = root,
                    directTarget = current,
                    inactiveTarget = inactive,
                    method = method,
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
                method = InstallMethod.FILE,
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
                    InstallMethod.DIRECT -> runDirectInstall(current.directTarget.orEmpty(), ko, log)
                    InstallMethod.INACTIVE -> runDirectInstall(current.inactiveTarget.orEmpty(), ko, log)
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
            val ctx = i18nContext()
            val staged = withContext(Dispatchers.IO) { VendorBootInstaller.stageVendorBoot(ctx, uri) }
            VendorBootInstaller.install(ctx, staged, ko) { line -> appendLog(log, line) }
        }.getOrElse { e ->
            appendError(log, e)
            -1
        }
    }

    private suspend fun runDirectInstall(
        partition: String,
        ko: String,
        log: StringBuilder,
    ): Int =
        runCatching {
            VendorBootInstaller.directInstall(i18nContext(), ko, partition) { line -> appendLog(log, line) }
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
