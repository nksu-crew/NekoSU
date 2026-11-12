// SPDX-License-Identifier: GPL-3.0-or-later
package me.nekosu.aqnya.ui.screens

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.Crossfade
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material.icons.automirrored.rounded.KeyboardArrowRight
import androidx.compose.material.icons.rounded.Badge
import androidx.compose.material.icons.rounded.Close
import androidx.compose.material.icons.rounded.Layers
import androidx.compose.material.icons.rounded.Lock
import androidx.compose.material.icons.rounded.LockOpen
import androidx.compose.material.icons.rounded.Search
import androidx.compose.material.icons.rounded.SearchOff
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
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import me.nekosu.aqnya.R
import me.nekosu.aqnya.ui.component.AppDialog
import me.nekosu.aqnya.ui.component.AppTopAppBar
import me.nekosu.aqnya.ui.component.CardGroup
import me.nekosu.aqnya.ui.component.ExpressiveTextField
import me.nekosu.aqnya.ui.component.ListRow
import me.nekosu.aqnya.ui.component.ModeLabel

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun CapsSheet(
    current: Set<LinuxCap>,
    onDismiss: () -> Unit,
    onSelectionChange: (Set<LinuxCap>) -> Unit,
) {
    val haptic = LocalHapticFeedback.current
    val sheetState =
        rememberBottomSheetState(
            initialValue = SheetValue.Hidden,
            enabledValues = setOf(SheetValue.Hidden, SheetValue.Expanded),
        )
    var query by rememberSaveable { mutableStateOf("") }
    val selectionProgress by
        animateFloatAsState(
            targetValue = current.size.toFloat() / LinuxCap.entries.size,
            animationSpec = MaterialTheme.motionScheme.defaultEffectsSpec(),
            label = "capabilitySelectionProgress",
        )

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
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(
                        modifier = Modifier.weight(1f),
                        verticalArrangement = Arrangement.spacedBy(2.dp),
                    ) {
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
                LinearProgressIndicator(
                    progress = { selectionProgress },
                    modifier = Modifier.fillMaxWidth(),
                    color = MaterialTheme.colorScheme.primary,
                    trackColor = MaterialTheme.colorScheme.surfaceContainerHighest,
                )
            }

            ExpressiveTextField(
                value = query,
                onValueChange = { query = it },
                placeholder = stringResource(R.string.caps_search_hint),
                shape = MaterialTheme.shapes.small,
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
                        Column(
                            modifier = Modifier.fillMaxWidth().padding(vertical = 32.dp),
                            horizontalAlignment = Alignment.CenterHorizontally,
                            verticalArrangement = Arrangement.spacedBy(8.dp),
                        ) {
                            Icon(
                                imageVector = Icons.Rounded.SearchOff,
                                contentDescription = null,
                                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                                modifier = Modifier.size(32.dp),
                            )
                            Text(
                                text = stringResource(R.string.caps_empty),
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                textAlign = TextAlign.Center,
                            )
                        }
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
            shape = MaterialTheme.shapes.small,
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
            shape = MaterialTheme.shapes.small,
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
    onSave: (AppConfig?) -> Unit,
    onBack: () -> Unit,
) {
    var allowed by rememberSaveable { mutableStateOf(config != null) }

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
    ) = onSave(
        if (allowedValue) {
            AppConfig(
                rootUid = rootUidValue.toIntOrNull() ?: 0,
                caps = capsValue,
                selinuxDomain = domainValue,
                namespace = nsValue,
            )
        } else {
            null
        },
    )

    val onToggleAuthorization: (Boolean) -> Unit = { value ->
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
    }

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
            WindowInsets.safeDrawing.only(
                WindowInsetsSides.Top + WindowInsetsSides.Horizontal,
            ),
    ) { innerPadding ->
        Box(
            modifier = Modifier.fillMaxSize().padding(innerPadding),
            contentAlignment = Alignment.TopCenter,
        ) {
            Column(
                modifier =
                    Modifier
                        .widthIn(max = 840.dp)
                        .fillMaxSize()
                        .verticalScroll(rememberScrollState())
                        .padding(start = 16.dp, end = 16.dp, top = 16.dp, bottom = 32.dp),
            ) {
                AppHeaderCard(app = app)
                Spacer(Modifier.height(24.dp))
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    SectionHeader(stringResource(R.string.app_authorization_section))
                    AuthorizationCard(
                        allowed = allowed,
                        onToggle = onToggleAuthorization,
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
                    Column {
                        Spacer(Modifier.height(24.dp))
                        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            SectionHeader(stringResource(R.string.app_root_configuration))
                            ConfigurationGroup(
                                rootUidText = rootUidText,
                                capsSelected = caps.size,
                                domain = domain,
                                namespace = ns,
                                onUidClick = { showUidDialog = true },
                                onCapsClick = { showCapsSheet = true },
                                onDomainClick = { showDomainDialog = true },
                                onNamespaceClick = { showNsDialog = true },
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun AppHeaderCard(app: AppInfo) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.extraLarge,
        colors =
            CardDefaults.cardColors(
                containerColor = MaterialTheme.colorScheme.surfaceBright,
            ),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(16.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            AppIcon(
                packageName = app.packageName,
                modifier = Modifier.size(56.dp),
            )
            Spacer(Modifier.width(16.dp))
            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(2.dp),
            ) {
                Text(
                    text = app.name,
                    style = MaterialTheme.typography.titleLarge,
                    fontWeight = FontWeight.Medium,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    text = app.packageName,
                    style =
                        MaterialTheme.typography.bodyMedium.copy(
                            fontFamily = FontFamily.Monospace,
                        ),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Spacer(Modifier.height(4.dp))
                Row(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    ModeLabel(
                        label = stringResource(R.string.app_uid_tag, app.uid),
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        containerColor = MaterialTheme.colorScheme.surfaceContainerHighest,
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
}

@Composable
private fun SectionHeader(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}

@Composable
private fun AuthorizationCard(
    allowed: Boolean,
    onToggle: (Boolean) -> Unit,
) {
    val motionScheme = MaterialTheme.motionScheme
    val containerColor by
        animateColorAsState(
            targetValue =
                if (allowed) {
                    MaterialTheme.colorScheme.secondaryContainer
                } else {
                    MaterialTheme.colorScheme.surfaceBright
                },
            animationSpec = motionScheme.defaultEffectsSpec(),
            label = "authorizationContainerColor",
        )
    val contentColor by
        animateColorAsState(
            targetValue =
                if (allowed) {
                    MaterialTheme.colorScheme.onSecondaryContainer
                } else {
                    MaterialTheme.colorScheme.onSurface
                },
            animationSpec = motionScheme.defaultEffectsSpec(),
            label = "authorizationContentColor",
        )
    val supportingColor by
        animateColorAsState(
            targetValue =
                if (allowed) {
                    MaterialTheme.colorScheme.onSecondaryContainer
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant
                },
            animationSpec = motionScheme.defaultEffectsSpec(),
            label = "authorizationSupportingColor",
        )
    val iconContainerColor by
        animateColorAsState(
            targetValue =
                if (allowed) {
                    MaterialTheme.colorScheme.primary
                } else {
                    MaterialTheme.colorScheme.surfaceContainerHighest
                },
            animationSpec = motionScheme.defaultEffectsSpec(),
            label = "authorizationIconContainerColor",
        )
    val iconContentColor by
        animateColorAsState(
            targetValue =
                if (allowed) {
                    MaterialTheme.colorScheme.onPrimary
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant
                },
            animationSpec = motionScheme.defaultEffectsSpec(),
            label = "authorizationIconContentColor",
        )

    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.extraLarge,
        colors =
            CardDefaults.cardColors(
                containerColor = containerColor,
                contentColor = contentColor,
            ),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
    ) {
        Row(
            modifier =
                Modifier
                    .fillMaxWidth()
                    .toggleable(
                        value = allowed,
                        role = Role.Switch,
                        onValueChange = onToggle,
                    ).padding(horizontal = 16.dp, vertical = 20.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Surface(
                modifier = Modifier.size(44.dp),
                shape = MaterialTheme.shapes.large,
                color = iconContainerColor,
                contentColor = iconContentColor,
            ) {
                Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Crossfade(
                        targetState = allowed,
                        animationSpec = motionScheme.defaultEffectsSpec(),
                        label = "authorizationLockIcon",
                    ) { isAllowed ->
                        Icon(
                            imageVector =
                                if (isAllowed) {
                                    Icons.Rounded.LockOpen
                                } else {
                                    Icons.Rounded.Lock
                                },
                            contentDescription = null,
                            modifier = Modifier.size(22.dp),
                        )
                    }
                }
            }
            Spacer(Modifier.width(16.dp))
            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(2.dp),
            ) {
                Text(
                    text = stringResource(R.string.app_allow_root),
                    style = MaterialTheme.typography.titleMedium,
                )
                Text(
                    text = stringResource(R.string.app_root_access_summary),
                    style = MaterialTheme.typography.bodyMedium,
                    color = supportingColor,
                )
            }
            Spacer(Modifier.width(12.dp))
            Switch(
                checked = allowed,
                onCheckedChange = null,
                modifier = Modifier.clearAndSetSemantics { },
            )
        }
    }
}

@Composable
private fun ConfigurationGroup(
    rootUidText: String,
    capsSelected: Int,
    domain: String,
    namespace: NksuNamespace,
    onUidClick: () -> Unit,
    onCapsClick: () -> Unit,
    onDomainClick: () -> Unit,
    onNamespaceClick: () -> Unit,
) {
    CardGroup {
        item {
            ConfigurationRow(
                title = stringResource(R.string.app_root_uid),
                summary = rootUidText,
                icon = Icons.Rounded.Badge,
                monospace = true,
                onClick = onUidClick,
            )
        }
        item {
            ConfigurationRow(
                title = stringResource(R.string.app_capabilities),
                summary =
                    stringResource(
                        R.string.app_caps_selected,
                        capsSelected,
                        LinuxCap.entries.size,
                    ),
                icon = Icons.Rounded.Tune,
                onClick = onCapsClick,
            )
        }
        item {
            ConfigurationRow(
                title = stringResource(R.string.app_selinux_domain),
                summary = domain,
                icon = Icons.Rounded.Shield,
                monospace = true,
                onClick = onDomainClick,
            )
        }
        item {
            ConfigurationRow(
                title = stringResource(R.string.app_mount_namespace),
                summary = stringResource(namespace.labelRes),
                icon = Icons.Rounded.Layers,
                onClick = onNamespaceClick,
            )
        }
    }
}

@Composable
private fun ConfigurationRow(
    title: String,
    summary: String,
    icon: ImageVector,
    onClick: () -> Unit,
    monospace: Boolean = false,
) {
    ListRow(
        icon = {
            IconBadge(
                icon = icon,
                containerColor = MaterialTheme.colorScheme.secondaryContainer,
                contentColor = MaterialTheme.colorScheme.onSecondaryContainer,
            )
        },
        headline = {
            Text(
                text = title,
                style = MaterialTheme.typography.titleMedium,
            )
        },
        supporting = {
            Text(
                text = summary,
                style = MaterialTheme.typography.bodyMedium,
                fontFamily = if (monospace) FontFamily.Monospace else FontFamily.Default,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
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
private fun IconBadge(
    icon: ImageVector,
    containerColor: Color,
    contentColor: Color,
    modifier: Modifier = Modifier.size(42.dp),
    iconSize: Dp = 21.dp,
) {
    Surface(
        modifier = modifier,
        shape = MaterialTheme.shapes.large,
        color = containerColor,
        contentColor = contentColor,
    ) {
        Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Icon(
                imageVector = icon,
                contentDescription = null,
                modifier = Modifier.size(iconSize),
            )
        }
    }
}

@Preview(showBackground = true, name = "Authorization enabled")
@Composable
private fun AppDetailScreenEnabledPreview() {
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
            config = AppConfig(),
            onSave = {},
            onBack = {},
        )
    }
}

@Preview(showBackground = true, name = "Authorization disabled")
@Composable
private fun AppDetailScreenDisabledPreview() {
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
            config = null,
            onSave = {},
            onBack = {},
        )
    }
}
