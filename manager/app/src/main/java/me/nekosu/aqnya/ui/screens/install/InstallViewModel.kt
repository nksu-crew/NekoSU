// SPDX-License-Identifier: GPL-3.0-or-later
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
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.nekosu.aqnya.core.model.InstallStrategy
import me.nekosu.aqnya.core.model.InstallTarget
import me.nekosu.aqnya.data.installer.BootInstaller
import me.nekosu.aqnya.data.installer.VendorBootInstaller
import me.nekosu.aqnya.data.kernel.KernelInfo
import me.nekosu.aqnya.data.prefs.AppPreferences
import me.nekosu.aqnya.data.shell.RootShell
import me.nekosu.aqnya.util.LocaleHelper

/** 日志刷新间隔，避免每来一行都做一次 O(n) 拼接并触发整页重组。 */
private const val LOG_EMIT_INTERVAL_MS = 50L

/**
 * 内核模块（LKM）的选择，对齐 KernelSU 的 `LkmSelection`：
 *  - [Auto]  按设备 KMI 自动匹配随 APK 打包的 nksu.ko；
 *  - [Kmi]   用户在弹窗里显式选择的 KMI 对应的打包模块；
 *  - [Local] 用户选择的本地 .ko 文件。
 *
 * 打包的候选模块不再平铺，只通过 KMI 选择弹窗选取。
 */
sealed interface LkmSelection {
    data object Auto : LkmSelection

    data class Kmi(
        val kmi: String,
    ) : LkmSelection

    data class Local(
        val uri: Uri,
        val name: String,
    ) : LkmSelection
}

/**
 * 安装页面的可观察状态。
 *
 * [target] 与 [strategy] 由 ChooseInstallScreen 选择后通过导航参数传入，安装页
 * 不再自行切换模式；某个模式不可用时只禁用安装并说明原因，不会静默回退。
 */
