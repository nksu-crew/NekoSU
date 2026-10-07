package me.nekosu.aqnya.ui.screens

import android.net.Uri
import android.os.Build
import android.widget.Toast
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Rule
import androidx.compose.material.icons.filled.Android
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.Numbers
import androidx.compose.material.icons.filled.PhoneAndroid
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.outlined.*
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.blur
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import me.nekosu.aqnya.R
import me.nekosu.aqnya.ui.component.StatusCard
import me.nekosu.aqnya.util.VendorBootInstaller
import me.nekosu.aqnya.util.getAppVersion
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

enum class InstallStatus {
    INSTALLED,
    NEED_UPDATE,
    NEED_REBOOT,
    NOT_INSTALLED,
    UNKNOWN,
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HomeScreenContent(
    installStatus: InstallStatus,
    isGki: Boolean,
    suCount: Int,
    managerVersion: String,
    onNavigateToApps: () -> Unit,
    onInstallClick: () -> Unit,
    onAboutClick: () -> Unit = {},
) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Text(
                        text = stringResource(R.string.app_name),
                    )
                },
                colors =
                    TopAppBarDefaults.topAppBarColors(
                        containerColor = MaterialTheme.colorScheme.surface,
                        titleContentColor = MaterialTheme.colorScheme.onSurface,
                    ),
            )
        },
    ) { innerPadding ->
        Column(
            modifier =
                Modifier
                    .fillMaxSize()
                    .padding(innerPadding)
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = 16.dp, vertical = 12.dp)
                    .padding(bottom = 88.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            StatusCard(
                state = installStatus,
                isGki = isGki,
                onCardClick = onInstallClick,
                workingText = stringResource(R.string.running),
                versionText = "$managerVersion - ${if (isGki) "GKI" else "LKM"}",
                customBadgeText = if (isGki) "GKI" else "LKM",
            )

            if (installStatus == InstallStatus.INSTALLED) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    StatCard(
                        label = stringResource(R.string.superuser),
                        value = suCount.toString(),
                        modifier = Modifier.weight(1f),
                        bgIcon = Icons.Filled.Numbers,
                        onClick = onNavigateToApps,
                    )
                }
            }
            DeviceInfoCard(modifier = Modifier.fillMaxWidth())

            AboutCard(modifier = Modifier.fillMaxWidth(), onClick = onAboutClick)
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HomeScreen(
    viewModel: HomeViewModel,
    onNavigateToApps: () -> Unit = {},
    onAboutClick: () -> Unit = {},
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var showInstallSheet by remember { mutableStateOf(false) }

    val installStatus by viewModel.installStatus.collectAsState()
    val suCount by viewModel.suCount.collectAsState()
    val managerVersion by viewModel.managerVersion.collectAsState()

    var installState by remember { mutableStateOf(InstallState()) }

    LaunchedEffect(Unit) {
        viewModel.installStatus.collect { status ->
            if (status == InstallStatus.INSTALLED) {
                viewModel.refresh()
                return@collect
            }
        }
    }

    HomeScreenContent(
        installStatus = installStatus,
        isGki = false, // TODO: detect is gki or lkm install.
        suCount = suCount,
        managerVersion = managerVersion,
        onNavigateToApps = onNavigateToApps,
        onInstallClick = {
            if (installStatus != InstallStatus.INSTALLED) {
                val candidates =
                    runCatching { VendorBootInstaller.koCandidates(context) }.getOrDefault(emptyList())
                installState =
                    installState.copy(
                        koCandidates = candidates,
                        selectedKo = installState.selectedKo ?: candidates.firstOrNull(),
                        done = false,
                        success = null,
                        message = "",
                    )
                showInstallSheet = true
            } else {
                Toast.makeText(context, context.getString(R.string.running), Toast.LENGTH_SHORT).show()
            }
        },
        onAboutClick = onAboutClick,
    )

    InstallDialog(
        show = showInstallSheet,
        state = installState,
        onDismiss = { showInstallSheet = false },
        onPickVendorBoot = { uri ->
            val name = uri.lastPathSegment?.substringAfterLast('/') ?: uri.toString()
            installState =
                installState.copy(
                    vendorBootUri = uri,
                    vendorBootName = name,
                    done = false,
                    success = null,
                    message = "",
                )
        },
        onSelectKo = { ko -> installState = installState.copy(selectedKo = ko) },
        onStart = {
            val uri = installState.vendorBootUri
            if (uri == null) {
                installState = installState.copy(message = context.getString(R.string.install_pick_first))
                return@InstallDialog
            }
            val ko = installState.selectedKo
            if (ko == null) {
                installState = installState.copy(message = context.getString(R.string.install_ko_missing))
                return@InstallDialog
            }

            installState =
                installState.copy(
                    running = true,
                    done = false,
                    success = null,
                    message = "",
                )

            scope.launch {
                val log = StringBuilder()
                val exit =
                    runCatching {
                        val staged =
                            withContext(Dispatchers.IO) {
                                VendorBootInstaller.stageVendorBoot(context, uri)
                            }
                        VendorBootInstaller.install(context, staged, ko) { line ->
                            log.append(line).append('\n')
                            installState = installState.copy(message = log.toString())
                        }
                    }.getOrElse { e ->
                        log.append("ERROR: ").append(e.message).append('\n')
                        installState = installState.copy(message = log.toString())
                        -1
                    }

                val ok = exit == 0
                installState =
                    installState.copy(
                        running = false,
                        done = true,
                        success = ok,
                        message =
                            if (ok) {
                                context.getString(R.string.install_success) + "\n\n" + log
                            } else {
                                context.getString(R.string.install_failed) + "\n\n" + log
                            },
                    )
                Toast.makeText(
                    context,
                    if (ok) context.getString(R.string.install_success) else context.getString(R.string.install_failed),
                    Toast.LENGTH_LONG,
                ).show()
            }
        },
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun StatCard(
    label: String,
    value: String,
    modifier: Modifier = Modifier,
    bgIcon: ImageVector? = null,
    onClick: () -> Unit = {},
) {
    Card(
        modifier = modifier,
        onClick = onClick,
        shape = RoundedCornerShape(24.dp),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
        colors =
            CardDefaults.cardColors(
                containerColor = MaterialTheme.colorScheme.surfaceContainer,
            ),
    ) {
        Box(modifier = Modifier.fillMaxWidth()) {
            if (bgIcon != null) {
                Icon(
                    imageVector = bgIcon,
                    contentDescription = null,
                    modifier =
                        Modifier
                            .align(Alignment.BottomEnd)
                            .size(58.dp)
                            .padding(end = 18.dp, bottom = 12.dp),
                    tint = MaterialTheme.colorScheme.primary.copy(alpha = 0.35f),
                )
            }
            Column(
                modifier =
                    Modifier
                        .fillMaxWidth()
                        .padding(20.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                Text(
                    text = value,
                    style = MaterialTheme.typography.headlineMedium,
                    fontWeight = FontWeight.Bold,
                    color = MaterialTheme.colorScheme.onSurface,
                )
                Text(
                    text = label,
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.60f),
                )
            }
        }
    }
}

@Composable
fun DeviceInfoCard(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val appVersion = remember { getAppVersion(context) }

    val items =
        listOf(
            Triple(Icons.Filled.Memory, stringResource(id = R.string.kernel_version), System.getProperty("os.version") ?: "Unavailable"),
            Triple(Icons.Filled.Android, stringResource(id = R.string.android_version), Build.VERSION.RELEASE),
            Triple(Icons.Filled.PhoneAndroid, stringResource(id = R.string.device_model), "${Build.MANUFACTURER} ${Build.MODEL}"),
            Triple(Icons.Filled.Settings, stringResource(id = R.string.manager_version), appVersion),
            Triple(Icons.Outlined.Fingerprint,stringResource(id = R.string.finger_print),Build.FINGERPRINT),
        )

    Box(modifier = modifier.clip(RoundedCornerShape(28.dp))) {
        Box(
            modifier =
                Modifier
                    .matchParentSize()
                    .background(
                        brush =
                            Brush.verticalGradient(
                                colors =
                                    listOf(
                                        MaterialTheme.colorScheme.primary.copy(alpha = 0.08f),
                                        Color.Transparent,
                                    ),
                            ),
                    ).blur(24.dp),
        )

        Card(
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(28.dp),
            elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
            colors =
                CardDefaults.cardColors(
                    containerColor = MaterialTheme.colorScheme.surfaceContainer,
                ),
        ) {
                items.forEach { (icon, title, value) ->
                    DeviceInfoItem(icon = icon, title = title, value = value)
                }
        }
    }
}

@Composable
fun DeviceInfoItem(
    icon: ImageVector,
    title: String,
    value: String,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier =
            modifier
                .fillMaxWidth()
                .padding(horizontal = 20.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(
            imageVector = icon,
            contentDescription = null,
            modifier = Modifier.size(20.dp),
        )

        Spacer(modifier = Modifier.width(16.dp))

        Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(
                text = title,
                style = MaterialTheme.typography.bodyLarge
            )
            Text(
                text = value,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.outline,
            )
        }
    }
}

@Composable
fun AboutCard(
    modifier: Modifier = Modifier,
    onClick: () -> Unit = {},
) {
    val context = LocalContext.current

    Card(
        modifier = modifier,
        onClick = onClick,
        shape = RoundedCornerShape(28.dp),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
        colors =
            CardDefaults.cardColors(
                containerColor = MaterialTheme.colorScheme.surfaceContainer,
            ),
    ) {
        Row(
            modifier =
                Modifier
                    .fillMaxWidth()
                    .padding(20.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Spacer(modifier = Modifier.width(16.dp))

            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(2.dp),
            ) {
                Text(
                    text = stringResource(R.string.about_card_title),
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onSurface,
                )
                Text(
                    text = stringResource(R.string.about_card_description),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.60f),
                    maxLines = 2,
                )
            }
        }
    }
}

@Preview(showBackground = true, name = "Installed")
@Composable
fun HomeScreenPreviewInstalled() {
    MaterialTheme {
        HomeScreenContent(
            installStatus = InstallStatus.INSTALLED,
            isGki = true,
            suCount = 0,
            managerVersion = "1.0.0",
            onNavigateToApps = {},
            onInstallClick = {},
            onAboutClick = {},
        )
    }
}

@Preview(showBackground = true, name = "Not Install")
@Composable
fun HomeScreenPreviewNotInstalled() {
    MaterialTheme {
        HomeScreenContent(
            installStatus = InstallStatus.NOT_INSTALLED,
            isGki = false,
            suCount = 0,
            managerVersion = "1.0.0",
            onNavigateToApps = {},
            onInstallClick = {},
            onAboutClick = {},
        )
    }
}
