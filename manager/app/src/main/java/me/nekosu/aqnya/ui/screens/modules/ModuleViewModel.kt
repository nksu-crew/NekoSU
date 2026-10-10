package me.nekosu.aqnya.ui.screens.modules

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
import me.nekosu.aqnya.util.ModuleInfo
import me.nekosu.aqnya.util.ModuleRepository
import me.nekosu.aqnya.util.RootShell
import java.io.File

data class ModulesUiState(
    val modules: List<ModuleInfo> = emptyList(),
    val loading: Boolean = false,
    val available: Boolean = true,
    val installing: Boolean = false,
    val installLog: String = "",
    val installDone: Boolean = false,
    val installSuccess: Boolean? = null,
    val actionModuleId: String? = null,
    val actionName: String = "",
    val actionRunning: Boolean = false,
    val actionOutput: String = "",
    val actionDone: Boolean = false,
    val actionSuccess: Boolean = true,
    val message: String? = null,
)

class ModuleViewModel(app: Application) : AndroidViewModel(app) {
    private val _uiState = MutableStateFlow(ModulesUiState())
    val uiState: StateFlow<ModulesUiState> = _uiState.asStateFlow()

    private val appContext = app.applicationContext

    fun refresh() {
        _uiState.update { it.copy(loading = true) }
        viewModelScope.launch {
            val list = withContext(Dispatchers.IO) { ModuleRepository.list(appContext) }
            _uiState.update {
                it.copy(
                    modules = list,
                    loading = false,
                    available = true,
                )
            }
        }
    }

    fun setEnabled(
        module: ModuleInfo,
        enabled: Boolean,
    ) {
        viewModelScope.launch {
            val result = withContext(Dispatchers.IO) { ModuleRepository.setEnabled(module.id, enabled) }
            if (!result.ok) {
                _uiState.update { it.copy(message = result.output.ifBlank { "failed" }) }
            }
            refresh()
        }
    }

    fun remove(module: ModuleInfo) {
        viewModelScope.launch {
            val result = withContext(Dispatchers.IO) { ModuleRepository.markRemove(module.id) }
            if (!result.ok) {
                _uiState.update { it.copy(message = result.output.ifBlank { "failed" }) }
            } else {
                _uiState.update { it.copy(message = "will be removed on next boot") }
            }
            refresh()
        }
    }

    fun uninstall(module: ModuleInfo) {
        viewModelScope.launch {
            val result = withContext(Dispatchers.IO) { ModuleRepository.uninstall(module.id) }
            if (!result.ok) {
                _uiState.update { it.copy(message = result.output.ifBlank { "failed" }) }
            }
            refresh()
        }
    }

    fun install(uri: Uri) {
        if (_uiState.value.installing) return
        _uiState.update {
            it.copy(
                installing = true,
                installLog = "",
                installDone = false,
                installSuccess = null,
            )
        }
        viewModelScope.launch {
            val result =
                withContext(Dispatchers.IO) {
                    val zip = stageZip(uri)
                    if (zip == null) {
                        ModuleRepository.Result(-1, "unable to read the selected file")
                    } else {
                        ModuleRepository.install(zip.absolutePath)
                    }
                }
            _uiState.update {
                it.copy(
                    installing = false,
                    installDone = true,
                    installSuccess = result.ok,
                    installLog = result.output,
                )
            }
            if (result.ok) refresh()
        }
    }

    fun resetInstall() {
        _uiState.update { it.copy(installDone = false, installSuccess = null, installLog = "") }
    }

    /**
     * 执行模块的 `action.sh`，把输出实时回填到 [ModulesUiState.actionOutput]。
     *
     * 输出回调来自 root shell 的读取线程；[MutableStateFlow] 本身线程安全，因此可直接 update。
     */
    fun runAction(module: ModuleInfo) {
        if (_uiState.value.actionRunning) return
        _uiState.update {
            it.copy(
                actionModuleId = module.id,
                actionName = module.title,
                actionRunning = true,
                actionOutput = "",
                actionDone = false,
                actionSuccess = true,
            )
        }
        viewModelScope.launch {
            val buffer = StringBuilder()
            val code =
                withContext(Dispatchers.IO) {
                    RootShell.execStreaming(ModuleRepository.actionCommand(appContext, module.id)) { line ->
                        buffer.append(line).append('\n')
                        _uiState.update { state -> state.copy(actionOutput = buffer.toString()) }
                    }
                }
            _uiState.update {
                it.copy(
                    actionRunning = false,
                    actionDone = true,
                    actionSuccess = code == 0,
                    actionOutput = buffer.toString(),
                )
            }
        }
    }

    fun resetAction() {
        _uiState.update {
            it.copy(
                actionModuleId = null,
                actionName = "",
                actionOutput = "",
                actionDone = false,
                actionRunning = false,
            )
        }
    }

    fun consumeMessage() {
        _uiState.update { it.copy(message = null) }
    }

    /** 复制所选 zip 到应用缓存目录，root shell 才有权访问。 */
    private fun stageZip(uri: Uri): File? =
        runCatching {
            val file = File(appContext.cacheDir, "module-install.zip")
            appContext.contentResolver.openInputStream(uri)?.use { input ->
                file.outputStream().use { output -> input.copyTo(output) }
            } ?: return null
            file
        }.getOrNull()
}

class ModuleViewModelFactory(
    private val app: Application,
) : ViewModelProvider.Factory {
    @Suppress("UNCHECKED_CAST")
    override fun <T : ViewModel> create(modelClass: Class<T>): T = ModuleViewModel(app) as T
}