data class InstallUiState(
    val kernelLoaded: Boolean = false,
    val kernelVersion: String? = null,
    val isGki: Boolean = false,
    val currentKmi: String? = null,
    /** assets/ko 中的全部候选，仅用于内部匹配。 */
    val koCandidates: List<String> = emptyList(),
    /** 按设备 KMI 自动匹配到的打包模块名；无候选时为 null。 */
    val autoKo: String? = null,
    val lkmSelection: LkmSelection = LkmSelection.Auto,
    val target: InstallTarget = InstallTarget.VENDOR_BOOT,
    val strategy: InstallStrategy = InstallStrategy.IMAGE,
    val vendorBootUri: Uri? = null,
    val vendorBootName: String? = null,
    /** boot/init_boot image patched through the first-stage init wrapper. */
    val targetBootUri: Uri? = null,
    val targetBootName: String? = null,
    val bootUri: Uri? = null,
    val bootName: String? = null,
    val verifyKernelSymbols: Boolean = true,
    /** root 是否已检测完成；为 false 时 direct/inactive 只显示「检测中」。 */
    val rootKnown: Boolean = false,
    val rootAvailable: Boolean = false,
    val isAbDevice: Boolean = false,
    val directTarget: String? = null,
    val directBootTarget: String? = null,
    val inactiveTarget: String? = null,
    val inactiveBootTarget: String? = null,
    val initBootDirectTarget: String? = null,
    val initBootInactiveTarget: String? = null,
    val running: Boolean = false,
    val done: Boolean = false,
    val success: Boolean? = null,
    val message: String = "",
) {
    val directReady: Boolean
        get() =
            rootAvailable &&
                directTarget != null &&
                (!verifyKernelSymbols || directBootTarget != null)

    val inactiveReady: Boolean
        get() =
            rootAvailable &&
                inactiveTarget != null &&
                (!verifyKernelSymbols || inactiveBootTarget != null)

    val initBootDirectReady: Boolean
        get() = rootAvailable && initBootDirectTarget != null

    val initBootInactiveReady: Boolean
        get() = rootAvailable && initBootInactiveTarget != null

    /** 有可用的内核模块（自动匹配或本地文件）。 */
    val hasKernelModule: Boolean
        get() = lkmSelection is LkmSelection.Local || autoKo != null

    /** 指定模式要写入的分区路径；镜像模式与尚未检测到分区时为 null。 */
    fun partitionFor(
        target: InstallTarget,
        strategy: InstallStrategy,
    ): String? =
        when (target) {
            InstallTarget.VENDOR_BOOT ->
                when (strategy) {
                    InstallStrategy.IMAGE -> null
                    InstallStrategy.DIRECT -> directTarget
                    InstallStrategy.INACTIVE -> inactiveTarget
                }
            InstallTarget.INIT_BOOT ->
                when (strategy) {
                    InstallStrategy.IMAGE -> null
                    InstallStrategy.DIRECT -> initBootDirectTarget
                    InstallStrategy.INACTIVE -> initBootInactiveTarget
                }
        }

    /** vendor_boot 直接安装时用于校验内核符号的 boot 分区。 */
    fun bootPartitionFor(
        target: InstallTarget,
        strategy: InstallStrategy,
    ): String? =
        when (target) {
            InstallTarget.VENDOR_BOOT ->
                when (strategy) {
                    InstallStrategy.DIRECT -> directBootTarget
                    InstallStrategy.INACTIVE -> inactiveBootTarget
                    InstallStrategy.IMAGE -> null
                }
            InstallTarget.INIT_BOOT -> null
        }

    /** 当前选中模式要写入的分区路径。 */
    val selectedPartition: String?
        get() = partitionFor(target, strategy)

    /** 当前选中模式用于校验内核符号的 boot 分区。 */
    val selectedBootPartition: String?
        get() = bootPartitionFor(target, strategy)

    /** 模式所需的分区 / root 是否具备；选择页只看这一项，不看是否已选文件。 */
    fun isModeAvailable(
        target: InstallTarget,
        strategy: InstallStrategy,
    ): Boolean =
        when (strategy) {
            InstallStrategy.IMAGE -> true
            InstallStrategy.DIRECT ->
                if (target == InstallTarget.VENDOR_BOOT) directReady else initBootDirectReady
            InstallStrategy.INACTIVE ->
                if (target == InstallTarget.VENDOR_BOOT) inactiveReady else initBootInactiveReady
        }

    /** 模式可用且本次安装的输入已齐备。 */
    fun isModeReady(
        target: InstallTarget,
        strategy: InstallStrategy,
    ): Boolean =
        isModeAvailable(target, strategy) &&
            when (strategy) {
                InstallStrategy.IMAGE ->
                    when (target) {
                        InstallTarget.VENDOR_BOOT ->
                            vendorBootUri != null && (!verifyKernelSymbols || bootUri != null)
                        InstallTarget.INIT_BOOT -> targetBootUri != null
                    }
                InstallStrategy.DIRECT, InstallStrategy.INACTIVE -> true
            }

    val canInstall: Boolean
        get() = !running && hasKernelModule && isModeReady(target, strategy)
}

/**
 * boot / vendor_boot 安装流程的状态持有者。
 *
 * 通过 [KernelInfo] 展示 JNI 查询到的内核版本 / KMI，并据此自动选择匹配的
 * nksu.ko；模块不再全部列出，只保留一个「使用本地 LKM」入口（对齐 KernelSU）。
 * 安装模式（镜像 / 直接安装 / 非活动 slot + vendor_boot / init_boot）在
 * ChooseInstallScreen 中选择，这里只负责执行和展示结果。
 */
