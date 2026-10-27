// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.screens

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.scaleOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material.icons.automirrored.rounded.KeyboardArrowRight
import androidx.compose.material.icons.rounded.Badge
import androidx.compose.material.icons.rounded.Close
import androidx.compose.material.icons.rounded.Layers
import androidx.compose.material.icons.rounded.Lock
import androidx.compose.material.icons.rounded.LockOpen
import androidx.compose.material.icons.rounded.Search
import androidx.compose.material.icons.rounded.Shield
import androidx.compose.material.icons.rounded.Tune
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.Saver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import me.nekosu.aqnya.R
import me.nekosu.aqnya.ui.component.AppDialog
import me.nekosu.aqnya.ui.component.AppListItem
import me.nekosu.aqnya.ui.component.AppTopAppBar
import me.nekosu.aqnya.ui.component.CardGroup
import me.nekosu.aqnya.ui.component.ExpressiveTextField
import me.nekosu.aqnya.ui.component.ModeLabel

/** Build an [AppConfig] from the screen's draft values (used for live saving). */
private fun buildConfig(
    allowed: Boolean,
    rootUid: String,
    caps: Set<LinuxCap>,
    domain: String,
    ns: NksuNamespace,
): AppConfig =
    AppConfig(
        allowed = allowed,
        rootUid = rootUid.toIntOrNull() ?: 0,
        caps = caps,
        selinuxDomain = domain,
        namespace = ns,
    )

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun CapsSheet(
    current: Set<LinuxCap>,
    onDismiss: () -> Unit,
    onSelectionChange: (Set<LinuxCap>) -> Unit,
) {
    val haptic = LocalHapticFeedback.current
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    var query by rememberSaveable { mutableStateOf("") }

    val filtered =
        remember(query) {
            val q = query.trim()
            if (q.isEmpty()) {
                LinuxCap.entries
            } else {
                LinuxCap.entries.filter { cap ->
                    cap.label.contains(q, ignoreCase = true) ||
                        "CAP_${cap.label}".contains(q, ignoreCase = true) ||
                        cap.value.toString() == q
                }
            }
        }

    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = sheetState,
        containerColor = MaterialTheme.colorScheme.surfaceContainerLow,
    ) {
        Column(
            modifier =
                Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 24.dp)
                    .padding(bottom = 24.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = stringResource(R.string.app_capabilities),
                        style = MaterialTheme.typography.headlineSmall,
                    )
                    Text(
                        text = stringResource(R.string.app_caps_selected, current.size, LinuxCap.entries.size),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                IconButton(
                    onClick = {
                        haptic.performHapticFeedback(HapticFeedbackType.VirtualKey)
                        onDismiss()
                    },
                ) {
                    Icon(Icons.Rounded.Close, contentDescription = stringResource(R.string.cd_close))
                }
            }

            ExpressiveTextField(
                value = query,
                onValueChange = { query = it },
                label = stringResource(R.string.caps_search_hint),
                leadingIcon = {
                    Icon(Icons.Rounded.Search, contentDescription = null, modifier = Modifier.size(20.dp))
                },
                trailingIcon = {
                    if (query.isNotEmpty()) {
                        IconButton(onClick = { query = "" }) {
                            Icon(
                                Icons.Rounded.Close,
                                contentDescription = stringResource(R.string.cd_clear),
                                modifier = Modifier.size(18.dp),
                            )
                        }
                    }
                },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                CapsPresetButton(
                    text = stringResource(R.string.dialog_select_all),
                    modifier = Modifier.weight(1f),
                    onClick = {
                        haptic.performHapticFeedback(HapticFeedbackType.VirtualKey)
                        onSelectionChange(LinuxCap.entries.toSet())
                    },
                )
                CapsPresetButton(
                    text = stringResource(R.string.dialog_default),
                    modifier = Modifier.weight(1f),
                    onClick = {
                        haptic.performHapticFeedback(HapticFeedbackType.VirtualKey)
                        onSelectionChange(DEFAULT_CAPS)
                    },
                )
                CapsPresetButton(
                    text = stringResource(R.string.dialog_clear),
                    modifier = Modifier.weight(1f),
                    onClick = {
                        haptic.performHapticFeedback(HapticFeedbackType.VirtualKey)
                        onSelectionChange(emptySet())
                    },
                )
            }

            LazyColumn(
                modifier = Modifier.fillMaxWidth().weight(1f, fill = false),
                verticalArrangement = Arrangement.spacedBy(2.dp),
            ) {
                if (filtered.isEmpty()) {
                    item {
                        Text(
                            text = stringResource(R.string.caps_empty),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            textAlign = TextAlign.Center,
                            modifier = Modifier.fillMaxWidth().padding(vertical = 24.dp),
                        )
                    }
                }
                items(filtered, key = { it.value }) { cap ->
                    CapListItem(
                        cap = cap,
                        checked = current.contains(cap),
                        onToggle = {
                            onSelectionChange(if (current.contains(cap)) current - cap else current + cap)
                        },
                    )
                }
            }
        }
    }
}

