@file:OptIn(ExperimentalSerializationApi::class)

package me.nekosu.aqnya.ui.screens

import android.content.Context
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.util.LruCache
import androidx.annotation.StringRes
import androidx.compose.animation.*
import androidx.compose.animation.Crossfade
import androidx.compose.animation.core.*
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.material3.pulltorefresh.PullToRefreshDefaults.Indicator
import androidx.compose.material3.pulltorefresh.rememberPullToRefreshState
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.core.graphics.drawable.toBitmap
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavController
import androidx.navigation.compose.currentBackStackEntryAsState
import dev.chrisbanes.haze.hazeSource
import dev.chrisbanes.haze.rememberHazeState
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.ExperimentalSerializationApi
import kotlinx.serialization.Serializable
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.protobuf.ProtoBuf
import me.nekosu.aqnya.R
import me.nekosu.aqnya.ncore
import me.nekosu.aqnya.ui.component.SearchAppBar
import me.nekosu.aqnya.ui.component.groupShape
import java.io.File

@Serializable
data class AppInfo(
    val name: String,
    val packageName: String,
    val uid: Int,
    val isSystem: Boolean,
    val isLaunchable: Boolean,
)

enum class FilterMode(
    @param:StringRes val labelRes: Int,
) {
    ALL(R.string.all_app),
    LAUNCHABLE(R.string.can_launch_app),
    SYSTEM(R.string.system_app),
    USER(R.string.user_app),
}

private val proto = ProtoBuf

