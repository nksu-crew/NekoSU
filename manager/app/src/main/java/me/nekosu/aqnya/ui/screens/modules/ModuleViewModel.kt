package me.nekosu.aqnya.ui.screens.modules

import android.app.Application
import android.content.Context
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.nekosu.aqnya.R
import me.nekosu.aqnya.ui.component.SearchStatus
import me.nekosu.aqnya.util.DebugPreferences
import me.nekosu.aqnya.util.ModuleInfo
import me.nekosu.aqnya.util.ModuleRepository
import me.nekosu.aqnya.util.ModuleUpdateInfo
import java.io.File
import java.text.Collator
import java.util.Locale

class ModuleViewModel(app: Application) : AndroidViewModel(app) {
    private val _uiState = MutableStateFlow(ModuleUiState())
    val uiState: StateFlow<ModuleUiState> = _uiState.asStateFlow()

    private val _moduleEvent = Channel<ModuleEffect>(Channel.BUFFERED)
    val moduleEvent: Flow<ModuleEffect> = _moduleEvent.receiveAsFlow()

    private val appContext = app.applicationContext
    private val resources = app.resources
    private val prefs = appContext.getSharedPreferences(DebugPreferences.PREF_NAME, Context.MODE_PRIVATE)

    private var fetchJob: Job? = null

    init {
        _uiState.update {
            it.copy(
                sortEnabledFirst = prefs.getBoolean(KEY_SORT_ENABLED_FIRST, false),
                sortActionFirst = prefs.getBoolean(KEY_SORT_ACTION_FIRST, false),
            )
        }
    }

    fun toggleSortActionFirst() {
        val value = !_uiState.value.sortActionFirst
        prefs.edit().putBoolean(KEY_SORT_ACTION_FIRST, value).apply()
        _uiState.update { it.copy(sortActionFirst = value) }
        updateModuleList()
    }

    fun toggleSortEnabledFirst() {
        val value = !_uiState.value.sortEnabledFirst
        prefs.edit().putBoolean(KEY_SORT_ENABLED_FIRST, value).apply()
        _uiState.update { it.copy(sortEnabledFirst = value) }
        updateModuleList()
    }

    fun updateSearchStatus(status: SearchStatus) {
        val previous = _uiState.value.searchStatus
        _uiState.update { it.copy(searchStatus = status) }
        if (previous.searchText != status.searchText) {
            applySearchText(status.searchText)
        }
    }

    fun updateSearchText(text: String) {
        updateSearchStatus(_uiState.value.searchStatus.copy(searchText = text))
    }

    private fun filterModules(
        modules: List<ModuleInfo>,
        text: String,
    ): List<ModuleInfo> {
        if (text.isEmpty()) return emptyList()
        return modules.filter {
            it.id.contains(text, true) ||
                it.name.contains(text, true) ||
                it.description.contains(text, true) ||
                it.author.contains(text, true)
        }
    }

    private fun applySearchText(text: String) {
        if (text.isEmpty()) {
            updateModuleList()
            return
        }
        viewModelScope.launch(Dispatchers.IO) {
            val state = _uiState.value
            val result = filterModules(state.modules, text).sortedWith(moduleComparator(state))
            _uiState.update {
                it.copy(
                    searchResults = result,
                    searchStatus =
                        it.searchStatus.copy(
                            resultStatus =
                                if (result.isEmpty()) {
                                    SearchStatus.ResultStatus.EMPTY
                                } else {
                                    SearchStatus.ResultStatus.SHOW
                                },
                        ),
                )
            }
        }
    }

    private fun updateModuleList(resort: Boolean = true) {
        viewModelScope.launch(Dispatchers.IO) {
            val state = _uiState.value
            val searchText = state.searchStatus.searchText
            val sorted =
                if (resort || state.moduleList.isEmpty()) {
                    state.modules.sortedWith(moduleComparator(state))
                } else {
                    val byId = state.modules.associateBy { it.id }
                    val existingIds = state.moduleList.mapTo(HashSet()) { it.id }
                    state.moduleList.mapNotNull { byId[it.id] } + state.modules.filter { it.id !in existingIds }
                }
            val searchResults = filterModules(sorted, searchText)

            _uiState.update {
                it.copy(
                    moduleList = sorted,
                    searchResults = searchResults,
                    searchStatus =
                        it.searchStatus.copy(
                            resultStatus =
                                if (searchText.isEmpty()) {
                                    SearchStatus.ResultStatus.DEFAULT
                                } else if (searchResults.isEmpty()) {
                                    SearchStatus.ResultStatus.EMPTY
                                } else {
                                    SearchStatus.ResultStatus.SHOW
                                },
                        ),
                )
            }
        }
    }

    private fun moduleComparator(state: ModuleUiState): Comparator<ModuleInfo> =
        compareBy<ModuleInfo>(
            {
                val executable = it.hasWebUi || it.hasActionScript
                when {
                    it.metamodule && it.enabled -> 0
                    state.sortEnabledFirst && state.sortActionFirst ->
                        when {
                            it.enabled && executable -> 1
                            it.enabled -> 2
                            executable -> 3
                            else -> 4
                        }

                    state.sortEnabledFirst && !state.sortActionFirst -> if (it.enabled) 1 else 2
                    !state.sortEnabledFirst && state.sortActionFirst -> if (executable) 1 else 2
                    else -> 1
                }
            },
            { if (state.sortEnabledFirst) !it.enabled else 0 },
            { if (state.sortActionFirst) !(it.hasWebUi || it.hasActionScript) else 0 },
        ).thenBy(Collator.getInstance(Locale.getDefault()), ModuleInfo::id)