@Composable
private fun CapsPresetButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    FilledTonalButton(
        onClick = onClick,
        modifier = modifier.defaultMinSize(minHeight = 40.dp),
        contentPadding = PaddingValues(horizontal = 8.dp, vertical = 6.dp),
    ) {
        Text(
            text = text,
            style = MaterialTheme.typography.labelLarge,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

@Composable
private fun CapListItem(
    cap: LinuxCap,
    checked: Boolean,
    onToggle: () -> Unit,
) {
    val haptic = LocalHapticFeedback.current
    val containerColor =
        if (checked) {
            MaterialTheme.colorScheme.secondaryContainer
        } else {
            Color.Transparent
        }
    val contentColor =
        if (checked) {
            MaterialTheme.colorScheme.onSecondaryContainer
        } else {
            MaterialTheme.colorScheme.onSurface
        }
    val supportingColor =
        if (checked) {
            MaterialTheme.colorScheme.onSecondaryContainer
        } else {
            MaterialTheme.colorScheme.onSurfaceVariant
        }

    ListItem(
        modifier =
            Modifier
                .fillMaxWidth()
                .clip(MaterialTheme.shapes.medium)
                .toggleable(
                    value = checked,
                    role = Role.Checkbox,
                    onValueChange = {
                        haptic.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                        onToggle()
                    },
                ),
        colors =
            ListItemDefaults.colors(
                containerColor = containerColor,
                contentColor = contentColor,
                supportingContentColor = supportingColor,
            ),
        leadingContent = { Checkbox(checked = checked, onCheckedChange = null) },
        content = {
            Text(
                text = "CAP_${cap.label}",
                style = MaterialTheme.typography.bodyMedium,
                fontFamily = FontFamily.Monospace,
            )
        },
        supportingContent = {
            Text(
                text = stringResource(cap.descriptionRes),
                style = MaterialTheme.typography.labelSmall,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
        },
        trailingContent = {
            Text(
                text = cap.value.toString(),
                style = MaterialTheme.typography.labelSmall,
                fontFamily = FontFamily.Monospace,
                color = supportingColor,
            )
        },
    )
}

@Composable
private fun UidDialog(
    current: String,
    onDismiss: () -> Unit,
    onConfirm: (String) -> Unit,
) {
    val haptic = LocalHapticFeedback.current
    var draft by remember { mutableStateOf(current) }

    AppDialog(
        onDismissRequest = onDismiss,
        title = stringResource(R.string.app_root_uid),
        confirmText = stringResource(R.string.dialog_confirm),
        onConfirm = {
            haptic.performHapticFeedback(HapticFeedbackType.VirtualKey)
            onConfirm(draft)
        },
        dismissText = stringResource(R.string.dialog_cancel),
    ) {
        ExpressiveTextField(
            value = draft,
            onValueChange = { value ->
                if (value.length <= 10 && value.all { it.isDigit() }) draft = value
            },
            label = stringResource(R.string.app_root_uid_input),
            supportingText = stringResource(R.string.app_root_uid_desc),
            placeholder = "0",
            monospace = true,
            trailingIcon = {
                if (draft.isNotEmpty()) {
                    IconButton(onClick = { draft = "" }) {
                        Icon(
                            Icons.Rounded.Close,
                            contentDescription = stringResource(R.string.cd_clear),
                            modifier = Modifier.size(18.dp),
                        )
                    }
                }
            },
            keyboardOptions =
                KeyboardOptions(
                    keyboardType = KeyboardType.Number,
                    imeAction = ImeAction.Done,
                ),
        )
    }
}

@Composable
private fun DomainDialog(
    current: String,
    onDismiss: () -> Unit,
    onConfirm: (String) -> Unit,
) {
    val haptic = LocalHapticFeedback.current
    var draft by remember { mutableStateOf(current) }

    AppDialog(
        onDismissRequest = onDismiss,
        title = stringResource(R.string.app_selinux_domain),
        confirmText = stringResource(R.string.dialog_confirm),
        onConfirm = {
            haptic.performHapticFeedback(HapticFeedbackType.VirtualKey)
            onConfirm(draft.trim())
        },
        dismissText = stringResource(R.string.dialog_cancel),
    ) {
        ExpressiveTextField(
            value = draft,
            onValueChange = { draft = it },
            label = stringResource(R.string.app_selinux_domain_input),
            supportingText = stringResource(R.string.app_selinux_domain_desc),
            placeholder = "u:r:nksu:s0",
            monospace = true,
            leadingIcon = {
                Icon(Icons.Rounded.Shield, contentDescription = null, modifier = Modifier.size(20.dp))
            },
            trailingIcon = {
                if (draft.isNotEmpty()) {
                    IconButton(onClick = { draft = "" }) {
                        Icon(
                            Icons.Rounded.Close,
                            contentDescription = stringResource(R.string.cd_clear),
                            modifier = Modifier.size(18.dp),
                        )
                    }
                }
            },
            keyboardOptions =
                KeyboardOptions(
                    keyboardType = KeyboardType.Ascii,
                    imeAction = ImeAction.Done,
                ),
        )
    }
}

@Composable
private fun NamespaceDialog(
    current: NksuNamespace,
    onDismiss: () -> Unit,
    onConfirm: (NksuNamespace) -> Unit,
) {
    val haptic = LocalHapticFeedback.current
    var draft by remember { mutableStateOf(current) }

    AppDialog(
        onDismissRequest = onDismiss,
        title = stringResource(R.string.app_mount_namespace),
        confirmText = stringResource(R.string.dialog_confirm),
        onConfirm = {
            haptic.performHapticFeedback(HapticFeedbackType.VirtualKey)
            onConfirm(draft)
        },
        dismissText = stringResource(R.string.dialog_cancel),
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            NksuNamespace.entries.forEach { option ->
                ListItem(
                    modifier =
                        Modifier
                            .fillMaxWidth()
                            .clip(MaterialTheme.shapes.medium)
                            .selectable(
                                selected = draft == option,
                                role = Role.RadioButton,
                                onClick = {
                                    haptic.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                                    draft = option
                                },
                            ),
                    colors =
                        ListItemDefaults.colors(
                            containerColor = Color.Transparent,
                            contentColor = MaterialTheme.colorScheme.onSurface,
                            supportingContentColor = MaterialTheme.colorScheme.onSurfaceVariant,
                        ),
                    leadingContent = { RadioButton(selected = draft == option, onClick = null) },
                    content = {
                        Text(
                            text = stringResource(option.labelRes),
                            style = MaterialTheme.typography.bodyLarge,
                        )
                    },
                    supportingContent = {
                        Text(
                            text = stringResource(option.descriptionRes),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    },
                )
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Screen
// ─────────────────────────────────────────────────────────────────────────────

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AppDetailScreen(
    app: AppInfo,
    config: AppConfig?,
    onSave: (AppConfig) -> Unit,
    onBack: () -> Unit,
) {
    var allowed by rememberSaveable { mutableStateOf(config?.allowed ?: false) }

    val capsSaver =
        remember {
            val labelMap = LinuxCap.entries.associateBy { it.label }
            Saver<Set<LinuxCap>, List<String>>(
                save = { it.map { cap -> cap.label } },
                restore = { it.mapNotNull { label -> labelMap[label] }.toSet() },
            )
        }
    var caps by rememberSaveable(stateSaver = capsSaver) { mutableStateOf(config?.caps ?: DEFAULT_CAPS) }

    var domain by rememberSaveable { mutableStateOf(config?.selinuxDomain ?: "u:r:nksu:s0") }

    // root 进程使用的 UID：0 表示真正的 root，否则提权后切换到该 UID。
    var rootUidText by rememberSaveable { mutableStateOf((config?.rootUid ?: 0).toString()) }

    val nsSaver =
        remember {
            val valueMap = NksuNamespace.entries.associateBy { it.value }
            Saver<NksuNamespace, Int>(
                save = { it.value },
                restore = { valueMap[it] ?: NksuNamespace.INHERITED },
            )
        }
    var ns by rememberSaveable(stateSaver = nsSaver) {
        mutableStateOf(config?.namespace ?: NksuNamespace.INHERITED)
    }

    var showCapsSheet by rememberSaveable { mutableStateOf(false) }
    var showUidDialog by rememberSaveable { mutableStateOf(false) }
    var showDomainDialog by rememberSaveable { mutableStateOf(false) }
    var showNsDialog by rememberSaveable { mutableStateOf(false) }
    val haptic = LocalHapticFeedback.current
    val motionScheme = MaterialTheme.motionScheme
    val scrollBehavior = TopAppBarDefaults.pinnedScrollBehavior(rememberTopAppBarState())

    // 实时保存：任何一处改动都会立即下发，不再需要保存按钮。
    fun save(
        allowedValue: Boolean = allowed,
        rootUidValue: String = rootUidText,
        capsValue: Set<LinuxCap> = caps,
        domainValue: String = domain,
        nsValue: NksuNamespace = ns,
    ) = onSave(buildConfig(allowedValue, rootUidValue, capsValue, domainValue, nsValue))

    if (showCapsSheet) {
        CapsSheet(
            current = caps,
            onDismiss = { showCapsSheet = false },
            onSelectionChange = { selected ->
                caps = selected
                save(capsValue = selected)
            },
        )
    }
    if (showUidDialog) {
        UidDialog(
            current = rootUidText,
            onDismiss = { showUidDialog = false },
            onConfirm = { value ->
                val resolved = value.ifBlank { "0" }
                rootUidText = resolved
                showUidDialog = false
                save(rootUidValue = resolved)
            },
        )
    }
    if (showDomainDialog) {
        DomainDialog(
            current = domain,
            onDismiss = { showDomainDialog = false },
            onConfirm = { value ->
                val resolved = value.ifBlank { "u:r:nksu:s0" }
                domain = resolved
                showDomainDialog = false
                save(domainValue = resolved)
            },
        )
    }
    if (showNsDialog) {
        NamespaceDialog(
            current = ns,
            onDismiss = { showNsDialog = false },
            onConfirm = { value ->
                ns = value
                showNsDialog = false
                save(nsValue = value)
            },
        )
    }

    Scaffold(
        containerColor = MaterialTheme.colorScheme.surfaceContainer,
        topBar = {
            AppTopAppBar(
                title = stringResource(R.string.app_profile_title),
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(
                            Icons.AutoMirrored.Rounded.ArrowBack,
                            contentDescription = stringResource(R.string.cd_back),
                        )
                    }
                },
                scrollBehavior = scrollBehavior,
            )
        },
        modifier = Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        contentWindowInsets =
            WindowInsets.safeDrawing
                .only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal),
    ) { innerPadding ->
        LazyColumn(
            modifier = Modifier.fillMaxSize().padding(innerPadding),
            contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 32.dp),
            verticalArrangement = Arrangement.spacedBy(24.dp),
        ) {
            item {
                AppIdentityHeader(app = app, allowed = allowed)
            }

            item {
                Column {
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        SectionTitle(stringResource(R.string.app_authorization_section))
                        AuthorizationCard(
                            allowed = allowed,
                            onToggle = { value ->
                                haptic.performHapticFeedback(HapticFeedbackType.VirtualKey)
                                val newCaps =
                                    if (!value) {
                                        emptySet()
                                    } else if (caps.isEmpty()) {
                                        DEFAULT_CAPS
                                    } else {
                                        caps
                                    }
                                val newDomain = if (!value) "u:r:nksu:s0" else domain
                                val newNs = if (!value) NksuNamespace.INHERITED else ns
                                allowed = value
                                caps = newCaps
                                domain = newDomain
                                ns = newNs
                                save(
                                    allowedValue = value,
                                    capsValue = newCaps,
                                    domainValue = newDomain,
                                    nsValue = newNs,
                                )
                            },
                        )
                    }

                    AnimatedVisibility(
                        visible = allowed,
                        enter =
                            expandVertically(
                                animationSpec = motionScheme.defaultSpatialSpec(),
                                expandFrom = Alignment.Top,
                            ) + fadeIn(animationSpec = motionScheme.defaultEffectsSpec()),
                        exit =
                            shrinkVertically(
                                animationSpec = motionScheme.fastSpatialSpec(),
                                shrinkTowards = Alignment.Top,
                            ) + fadeOut(animationSpec = motionScheme.fastEffectsSpec()),
                    ) {
                        Column(
                            modifier = Modifier.padding(top = 24.dp),
                            verticalArrangement = Arrangement.spacedBy(8.dp),
                        ) {
                            SectionTitle(stringResource(R.string.app_root_configuration))
                            CardGroup {
                                item {
                                    ConfigurationItem(
                                        title = stringResource(R.string.app_root_uid),
                                        summary = rootUidText,
                                        icon = Icons.Rounded.Badge,
                                        onClick = { showUidDialog = true },
                                    )
                                }
                                item {
                                    ConfigurationItem(
                                        title = stringResource(R.string.app_capabilities),
                                        summary =
                                            stringResource(
                                                R.string.app_caps_selected,
                                                caps.size,
                                                LinuxCap.entries.size,
                                            ),
                                        icon = Icons.Rounded.Tune,
                                        onClick = { showCapsSheet = true },
                                    )
                                }
                                item {
                                    ConfigurationItem(
                                        title = stringResource(R.string.app_selinux_domain),
                                        summary = domain,
                                        icon = Icons.Rounded.Shield,
                                        onClick = { showDomainDialog = true },
                                    )
                                }
                                item {
                                    ConfigurationItem(
                                        title = stringResource(R.string.app_mount_namespace),
                                        summary = stringResource(ns.labelRes),
                                        icon = Icons.Rounded.Layers,
                                        onClick = { showNsDialog = true },
                                    )
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun AppIdentityHeader(
    app: AppInfo,
    allowed: Boolean,
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Surface(
            modifier = Modifier.size(72.dp),
            shape = MaterialTheme.shapes.large,
            color = MaterialTheme.colorScheme.surfaceContainerHigh,
        ) {
            AppIcon(
                packageName = app.packageName,
                modifier = Modifier.fillMaxSize().padding(4.dp),
            )
        }

        Spacer(Modifier.width(16.dp))

        Column(
            modifier = Modifier.weight(1f),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            Text(
                text = app.name,
                style = MaterialTheme.typography.headlineSmall,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                text = app.packageName,
                style =
                    MaterialTheme.typography.bodySmall.copy(
                        fontFamily = FontFamily.Monospace,
                    ),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Row(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = stringResource(R.string.app_uid_tag, app.uid),
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                if (app.isSystem) {
                    ModeLabel(
                        label = stringResource(R.string.app_tag_system),
                        color = MaterialTheme.colorScheme.onTertiaryContainer,
                        containerColor = MaterialTheme.colorScheme.tertiaryContainer,
                    )
                }
            }
        }
    }
}

@Composable
private fun AuthorizationCard(
    allowed: Boolean,
    onToggle: (Boolean) -> Unit,
) {
    val motionScheme = MaterialTheme.motionScheme

    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        colors =
            CardDefaults.cardColors(
                containerColor = MaterialTheme.colorScheme.surfaceBright,
                contentColor = MaterialTheme.colorScheme.onSurface,
            ),
    ) {
        Row(
            modifier =
                Modifier
                    .fillMaxWidth()
                    .toggleable(
                        value = allowed,
                        role = Role.Switch,
                        onValueChange = onToggle,
                    ).padding(20.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Surface(
                modifier = Modifier.size(48.dp),
                shape = MaterialTheme.shapes.medium,
                color =
                    MaterialTheme.colorScheme.surfaceContainerHigh,
                contentColor =
                    MaterialTheme.colorScheme.onSurfaceVariant,
            ) {
                Box(contentAlignment = Alignment.Center) {
                    AnimatedContent(
                        targetState = allowed,
                        transitionSpec = {
                            (
                                fadeIn(animationSpec = motionScheme.defaultEffectsSpec()) +
                                    scaleIn(
                                        initialScale = 0.7f,
                                        animationSpec = motionScheme.defaultSpatialSpec(),
                                    )
                            ).togetherWith(
                                fadeOut(animationSpec = motionScheme.fastEffectsSpec()) +
                                    scaleOut(
                                        targetScale = 0.7f,
                                        animationSpec = motionScheme.fastSpatialSpec(),
                                    ),
                            )
                        },
                        label = "authorization_lock",
                    ) { granted ->
                        Icon(
                            imageVector = if (granted) Icons.Rounded.LockOpen else Icons.Rounded.Lock,
                            contentDescription = null,
                        )
                    }
                }
            }

            Spacer(Modifier.width(16.dp))

            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                Text(
                    text = stringResource(R.string.app_allow_root),
                    style = MaterialTheme.typography.titleMedium,
                )
                Text(
                    text = stringResource(R.string.app_root_access_summary),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }

            Spacer(Modifier.width(12.dp))

            Switch(checked = allowed, onCheckedChange = null)
        }
    }
}

@Composable
private fun ConfigurationItem(
    title: String,
    summary: String,
    icon: ImageVector,
    onClick: () -> Unit,
) {
    AppListItem(
        title = title,
        modifier = Modifier.fillMaxWidth(),
        summary = summary,
        leading = {
            Icon(
                imageVector = icon,
                contentDescription = null,
            )
        },
        trailing = {
            Icon(
                imageVector = Icons.AutoMirrored.Rounded.KeyboardArrowRight,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        },
        onClick = onClick,
    )
}

@Composable
private fun SectionTitle(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(start = 16.dp),
    )
}

@Preview(showBackground = true)
@Composable
private fun AppDetailScreenPreview() {
    MaterialTheme {
        AppDetailScreen(
            app =
                AppInfo(
                    name = "Sample App",
                    packageName = "com.example.sample",
                    uid = 10123,
                    isSystem = false,
                    isLaunchable = true,
                ),
            config = AppConfig(allowed = true),
            onSave = {},
            onBack = {},
        )
    }
}
