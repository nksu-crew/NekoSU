@file:OptIn(ExperimentalSerializationApi::class)

package me.nekosu.aqnya.ui.screens

import android.content.Context
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.os.Build
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
import androidx.compose.material.icons.rounded.*
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
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.ExperimentalSerializationApi
import kotlinx.serialization.Serializable
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.protobuf.ProtoBuf
import me.nekosu.aqnya.R
import me.nekosu.aqnya.core.Ncore
import me.nekosu.aqnya.core.navigation.AppRoute
import me.nekosu.aqnya.ui.component.AppCard
import me.nekosu.aqnya.ui.component.SearchAppBar
import me.nekosu.aqnya.ui.component.groupShape
import me.nekosu.aqnya.ui.theme.ContinuousCornerShape
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

/** 搜索输入防抖：停止输入一小段时间后才真正过滤，避免每敲一个字都全量排序。 */
private const val SEARCH_DEBOUNCE_MS = 150L

/** 复用同一个空白正则，避免每行 profile 都构造一次。 */
private val PROFILE_WHITESPACE = Regex("\\s+")

class AppViewModel(
    private val context: Context,
) : ViewModel() {
    val listState = LazyListState()

    private var _filterMode = mutableStateOf(FilterMode.USER)
    var filterMode: FilterMode
        get() = _filterMode.value
        set(value) {
            if (_filterMode.value == value) return
            _filterMode.value = value
            updateFilteredApps()
        }

    private var _searchQuery = mutableStateOf("")
    var searchQuery: String
        get() = _searchQuery.value
        set(value) {
            if (_searchQuery.value == value) return
            _searchQuery.value = value
            updateFilteredApps(debounceMs = SEARCH_DEBOUNCE_MS)
        }

    // 过滤 / 排序任务的持有者；新任务会取消旧任务，避免过期结果覆盖新结果。
    private var filterJob: Job? = null

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

    private fun applyApps(apps: List<AppInfo>) {
        allApps = apps
        allAppsLower = apps.map { it to it.name.lowercase() }
    }

    /**
     * 过滤 + 排序。搜索输入走 [debounceMs] 防抖，真正的计算放到
     * [Dispatchers.Default]，结果再回到主线程发布，避免在 UI 线程做全量排序。
     */
    private fun updateFilteredApps(debounceMs: Long = 0L) {
        filterJob?.cancel()
        filterJob =
            viewModelScope.launch {
                if (debounceMs > 0L) delay(debounceMs)
                // 在主线程取一份快照，保证后台计算读到一致的状态。
                val q = searchQuery.trim().lowercase()
                val mode = filterMode
                val configs = appConfigs
                val source = allAppsLower
                val result =
                    withContext(Dispatchers.Default) {
                        source
                            .filter { (app, nameLower) ->
                                val passFilter =
                                    when (mode) {
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
                filteredApps = result
            }
    }

    // 内核保存的是 UID → profile；这里映射回包名供界面使用。
    private fun rebuildConfigs() {
        appConfigs =
            allApps
                .mapNotNull { app -> profilesByUid[app.uid]?.let { app.packageName to it } }
                .toMap()
    }

    private fun readKernelProfiles(): Map<Int, AppConfig> {
        val text =
            Ncore.listProfiles()
                ?: error("listProfiles() returned null: control fd unavailable or ioctl failed")
        val map = parseAllowProfile(text)
        if (map.isEmpty() && text.isNotBlank()) {
            error("listProfiles() returned non-empty data but parsed to empty map: $text")
        }
        return map
    }

    private suspend fun loadAppConfigs() =
        withContext(Dispatchers.IO) {
            try {
                profilesByUid = readKernelProfiles()
                rebuildConfigs()
            } catch (e: Exception) {
                e.printStackTrace()
            }
            // updateFilteredApps 自己会回到主线程发布结果。
            updateFilteredApps()
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
                            applyApps(cached)
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
            val resolved =
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    pm.queryIntentActivities(launcherIntent, PackageManager.ResolveInfoFlags.of(0))
                } else {
                    @Suppress("DEPRECATION")
                    pm.queryIntentActivities(launcherIntent, 0)
                }
            val launchable = resolved.mapNotNullTo(HashSet<String>()) { it.activityInfo?.packageName }

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
            applyApps(apps)
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
                    Ncore.setProfile(app.uid, config.rootUid, capsBits, config.selinuxDomain, config.namespace.value)
                } else {
                    Ncore.deluid(app.uid)
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
            // updateFilteredApps 自己会回到主线程发布结果。
            updateFilteredApps()
        }
    }
}

/** 解析内核 API 返回的 profile 表：每行 `<uid> <caps_hex> <ns> <root_uid> <domain>`（旧版无 root_uid）。 */
private fun parseAllowProfile(text: String): Map<Int, AppConfig> {
    val profiles = mutableMapOf<Int, AppConfig>()
    text.lineSequence().forEach { raw ->
        val line = raw.trim()
        if (line.isEmpty() || line.startsWith("#")) return@forEach

        val parts = line.split(PROFILE_WHITESPACE)
        if (parts.size < 3) return@forEach

        val uid = parts[0].toIntOrNull() ?: return@forEach
        val caps = parts[1].toULongOrNull(16)?.toLong() ?: 0L
        val nsValue = parts[2].toIntOrNull() ?: NksuNamespace.INHERITED.value
        // 新版格式带 root_uid：<uid> <caps_hex> <ns> <root_uid> <domain>；
        // 旧版没有该字段，只有 4 段（或 3 段，省略 domain）。
        val rootUid = if (parts.size >= 5) parts[3].toIntOrNull() ?: 0 else 0
        val domainRaw =
            when {
                parts.size >= 5 -> parts[4]
                parts.size == 4 -> parts[3]
                else -> null
            }
        val domain = if (domainRaw.isNullOrEmpty() || domainRaw == "-") "u:r:nksu:s0" else domainRaw

        val capSet = LinuxCap.entries.filter { ((caps shr it.value) and 1L) == 1L }.toSet()
        val namespace = NksuNamespace.entries.find { it.value == nsValue } ?: NksuNamespace.INHERITED
        profiles[uid] =
            AppConfig(
                allowed = true,
                rootUid = rootUid,
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
    val isSearching = viewModel.isSearching
    var menuExpanded by remember { mutableStateOf(false) }
    var isRefreshing by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    val refreshState = rememberPullToRefreshState()
    val hazeState = rememberHazeState()
    val motionScheme = MaterialTheme.motionScheme

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
        containerColor = MaterialTheme.colorScheme.surfaceContainer,
        topBar = {
            SearchAppBar(
                title = stringResource(filterMode.labelRes),
                // 搜索文本在输入框自己的作用域里读取，避免每敲一个字都重组整个列表。
                searchText = viewModel.searchQuery,
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
                            Icon(Icons.Rounded.FilterList, null)
                        }
                        DropdownMenu(
                            expanded = menuExpanded,
                            onDismissRequest = { menuExpanded = false },
                            shape = ContinuousCornerShape(20.dp),
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
                                            { Icon(Icons.Rounded.CheckCircle, null, Modifier.size(18.dp)) }
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
                    containerColor = MaterialTheme.colorScheme.surfaceBright,
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
                        EmptyState(isSearching, viewModel.searchQuery)
                    }
                }

                else -> {
                    // 排序结果已经稳定（可能很大），把它按「已授权在前」分好段并缓存，
                    // 避免每次重组都重新 partition + 拼接。
                    val pinnedApps = viewModel.pinnedApps
                    val allItems =
                        remember(apps, pinnedApps) {
                            val (pinnedList, otherList) = apps.partition { it.packageName in pinnedApps }
                            pinnedList + otherList
                        }

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
                        itemsIndexed(
                            allItems,
                            key = { _, app -> app.packageName },
                        ) { index, app ->
                            AppInfoItem(
                                app = app,
                                config = viewModel.appConfigs[app.packageName],
                                onClick = { navController.navigate(AppRoute.appDetail(app.packageName)) },
                                shape = groupShape(index, allItems.size),
                                modifier =
                                    Modifier.animateItem(
                                        fadeInSpec = motionScheme.fastEffectsSpec(),
                                        placementSpec = motionScheme.defaultSpatialSpec(),
                                        fadeOutSpec = motionScheme.fastEffectsSpec(),
                                    ),
                            )
                        }
                    }
                }
            }
        }
    }
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
            imageVector = if (isSearching) Icons.Rounded.Search else Icons.Rounded.Android,
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
    shape: Shape = ContinuousCornerShape(20.dp),
) {
    val isAllowed = config?.allowed == true
    val haptic = LocalHapticFeedback.current

    AppCard(
        onClick = {
            haptic.performHapticFeedback(HapticFeedbackType.VirtualKey)
            onClick()
        },
        modifier = modifier.fillMaxWidth(),
        shape = shape,
        containerColor = MaterialTheme.colorScheme.surfaceBright,
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
    Surface(shape = RoundedCornerShape(50), color = color.copy(alpha = 0.12f)) {
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
    val motionScheme = MaterialTheme.motionScheme
    var iconBitmap by remember(packageName) { mutableStateOf(iconCache.get(packageName)) }

    LaunchedEffect(packageName) {
        if (iconBitmap == null) {
            withContext(Dispatchers.IO) {
                try {
                    val drawable = context.packageManager.getApplicationIcon(packageName)
                    // 已经是不可变的位图无需再复制一份；Compose 需要不可变位图才能走硬件绘制。
                    val raw = drawable.toBitmap(ICON_MAX_PX, ICON_MAX_PX)
                    val safe = if (raw.isMutable) raw.copy(Bitmap.Config.ARGB_8888, false) else raw
                    val bitmap = safe.asImageBitmap()
                    iconCache.put(packageName, bitmap)
                    iconBitmap = bitmap
                } catch (_: Exception) {
                }
            }
        }
    }

    Crossfade(
        targetState = iconBitmap,
        animationSpec = motionScheme.defaultEffectsSpec(),
        label = "icon_crossfade",
    ) { bitmap ->
        if (bitmap != null) {
            Image(bitmap = bitmap, contentDescription = "App Icon", modifier = modifier)
        } else {
            Box(modifier = modifier, contentAlignment = Alignment.Center) {
                Icon(
                    imageVector = Icons.Rounded.Android,
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