class AppViewModel(
    private val context: Context,
) : ViewModel() {
    val listState = LazyListState()

    private var _filterMode = mutableStateOf(FilterMode.USER)
    var filterMode: FilterMode
        get() = _filterMode.value
        set(value) {
            _filterMode.value = value
            updateFilteredApps()
        }

    private var _searchQuery = mutableStateOf("")
    var searchQuery: String
        get() = _searchQuery.value
        set(value) {
            _searchQuery.value = value
            updateFilteredApps()
        }

    var isSearching by mutableStateOf(false)

    var allApps by mutableStateOf<List<AppInfo>>(emptyList())
        private set

    // 预计算小写名字，供过滤 / 排序复用。
    private var allAppsLower: List<Pair<AppInfo, String>> = emptyList()

    var isLoaded by mutableStateOf(false)
        private set

    // 内核按 UID 保存 profile；管理器只读它、不再自己落盘。
    private var profilesByUid: Map<Int, AppConfig> = emptyMap()

    var appConfigs by mutableStateOf<Map<String, AppConfig>>(emptyMap())
        private set

    var filteredApps by mutableStateOf<List<AppInfo>>(emptyList())
        private set

    val pinnedApps: Set<String>
        get() = appConfigs.keys

    init {
        viewModelScope.launch(Dispatchers.IO) {
            loadAppConfigs()
        }
    }

    private fun setAllApps(apps: List<AppInfo>) {
        allApps = apps
        allAppsLower = apps.map { it to it.name.lowercase() }
    }

    private fun updateFilteredApps() {
        val q = searchQuery.trim().lowercase()
        val configs = appConfigs
        filteredApps =
            allAppsLower
                .filter { (app, nameLower) ->
                    val passFilter =
                        when (filterMode) {
                            FilterMode.ALL -> true
                            FilterMode.LAUNCHABLE -> app.isLaunchable
                            FilterMode.SYSTEM -> app.isSystem
                            FilterMode.USER -> !app.isSystem
                        }
                    passFilter &&
                        (
                            q.isEmpty() ||
                                nameLower.contains(q) ||
                                app.packageName.contains(q, ignoreCase = true)
                        )
                }.sortedWith(
                    compareBy(
                        { (app, _) -> if (app.packageName in configs) 0 else 1 },
                        { (_, nameLower) -> nameLower },
                    ),
                ).map { it.first }
    }

    // 内核保存的是 UID → profile；这里映射回包名供界面使用。
    private fun rebuildConfigs() {
        appConfigs =
            allApps
                .mapNotNull { app -> profilesByUid[app.uid]?.let { app.packageName to it } }
                .toMap()
    }

    /**
     * 内核提供 API 返回 profile 表，管理器通过 JNI 获取；它不再自己保存。
     * 控制 fd 尚不可用时返回空表。
     */
    private fun readKernelProfiles(): Map<Int, AppConfig> {
        val text =
            try {
                ncore.listProfiles()
            } catch (_: Throwable) {
                null
            } ?: return emptyMap()
        return parseAllowProfile(text)
    }

    private suspend fun loadAppConfigs() =
        withContext(Dispatchers.IO) {
            try {
                profilesByUid = readKernelProfiles()
                rebuildConfigs()
            } catch (e: Exception) {
                e.printStackTrace()
            }
            withContext(Dispatchers.Main) { updateFilteredApps() }
        }

    private fun appsCacheFile(hash: String) = File(context.cacheDir, "apps_cache_$hash.pb")

    suspend fun loadApps(forceRefresh: Boolean = false) =
        withContext(Dispatchers.IO) {
            val pm = context.packageManager
            val packages = pm.getInstalledPackages(0)

            // 与顺序无关的滚动哈希：PM 返回顺序不保证稳定。
            var hash = packages.size
            for (p in packages) {
                hash = hash xor (p.packageName.hashCode() * 31 + p.longVersionCode.hashCode())
            }
            val cacheKey = hash.toString()

            if (!forceRefresh) {
                val cacheFile = appsCacheFile(cacheKey)
                if (cacheFile.exists()) {
                    try {
                        val cached =
                            proto.decodeFromByteArray(
                                ListSerializer(AppInfo.serializer()),
                                cacheFile.readBytes(),
                            )
                        if (cached.isNotEmpty()) {
                            setAllApps(cached)
                            isLoaded = true
                            loadAppConfigs()
                            return@withContext
                        }
                    } catch (_: Exception) {
                        cacheFile.delete()
                    }
                }
            }

            val launcherIntent =
                Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
            val launchable =
                pm.queryIntentActivities(launcherIntent, PackageManager.ResolveInfoFlags.of(0))
                    .mapNotNullTo(HashSet<String>()) { it.activityInfo?.packageName }

            val apps = ArrayList<AppInfo>(packages.size)
            for (pkg in packages) {
                val ai = pkg.applicationInfo ?: continue
                apps +=
                    AppInfo(
                        name = ai.loadLabel(pm).toString().takeIf { it.isNotBlank() } ?: pkg.packageName,
                        packageName = pkg.packageName,
                        uid = ai.uid,
                        isSystem = (ai.flags and ApplicationInfo.FLAG_SYSTEM) != 0,
                        isLaunchable = pkg.packageName in launchable,
                    )
            }
            apps.sortBy { it.name.lowercase() }
            setAllApps(apps)
            isLoaded = true

            context.cacheDir
                .listFiles { f ->
                    f.name.startsWith("apps_cache_") &&
                        f.name.endsWith(".pb") &&
                        f.name != "apps_cache_$cacheKey.pb"
                }?.forEach(File::delete)
            appsCacheFile(cacheKey).writeBytes(
                proto.encodeToByteArray(
                    ListSerializer(AppInfo.serializer()),
                    apps,
                ),
            )
            loadAppConfigs()
        }

    fun setAppConfig(
        app: AppInfo,
        config: AppConfig,
    ) {
        profilesByUid =
            if (config.allowed) {
                profilesByUid + (app.uid to config)
            } else {
                profilesByUid - app.uid
            }
        rebuildConfigs()
        updateFilteredApps()

        // 只把变更下发给内核；内核负责落盘（allow.profile），管理器不再保存。
        viewModelScope.launch(Dispatchers.IO) {
            try {
                if (config.allowed) {
                    val capsBits = config.caps.fold(0L) { acc, cap -> acc or (1L shl cap.value) }
                    ncore.setProfile(app.uid, capsBits, config.selinuxDomain, config.namespace.value)
                } else {
                    ncore.deluid(app.uid)
                }
            } catch (e: Exception) {
                android.util.Log.e("AppViewModel", "JNI call failed, will recover on next launch", e)
            }
        }
    }

    fun refreshAppConfig(packageName: String) {
        if (packageName.isBlank()) return
        viewModelScope.launch(Dispatchers.IO) {
            profilesByUid = readKernelProfiles()
            rebuildConfigs()
            withContext(Dispatchers.Main) { updateFilteredApps() }
        }
    }
}

/** 解析内核 API 返回的 profile 表：每行 `<uid> <caps_hex> <ns> <domain>`。 */
private fun parseAllowProfile(text: String): Map<Int, AppConfig> {
    val profiles = mutableMapOf<Int, AppConfig>()
    text.lineSequence().forEach { raw ->
        val line = raw.trim()
        if (line.isEmpty() || line.startsWith("#")) return@forEach

        val parts = line.split(Regex("\\s+"))
        if (parts.size < 3) return@forEach

        val uid = parts[0].toIntOrNull() ?: return@forEach
        val caps = parts[1].toULongOrNull(16)?.toLong() ?: 0L
        val nsValue = parts[2].toIntOrNull() ?: NksuNamespace.INHERITED.value
        val domain = if (parts.size >= 4) parts[3] else "u:r:nksu:s0"

        val capSet = LinuxCap.entries.filter { ((caps shr it.value) and 1L) == 1L }.toSet()
        val namespace = NksuNamespace.entries.find { it.value == nsValue } ?: NksuNamespace.INHERITED
        profiles[uid] =
            AppConfig(
                allowed = true,
                caps = capSet,
                selinuxDomain = domain,
                namespace = namespace,
            )
    }
    return profiles
}