    fun fetchModuleList(
        @Suppress("UNUSED_PARAMETER") checkUpdate: Boolean = false,
        resort: Boolean = true,
    ) {
        fetchJob?.cancel()
        _uiState.update { it.copy(isRefreshing = true) }
        fetchJob =
            viewModelScope.launch {
                try {
                    val parsed =
                        withContext(Dispatchers.IO) {
                            ModuleRepository.list()
                        }
                    _uiState.update { it.copy(modules = parsed) }
                    updateModuleList(resort)
                } finally {
                    _uiState.update { it.copy(isRefreshing = false, hasLoaded = true) }
                }
            }
    }

    fun requestUpdateConfirmation(
        module: ModuleInfo,
        updateInfo: ModuleUpdateInfo,
    ) {
        _uiState.update {
            it.copy(
                confirmDialogState =
                    ModuleConfirmDialogState(
                        request =
                            ModuleConfirmRequest.Update(
                                module = module,
                                downloadUrl = updateInfo.downloadUrl,
                                fileName = "${module.name}-${updateInfo.version}.zip",
                            ),
                        title = resources.getString(R.string.module),
                        content = resources.getString(R.string.module_update_unsupported),
                        confirm = resources.getString(R.string.module_update),
                    ),
            )
        }
    }

    fun requestUninstallConfirmation(module: ModuleInfo) {
        _uiState.update {
            it.copy(
                confirmDialogState =
                    ModuleConfirmDialogState(
                        request = ModuleConfirmRequest.Uninstall(module),
                        title = resources.getString(R.string.module),
                        content =
                            resources.getString(
                                if (module.metamodule) {
                                    R.string.metamodule_uninstall_confirm
                                } else {
                                    R.string.module_uninstall_confirm
                                },
                            ).format(module.name),
                        confirm = resources.getString(R.string.uninstall),
                        dismiss = resources.getString(android.R.string.cancel),
                    ),
            )
        }
    }

    fun dismissConfirmRequest() {
        _uiState.update { it.copy(confirmDialogState = null) }
    }

    fun toggleModule(module: ModuleInfo) {
        viewModelScope.launch {
            val result =
                withContext(Dispatchers.IO) {
                    ModuleRepository.setEnabled(module.id, !module.enabled)
                }
            if (result.ok) {
                fetchModuleList(resort = false)
                emitEffect(ModuleEffect.SnackBar(resources.getString(R.string.reboot_to_apply)))
            } else {
                val message =
                    if (module.enabled) {
                        R.string.module_toggle_failed_disable
                    } else {
                        R.string.module_toggle_failed_enable
                    }
                emitEffect(ModuleEffect.Toast(resources.getString(message).format(module.name)))
            }
        }
    }

    fun uninstallModule(module: ModuleInfo) {
        viewModelScope.launch {
            val result =
                withContext(Dispatchers.IO) {
                    ModuleRepository.uninstall(module.id)
                }
            _uiState.update { it.copy(confirmDialogState = null) }
            if (result.ok) {
                fetchModuleList(resort = false)
                emitEffect(ModuleEffect.SnackBar(resources.getString(R.string.module_uninstall_success).format(module.name)))
            } else {
                emitEffect(ModuleEffect.Toast(resources.getString(R.string.module_uninstall_failed).format(module.name)))
            }
        }
    }

    fun undoUninstallModule(module: ModuleInfo) {
        viewModelScope.launch {
            val result =
                withContext(Dispatchers.IO) {
                    ModuleRepository.undoRemove(module.id)
                }
            if (result.ok) {
                fetchModuleList(resort = false)
                emitEffect(ModuleEffect.SnackBar(resources.getString(R.string.module_undo_uninstall_success).format(module.name)))
            } else {
                emitEffect(ModuleEffect.Toast(resources.getString(R.string.module_undo_uninstall_failed).format(module.name)))
            }
        }
    }

    fun install(uris: List<Uri>) {
        if (uris.isEmpty()) return
        viewModelScope.launch {
            val messages = mutableListOf<String>()
            withContext(Dispatchers.IO) {
                uris.forEachIndexed { index, uri ->
                    val zip = stageZip(uri, index)
                    if (zip == null) {
                        messages += resources.getString(R.string.module_install_failed).format("unreadable file")
                        return@forEachIndexed
                    }
                    val result = ModuleRepository.install(zip.absolutePath)
                    messages +=
                        if (result.ok) {
                            resources.getString(R.string.module_install_success).format(zip.name)
                        } else {
                            resources.getString(R.string.module_install_failed).format(result.output.ifBlank { "unknown error" })
                        }
                }
            }
            emitEffect(ModuleEffect.Toast(messages.joinToString("\n")))
            fetchModuleList(resort = false)
        }
    }

    fun emitEffect(effect: ModuleEffect) {
        _moduleEvent.trySend(effect)
    }

    /** Copy the selected zip into the app cache dir so the root shell can read it. */
    private fun stageZip(
        uri: Uri,
        index: Int,
    ): File? =
        runCatching {
            val file = File(appContext.cacheDir, "module-install-$index.zip")
            appContext.contentResolver.openInputStream(uri)?.use { input ->
                file.outputStream().use { output -> input.copyTo(output) }
            } ?: return null
            file
        }.getOrNull()

    private companion object {
        const val KEY_SORT_ENABLED_FIRST = "module_sort_enabled_first"
        const val KEY_SORT_ACTION_FIRST = "module_sort_action_first"
    }
}

class ModuleViewModelFactory(
    private val app: Application,
) : ViewModelProvider.Factory {
    @Suppress("UNCHECKED_CAST")
    override fun <T : ViewModel> create(modelClass: Class<T>): T = ModuleViewModel(app) as T
}