class InstallViewModel(
    app: Application,
    target: InstallTarget = InstallTarget.VENDOR_BOOT,
    strategy: InstallStrategy = InstallStrategy.IMAGE,
) : AndroidViewModel(app) {
    private val appContext = app.applicationContext

    private val _uiState =
        MutableStateFlow(
            InstallUiState(
                target = target,
                strategy = strategy,
                verifyKernelSymbols = AppPreferences.verifyKernelSymbols(appContext),
            ),
        )
    val uiState: StateFlow<InstallUiState> = _uiState.asStateFlow()

    private var kernelInfoLoaded = false

    init {
        viewModelScope.launch {
            RootShell.availability.filterNotNull().collect { hasRoot ->
                applyRootAvailability(hasRoot)
            }
        }
        viewModelScope.launch {
            AppPreferences.verifyKernelSymbolsFlow(appContext).collect { enabled ->
                _uiState.update { it.copy(verifyKernelSymbols = enabled) }
            }
        }
    }

    // 上一次把日志推送到 UI 的时间戳，用于合并高频输出。
    private var lastLogEmit = 0L

    /**
     * 按应用内当前所选语言解析字符串，供安装日志/错误本地化使用。
     *
     * 语言切换只会重建 Activity 而不会重建进程，因此这里每次安装时重新读取
     * 已保存的语言标签，避免沿用旧的 [android.content.res.Resources] 配置。
     */
    private fun i18nContext(): Context = LocaleHelper.wrap(appContext, LocaleHelper.savedLanguageTag(appContext))

    /** 载入内核信息与可用的内核模块，只执行一次。 */
    fun load() {
        if (kernelInfoLoaded) return
        kernelInfoLoaded = true
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
                        isAbDevice = VendorBootInstaller.isAbDevice(),
                    )
                }
            _uiState.update { current ->
                current.copy(
                    kernelLoaded = true,
                    kernelVersion = loaded.kernelVersion,
                    isGki = loaded.isGki,
                    currentKmi = loaded.currentKmi,
                    koCandidates = loaded.candidates,
                    autoKo = loaded.recommendedKo,
                    isAbDevice = loaded.isAbDevice,
                )
            }
        }
    }

    /** 设定本次安装模式；由安装页在进入时根据导航参数调用。 */
    fun setMode(
        target: InstallTarget,
        strategy: InstallStrategy,
    ) {
        if (_uiState.value.running) return
        _uiState.update { state ->
            if (state.target == target && state.strategy == strategy) {
                state
            } else {
                state.copy(
                    target = target,
                    strategy = strategy,
                    done = false,
                    success = null,
                    message = "",
                )
            }
        }
    }

    /** root 状态由启动阶段检测；这里只静默查找可直接安装的分区。 */
    private suspend fun applyRootAvailability(hasRoot: Boolean) {
        // RootShell 只覆盖 /system/bin/su；已加载 nksu 内核模块时也能直接安装。
        val effectiveRoot = hasRoot || withContext(Dispatchers.IO) { VendorBootInstaller.hasRoot() }
        val targets =
            withContext(Dispatchers.IO) {
                if (effectiveRoot) {
                    PartitionTargets(
                        vendorBoot = VendorBootInstaller.detectVendorBootPartition(),
                        boot = VendorBootInstaller.detectBootPartition(),
                        inactiveVendorBoot = VendorBootInstaller.detectInactiveVendorBootPartition(),
                        inactiveBoot = VendorBootInstaller.detectInactiveBootPartition(),
                        initBoot = BootInstaller.detectInitBootPartition(),
                        inactiveInitBoot = BootInstaller.detectInactiveInitBootPartition(),
                    )
                } else {
                    PartitionTargets()
                }
            }
        _uiState.update { state ->
            state.copy(
                rootKnown = true,
                rootAvailable = effectiveRoot,
                directTarget = targets.vendorBoot,
                directBootTarget = targets.boot,
                inactiveTarget = targets.inactiveVendorBoot,
                inactiveBootTarget = targets.inactiveBoot,
                initBootDirectTarget = targets.initBoot,
                initBootInactiveTarget = targets.inactiveInitBoot,
            )
        }
    }

    /** 使用本地 .ko 文件。 */
    fun selectLocalKo(uri: Uri) {
        if (_uiState.value.running) return
        val name =
            uri.lastPathSegment
                ?.substringAfterLast('/')
                ?.takeIf { it.isNotBlank() }
                ?: "nksu.ko"
        _uiState.update {
            it.copy(
                lkmSelection = LkmSelection.Local(uri, name),
                done = false,
                success = null,
                message = "",
            )
        }
    }

    /** 选择某个 KMI（弹窗确认后调用），使用对应的打包模块。 */
    fun selectKmi(kmi: String) {
        if (_uiState.value.running) return
        _uiState.update {
            it.copy(
                lkmSelection = LkmSelection.Kmi(kmi),
                done = false,
                success = null,
                message = "",
            )
        }
    }

    /** 清除本地 .ko，回到按 KMI 自动匹配。 */
    fun clearLocalKo() {
        if (_uiState.value.running) return
        _uiState.update {
            it.copy(
                lkmSelection = LkmSelection.Auto,
                done = false,
                success = null,
                message = "",
            )
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

    fun selectBoot(uri: Uri) {
        if (_uiState.value.running) return
        val name =
            uri.lastPathSegment
                ?.substringAfterLast('/')
                ?.takeIf { it.isNotBlank() }
                ?: uri.toString()
        _uiState.update {
            it.copy(
                bootUri = uri,
                bootName = name,
                done = false,
                success = null,
                message = "",
            )
        }
    }

    /** Select the boot/init_boot image that will receive the init wrapper. */
    fun selectTargetBoot(uri: Uri) {
        if (_uiState.value.running) return
        val name =
            uri.lastPathSegment
                ?.substringAfterLast('/')
                ?.takeIf { it.isNotBlank() }
                ?: uri.toString()
        _uiState.update {
            it.copy(
                targetBootUri = uri,
                targetBootName = name,
                done = false,
                success = null,
                message = "",
            )
        }
    }

    fun install() {
        val current = _uiState.value
        if (current.running || current.done) return
        if (!current.canInstall) return

        val koName = resolveKoName(current)
        val koUri = (current.lkmSelection as? LkmSelection.Local)?.uri

        _uiState.update { it.copy(running = true, done = false, success = null, message = "") }
        lastLogEmit = 0L

        viewModelScope.launch {
            val log = StringBuilder()
            val exit =
                when (current.strategy) {
                    InstallStrategy.IMAGE ->
                        when (current.target) {
                            InstallTarget.VENDOR_BOOT -> runVendorBootImageInstall(current, koName, koUri, log)
                            InstallTarget.INIT_BOOT -> runInitBootImageInstall(current, koName, koUri, log)
                        }
                    InstallStrategy.DIRECT, InstallStrategy.INACTIVE ->
                        when (current.target) {
                            InstallTarget.VENDOR_BOOT ->
                                runVendorBootDirectInstall(
                                    current,
                                    current.selectedPartition.orEmpty(),
                                    koName,
                                    koUri,
                                    log,
                                )
                            InstallTarget.INIT_BOOT ->
                                runInitBootDirectInstall(
                                    current,
                                    current.selectedPartition.orEmpty(),
                                    koName,
                                    koUri,
                                    log,
                                )
                        }
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

    private suspend fun runVendorBootImageInstall(
        current: InstallUiState,
        koName: String?,
        koUri: Uri?,
        log: StringBuilder,
    ): Int {
        val uri = current.vendorBootUri ?: return -1
        return runCatching {
            val ctx = i18nContext()
            val staged = withContext(Dispatchers.IO) { VendorBootInstaller.stageVendorBoot(ctx, uri) }
            val boot =
                current.bootUri
                    ?.takeIf { current.verifyKernelSymbols }
                    ?.let { bootUri -> withContext(Dispatchers.IO) { VendorBootInstaller.stageBoot(ctx, bootUri) } }
            VendorBootInstaller.install(
                ctx,
                staged,
                boot,
                koName,
                koUri,
                current.verifyKernelSymbols,
            ) { line -> appendLog(log, line) }
        }.getOrElse { e ->
            appendError(log, e)
            -1
        }
    }

    private suspend fun runInitBootImageInstall(
        current: InstallUiState,
        koName: String?,
        koUri: Uri?,
        log: StringBuilder,
    ): Int {
        val uri = current.targetBootUri ?: return -1
        return runCatching {
            val ctx = i18nContext()
            val staged = withContext(Dispatchers.IO) { BootInstaller.stageImage(ctx, uri) }
            BootInstaller.install(ctx, staged, koName, koUri) { line -> appendLog(log, line) }
        }.getOrElse { e ->
            appendError(log, e)
            -1
        }
    }

    private suspend fun runVendorBootDirectInstall(
        current: InstallUiState,
        partition: String,
        koName: String?,
        koUri: Uri?,
        log: StringBuilder,
    ): Int =
        runCatching {
            VendorBootInstaller.directInstall(
                i18nContext(),
                koName,
                koUri,
                partition,
                current.selectedBootPartition,
                current.verifyKernelSymbols,
            ) { line -> appendLog(log, line) }
        }.getOrElse { e ->
            appendError(log, e)
            -1
        }

    private suspend fun runInitBootDirectInstall(
        current: InstallUiState,
        partition: String,
        koName: String?,
        koUri: Uri?,
        log: StringBuilder,
    ): Int =
        runCatching {
            BootInstaller.directInstall(
                i18nContext(),
                koName,
                koUri,
                partition,
            ) { line -> appendLog(log, line) }
        }.getOrElse { e ->
            appendError(log, e)
            -1
        }

    private fun appendLog(
        log: StringBuilder,
        line: String,
    ) {
        log.append(line).append('\n')
        val now = System.currentTimeMillis()
        // 合并高频输出；安装结束时会再推一次完整日志。
        if (now - lastLogEmit >= LOG_EMIT_INTERVAL_MS) {
            lastLogEmit = now
            _uiState.update { it.copy(message = log.toString()) }
        }
    }

    private fun appendError(
        log: StringBuilder,
        e: Throwable,
    ) {
        log.append("ERROR: ").append(e.message).append('\n')
        _uiState.update { it.copy(message = log.toString()) }
    }

    /** 把当前选择解析成随包模块名（本地文件返回 null，改用 koUri）。 */
    private fun resolveKoName(state: InstallUiState): String? =
        when (val selection = state.lkmSelection) {
            is LkmSelection.Kmi -> state.koCandidates.firstOrNull { KernelInfo.kmiOf(it) == selection.kmi }
            LkmSelection.Auto -> state.autoKo
            is LkmSelection.Local -> null
        }

    private data class LoadedKernelInfo(
        val kernelVersion: String?,
        val isGki: Boolean,
        val currentKmi: String?,
        val candidates: List<String>,
        val recommendedKo: String?,
        val isAbDevice: Boolean,
    )

    private data class PartitionTargets(
        val vendorBoot: String? = null,
        val boot: String? = null,
        val inactiveVendorBoot: String? = null,
        val inactiveBoot: String? = null,
        val initBoot: String? = null,
        val inactiveInitBoot: String? = null,
    )
}

class InstallViewModelFactory(
    private val app: Application,
    private val target: InstallTarget = InstallTarget.VENDOR_BOOT,
    private val strategy: InstallStrategy = InstallStrategy.IMAGE,
) : ViewModelProvider.Factory {
    @Suppress("UNCHECKED_CAST")
    override fun <T : ViewModel> create(modelClass: Class<T>): T = InstallViewModel(app, target, strategy) as T
}