class AppViewModelFactory(
    private val context: Context,
) : ViewModelProvider.Factory {
    @Suppress("UNCHECKED_CAST")
    override fun <T : ViewModel> create(modelClass: Class<T>): T = AppViewModel(context) as T
}

@Composable
fun getAdapterShape(
    index: Int,
    totalCount: Int,
    cornerRadius: Dp = 20.dp,
): Shape = groupShape(index, totalCount, cornerRadius)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AppListScreen(
    navController: NavController,
    extraBottomPadding: Dp = 0.dp,
) {
    val context = LocalContext.current.applicationContext
    val viewModel: AppViewModel = viewModel(factory = AppViewModelFactory(context))
    val listState = viewModel.listState
    val apps = viewModel.filteredApps
    val filterMode = viewModel.filterMode
    val searchQuery = viewModel.searchQuery
    val isSearching = viewModel.isSearching
    var menuExpanded by remember { mutableStateOf(false) }
    var isRefreshing by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    val refreshState = rememberPullToRefreshState()
    val hazeState = rememberHazeState()

    LaunchedEffect(Unit) { viewModel.loadApps() }

    val navBackStackEntry by navController.currentBackStackEntryAsState()
    LaunchedEffect(navBackStackEntry) {
        val route = navBackStackEntry?.destination?.route ?: return@LaunchedEffect
        if (route.startsWith("app_detail/")) {
        } else {
            val prevEntry = navController.previousBackStackEntry
            val pkg = prevEntry?.arguments?.getString("packageName") ?: return@LaunchedEffect
            delay(150)
            viewModel.refreshAppConfig(pkg)
        }
    }

    Scaffold(
        topBar = {
            SearchAppBar(
                title = {
                    Text(stringResource(filterMode.labelRes))
                },
                searchText = searchQuery,
                onSearchTextChange = { viewModel.searchQuery = it },
                onClearClick = {
                    viewModel.isSearching = false
                    viewModel.searchQuery = ""
                },
                onBackClick = null,
                onConfirm = {},
                dropdownContent = {
                    Box {
                        IconButton(onClick = { menuExpanded = true }) {
                            Icon(Icons.Default.FilterList, null)
                        }
                        DropdownMenu(
                            expanded = menuExpanded,
                            onDismissRequest = { menuExpanded = false },
                            shape = RoundedCornerShape(20.dp),
                        ) {
                            FilterMode.entries.forEach { mode ->
                                DropdownMenuItem(
                                    text = {
                                        Text(
                                            stringResource(mode.labelRes),
                                            fontWeight = if (mode == filterMode) FontWeight.SemiBold else FontWeight.Normal,
                                        )
                                    },
                                    leadingIcon =
                                        if (mode == filterMode) {
                                            { Icon(Icons.Default.CheckCircle, null, Modifier.size(18.dp)) }
                                        } else {
                                            null
                                        },
                                    onClick = {
                                        viewModel.filterMode = mode
                                        menuExpanded = false
                                    },
                                )
                            }
                        }
                    }
                },
                leadingActions = null,
                trailingActions = null,
                startInSearchMode = false,
                hazeState = hazeState,
            )
        },
        contentWindowInsets = WindowInsets(0),
    ) { innerPadding ->
        PullToRefreshBox(
            isRefreshing = isRefreshing,
            onRefresh = {
                if (!isRefreshing) {
                    isRefreshing = true
                    scope.launch {
                        viewModel.loadApps(forceRefresh = true)
                        isRefreshing = false
                    }
                }
            },
            state = refreshState,
            indicator = {
                Indicator(
                    modifier =
                        Modifier
                            .align(Alignment.TopCenter)
                            .padding(top = innerPadding.calculateTopPadding()),
                    isRefreshing = isRefreshing,
                    state = refreshState,
                    color = MaterialTheme.colorScheme.primary,
                    containerColor = MaterialTheme.colorScheme.surfaceVariant,
                )
            },
            modifier =
                Modifier
                    .fillMaxSize()
                    .padding(horizontal = 16.dp),
            contentAlignment = Alignment.Center,
        ) {
            when {
                !viewModel.isLoaded -> {
                    Box(
                        modifier = Modifier.fillMaxSize().padding(innerPadding),
                        contentAlignment = Alignment.Center,
                    ) {
                        LoadingState()
                    }
                }

                apps.isEmpty() -> {
                    Box(
                        modifier = Modifier.fillMaxSize().padding(innerPadding),
                        contentAlignment = Alignment.Center,
                    ) {
                        EmptyState(isSearching, searchQuery)
                    }
                }

                else -> {
                    val pinnedApps = viewModel.pinnedApps
                    val (pinnedList, otherList) = apps.partition { it.packageName in pinnedApps }

                    LazyColumn(
                        state = listState,
                        modifier = Modifier.fillMaxSize().hazeSource(hazeState),
                        verticalArrangement = Arrangement.spacedBy(2.dp),
                        contentPadding =
                            PaddingValues(
                                top = innerPadding.calculateTopPadding() + 12.dp,
                                bottom = extraBottomPadding,
                            ),
                    ) {
                        val allItems = pinnedList + otherList

                        itemsIndexed(
                            allItems,
                            key = { _, app -> app.packageName },
                        ) { index, app ->
                            AppInfoItem(
                                app = app,
                                config = viewModel.appConfigs[app.packageName],
                                onClick = { navController.navigate("app_detail/${app.packageName}") },
                                shape = groupShape(index, allItems.size),
                                modifier = Modifier.animateItem(),
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
fun SectionLabel(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.labelMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f),
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier.padding(start = 4.dp, top = 6.dp, bottom = 2.dp),
    )
}

@Composable
fun LoadingState() {
    Column(
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        CircularProgressIndicator(strokeWidth = 2.5.dp, modifier = Modifier.size(36.dp))
        Text(
            stringResource(R.string.loading_app),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f),
        )
    }
}

@Composable
fun EmptyState(
    isSearching: Boolean,
    query: String,
) {
    Column(
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Icon(
            imageVector = if (isSearching) Icons.Default.Search else Icons.Default.Android,
            contentDescription = null,
            modifier = Modifier.size(40.dp),
            tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.25f),
        )
        Text(
            text = if (isSearching && query.isNotBlank()) "未找到「$query」" else "暂无应用",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.5f),
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AppInfoItem(
    app: AppInfo,
    config: AppConfig?,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    shape: Shape = RoundedCornerShape(20.dp),
) {
    val isAllowed = config?.allowed == true
    val haptic = LocalHapticFeedback.current

    Card(
        onClick = {
            haptic.performHapticFeedback(HapticFeedbackType.VirtualKey)
            onClick()
        },
        modifier = modifier.fillMaxWidth(),
        shape = shape,
        colors =
            CardDefaults.cardColors(
                containerColor = MaterialTheme.colorScheme.surfaceContainer,
            ),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
    ) {
        Row(
            modifier =
                Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            AppIcon(packageName = app.packageName, modifier = Modifier.size(42.dp))

            Spacer(Modifier.width(14.dp))

            Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.Center) {
                Text(
                    text = app.name,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    text = app.packageName,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }

            Row(
                horizontalArrangement = Arrangement.spacedBy(4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (isAllowed) AppTag(label = "allowed", color = MaterialTheme.colorScheme.primary)
                if (app.isSystem) AppTag(label = "system", color = MaterialTheme.colorScheme.secondary)
            }
        }
    }
}

@Composable
fun AppTag(
    label: String,
    color: Color,
) {
    Surface(shape = RoundedCornerShape(4.dp), color = color.copy(alpha = 0.12f)) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            color = color,
            fontWeight = FontWeight.Medium,
            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
        )
    }
}

private val iconCache =
    object : LruCache<String, ImageBitmap>(16 * 1024 * 1024) {
        override fun sizeOf(
            key: String,
            value: ImageBitmap,
        ): Int = value.width * value.height * 4
    }

/** 图标最多画到 42dp，超过这个尺寸解码没有意义。 */
private const val ICON_MAX_PX = 160

@Composable
fun AppIcon(
    packageName: String,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    var iconBitmap by remember(packageName) { mutableStateOf(iconCache.get(packageName)) }

    LaunchedEffect(packageName) {
        if (iconBitmap == null) {
            withContext(Dispatchers.IO) {
                try {
                    val drawable = context.packageManager.getApplicationIcon(packageName)
                    val bitmap =
                        drawable
                            .toBitmap(ICON_MAX_PX, ICON_MAX_PX)
                            .copy(Bitmap.Config.ARGB_8888, false)
                            .asImageBitmap()
                    iconCache.put(packageName, bitmap)
                    iconBitmap = bitmap
                } catch (_: Exception) {
                }
            }
        }
    }

    Crossfade(targetState = iconBitmap, label = "icon_crossfade") { bitmap ->
        if (bitmap != null) {
            Image(bitmap = bitmap, contentDescription = "App Icon", modifier = modifier)
        } else {
            Box(modifier = modifier, contentAlignment = Alignment.Center) {
                Icon(
                    imageVector = Icons.Default.Android,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary.copy(alpha = 0.3f),
                    modifier = Modifier.size(24.dp),
                )
            }
        }
    }
}

@Preview(showBackground = true)
@Composable
fun PreviewAppListScreen() {
    val navController = androidx.navigation.compose.rememberNavController()
    MaterialTheme {
        AppListScreen(navController = navController)
    }
}
