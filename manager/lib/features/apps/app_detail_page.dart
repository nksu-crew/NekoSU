import 'package:material_ui/material_ui.dart';
import 'package:flutter/services.dart';
import 'package:nekosu/core/ncore_ffi.dart';
import 'package:nekosu/data/apps.dart';
import 'package:nekosu/l10n/app_localizations.dart';
import 'package:nekosu/shared/app_feedback.dart';
import 'package:nekosu/shared/widgets/app_icon.dart';
import 'package:nekosu/shared/widgets/app_popup_menu.dart';
import 'package:nekosu/shared/widgets/card_group.dart';
import 'package:nekosu/shared/widgets/section_card.dart';

enum _ProfileMode { defaults, custom }

enum _ProfileMenuAction { copyPackageName, resetProfile }

class AppDetailPage extends StatefulWidget {
  const AppDetailPage({super.key, required this.app, this.config});

  final AppInfo app;
  final AppConfig? config;

  @override
  State<AppDetailPage> createState() => _AppDetailPageState();
}

class _AppDetailPageState extends State<AppDetailPage> {
  late bool _allowed;
  late int _rootUid;
  late Set<int> _caps;
  late String _domain;
  late NksuNamespace _namespace;
  late _ProfileMode _mode;
  bool _saveErrorShown = false;

  @override
  void initState() {
    super.initState();
    final config = widget.config;
    _allowed = config?.allowed ?? false;
    _rootUid = config?.rootUid ?? 0;
    _caps = config?.caps.toSet() ?? {...defaultCaps};
    _domain = config?.selinuxDomain ?? defaultSelinuxDomain;
    _namespace = config?.namespace ?? NksuNamespace.inherited;
    _mode = _isDefaultProfile ? _ProfileMode.defaults : _ProfileMode.custom;
  }

  bool get _isDefaultProfile =>
      _rootUid == 0 &&
      _caps.length == defaultCaps.length &&
      _caps.containsAll(defaultCaps) &&
      _domain == defaultSelinuxDomain &&
      _namespace == NksuNamespace.inherited;

  void _persist() {
    final success = _allowed
        ? NcoreFfi.setProfile(
            uid: widget.app.uid,
            rootUid: _rootUid,
            caps: capsToBits(_caps),
            domain: _domain,
            namespace: _namespace.value,
          )
        : NcoreFfi.deleteProfile(widget.app.uid);

    if (!mounted) return;
    if (success) {
      _saveErrorShown = false;
      return;
    }
    if (_saveErrorShown) return;
    _saveErrorShown = true;
    final l10n = AppLocalizations.of(context)!;
    showAppSnackBar(context, l10n.profileSaveFailed);
  }

  void _toggleAllowed(bool value) {
    setState(() {
      _allowed = value;
      if (value) {
        _ensureProfileDefaults();
      } else {
        _applyDefaultProfile();
      }
    });
    _persist();
  }

  void _ensureProfileDefaults() {
    if (_caps.isEmpty) _caps = {...defaultCaps};
    if (_domain.trim().isEmpty) _domain = defaultSelinuxDomain;
  }

  void _applyDefaultProfile() {
    _rootUid = 0;
    _caps = {...defaultCaps};
    _domain = defaultSelinuxDomain;
    _namespace = NksuNamespace.inherited;
    _mode = _ProfileMode.defaults;
  }

  void _selectProfileMode(_ProfileMode mode) {
    if (_mode == mode) return;
    setState(() {
      _mode = mode;
      if (mode == _ProfileMode.defaults) {
        _applyDefaultProfile();
      } else {
        _allowed = true;
        _ensureProfileDefaults();
      }
    });
    _persist();
  }

  Future<void> _copyPackageName() async {
    await Clipboard.setData(ClipboardData(text: widget.app.packageName));
    if (!mounted) return;
    final l10n = AppLocalizations.of(context)!;
    showAppSnackBar(context, l10n.packageNameCopied);
  }

  Future<void> _confirmResetProfile() async {
    final l10n = AppLocalizations.of(context)!;
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(l10n.resetProfileConfirmTitle),
        content: Text(l10n.resetProfileConfirmMessage),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: Text(l10n.dialogCancel),
          ),
          FilledButton(
            onPressed: () => Navigator.of(context).pop(true),
            child: Text(l10n.resetProfile),
          ),
        ],
      ),
    );
    if (confirmed != true || !mounted) return;
    setState(() {
      _allowed = false;
      _applyDefaultProfile();
    });
    _persist();
  }

  void _onProfileMenuSelected(_ProfileMenuAction action) {
    switch (action) {
      case _ProfileMenuAction.copyPackageName:
        _copyPackageName();
        break;
      case _ProfileMenuAction.resetProfile:
        _confirmResetProfile();
        break;
    }
  }

  Future<void> _editRootUid() async {
    final l10n = AppLocalizations.of(context)!;
    final value = await showDialog<int>(
      context: context,
      builder: (context) => _ConfigTextDialog<int>(
        title: l10n.rootUid,
        initialValue: '$_rootUid',
        labelText: l10n.rootUidInput,
        helperText: l10n.rootUidDescription,
        keyboardType: TextInputType.number,
        inputFormatters: [
          FilteringTextInputFormatter.digitsOnly,
          LengthLimitingTextInputFormatter(10),
        ],
        submit: (value) => int.tryParse(value.trim()) ?? 0,
      ),
    );
    if (value == null || !mounted) return;
    setState(() => _rootUid = value);
    _persist();
  }

  Future<void> _editDomain() async {
    final l10n = AppLocalizations.of(context)!;
    final value = await showDialog<String>(
      context: context,
      builder: (context) => _ConfigTextDialog<String>(
        title: l10n.selinuxDomain,
        initialValue: _domain,
        labelText: l10n.selinuxDomainInput,
        helperText: l10n.selinuxDomainDescription,
        hintText: defaultSelinuxDomain,
        submit: (value) => value,
      ),
    );
    if (value == null || !mounted) return;
    final domain = value.trim().isEmpty ? defaultSelinuxDomain : value.trim();
    setState(() => _domain = domain);
    _persist();
  }

  Future<void> _editNamespace() async {
    final l10n = AppLocalizations.of(context)!;
    final value = await showModalBottomSheet<NksuNamespace>(
      context: context,
      showDragHandle: true,
      useSafeArea: true,
      builder: (context) {
        final scheme = Theme.of(context).colorScheme;
        return SingleChildScrollView(
          padding: const EdgeInsets.fromLTRB(16, 0, 16, 24),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Padding(
                padding: const EdgeInsets.fromLTRB(8, 0, 8, 12),
                child: Text(
                  l10n.mountNamespace,
                  style: Theme.of(context).textTheme.headlineSmall,
                ),
              ),
              for (final namespace in NksuNamespace.values)
                ListTile(
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(16),
                  ),
                  leading: Icon(
                    namespace == _namespace
                        ? Icons.check_circle_rounded
                        : Icons.circle_outlined,
                    color: namespace == _namespace
                        ? scheme.primary
                        : scheme.onSurfaceVariant,
                  ),
                  title: Text(_namespaceLabel(l10n, namespace)),
                  subtitle: Text(_namespaceDescription(l10n, namespace)),
                  onTap: () => Navigator.of(context).pop(namespace),
                ),
            ],
          ),
        );
      },
    );
    if (value == null || !mounted) return;
    setState(() => _namespace = value);
    _persist();
  }

  Future<void> _editCaps() async {
    final value = await showModalBottomSheet<Set<int>>(
      context: context,
      isScrollControlled: true,
      useSafeArea: true,
      showDragHandle: true,
      builder: (context) => _CapsSheet(initial: _caps),
    );
    if (value == null || !mounted) return;
    setState(() => _caps = value);
    _persist();
  }

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final textTheme = Theme.of(context).textTheme;
    final l10n = AppLocalizations.of(context)!;
    final bottomInset = MediaQuery.viewPaddingOf(context).bottom;

    return Scaffold(
      backgroundColor: scheme.surfaceContainer,
      body: SafeArea(
        bottom: false,
        child: ListView(
          padding: EdgeInsets.only(top: 12, bottom: 32 + bottomInset),
          children: [
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16),
              child: _ProfileToolbar(onMenuSelected: _onProfileMenuSelected),
            ),
            const SizedBox(height: 18),
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16),
              child: Text(
                l10n.appProfileTitle,
                style: textTheme.headlineLarge?.copyWith(
                  color: scheme.onSurface,
                ),
              ),
            ),
            const SizedBox(height: 18),
            CardGroup(
              children: [
                _AppIdentityCard(app: widget.app),
                _AuthorizationCard(
                  allowed: _allowed,
                  onChanged: _toggleAllowed,
                ),
                _ProfileSummaryCard(mode: _mode),
              ],
            ),
            const SizedBox(height: 12),
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16),
              child: _ProfileModeSelector(
                mode: _mode,
                onSelected: _selectProfileMode,
              ),
            ),
            AnimatedSize(
              duration: const Duration(milliseconds: 260),
              curve: Curves.easeOutCubic,
              alignment: Alignment.topCenter,
              child: _mode == _ProfileMode.custom
                  ? Padding(
                      padding: const EdgeInsets.only(top: 18),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.stretch,
                        children: [
                          SectionLabel(
                            text: l10n.rootConfiguration,
                            padding: const EdgeInsets.only(left: 16),
                          ),
                          const SizedBox(height: 8),
                          CardGroup(
                            children: [
                              _ConfigRow(
                                icon: Icons.badge_outlined,
                                title: l10n.rootUid,
                                summary: '$_rootUid',
                                onTap: _editRootUid,
                              ),
                              _ConfigRow(
                                icon: Icons.tune_rounded,
                                title: l10n.capabilities,
                                summary: l10n.capabilitiesSelected(
                                  _caps.length,
                                  linuxCaps.length,
                                ),
                                onTap: _editCaps,
                              ),
                              _ConfigRow(
                                icon: Icons.shield_outlined,
                                title: l10n.selinuxDomain,
                                summary: _domain,
                                onTap: _editDomain,
                              ),
                              _ConfigRow(
                                icon: Icons.layers_outlined,
                                title: l10n.mountNamespace,
                                summary: _namespaceLabel(l10n, _namespace),
                                onTap: _editNamespace,
                              ),
                            ],
                          ),
                        ],
                      ),
                    )
                  : const SizedBox.shrink(),
            ),
          ],
        ),
      ),
    );
  }
}

class _ProfileToolbar extends StatelessWidget {
  const _ProfileToolbar({required this.onMenuSelected});

  final ValueChanged<_ProfileMenuAction> onMenuSelected;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final l10n = AppLocalizations.of(context)!;
    final localizations = MaterialLocalizations.of(context);

    return Row(
      children: [
        Material(
          color: scheme.surfaceContainerHigh,
          shape: const CircleBorder(),
          clipBehavior: Clip.antiAlias,
          child: SizedBox.square(
            dimension: 40,
            child: IconButton(
              onPressed: () => Navigator.of(context).maybePop(),
              icon: const Icon(Icons.arrow_back_rounded),
              iconSize: 22,
              tooltip: localizations.backButtonTooltip,
            ),
          ),
        ),
        const Spacer(),
        AppPopupMenu<_ProfileMenuAction>(
          icon: Icons.more_vert_rounded,
          tooltip: localizations.showMenuTooltip,
          onSelected: onMenuSelected,
          items: [
            AppPopupMenuItem(
              value: _ProfileMenuAction.copyPackageName,
              label: l10n.copyPackageName,
              icon: Icons.content_copy_rounded,
            ),
            AppPopupMenuItem(
              value: _ProfileMenuAction.resetProfile,
              label: l10n.resetProfile,
              icon: Icons.restart_alt_rounded,
              destructive: true,
              dividerBefore: true,
            ),
          ],
        ),
      ],
    );
  }
}

class _AppIdentityCard extends StatelessWidget {
  const _AppIdentityCard({required this.app});

  final AppInfo app;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final textTheme = Theme.of(context).textTheme;
    final l10n = AppLocalizations.of(context)!;
    final version = _appVersionLabel(app);

    return ListRow(
      icon: AppIcon(packageName: app.packageName, size: 40),
      headline: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            app.name,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: textTheme.titleMedium,
          ),
          if (version.isNotEmpty) ...[
            const SizedBox(height: 1),
            Text(
              version,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: textTheme.bodySmall?.copyWith(
                color: scheme.onSurfaceVariant,
              ),
            ),
          ],
          const SizedBox(height: 1),
          Text(
            app.packageName,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: textTheme.bodySmall?.copyWith(
              color: scheme.onSurfaceVariant,
            ),
          ),
        ],
      ),
      trailing: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.end,
        children: [
          _ModeLabel(
            label: l10n.uidLabel(app.uid),
            foreground: scheme.onSecondaryContainer,
            background: scheme.secondaryContainer,
          ),
          if (app.isSystem) ...[
            const SizedBox(height: 6),
            _ModeLabel(
              label: l10n.tagSystem,
              foreground: scheme.onTertiaryContainer,
              background: scheme.tertiaryContainer,
            ),
          ],
        ],
      ),
    );
  }
}

class _AuthorizationCard extends StatelessWidget {
  const _AuthorizationCard({required this.allowed, required this.onChanged});

  final bool allowed;
  final ValueChanged<bool> onChanged;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final textTheme = Theme.of(context).textTheme;
    final l10n = AppLocalizations.of(context)!;

    return ListRow(
      icon: Icon(
        Icons.shield_rounded,
        size: 28,
        color: allowed ? scheme.primary : scheme.onSurface,
      ),
      headline: Text(l10n.superuser, style: textTheme.titleMedium),
      trailing: _CapsuleSwitch(
        value: allowed,
        label: l10n.superuser,
        onChanged: onChanged,
      ),
      onTap: () => onChanged(!allowed),
    );
  }
}

class _ProfileSummaryCard extends StatelessWidget {
  const _ProfileSummaryCard({required this.mode});

  final _ProfileMode mode;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final textTheme = Theme.of(context).textTheme;
    final l10n = AppLocalizations.of(context)!;
    final modeLabel = switch (mode) {
      _ProfileMode.defaults => l10n.profileModeDefault,
      _ProfileMode.custom => l10n.profileModeCustom,
    };

    return ListRow(
      icon: Icon(Icons.person_rounded, size: 28, color: scheme.onSurface),
      headline: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(l10n.appProfileTitle, style: textTheme.titleMedium),
          const SizedBox(height: 1),
          Text(
            modeLabel,
            style: textTheme.bodyMedium?.copyWith(
              color: scheme.onSurfaceVariant,
            ),
          ),
        ],
      ),
    );
  }
}

class _ProfileModeSelector extends StatelessWidget {
  const _ProfileModeSelector({required this.mode, required this.onSelected});

  final _ProfileMode mode;
  final ValueChanged<_ProfileMode> onSelected;

  @override
  Widget build(BuildContext context) {
    final l10n = AppLocalizations.of(context)!;
    return Row(
      children: [
        Expanded(
          child: _ProfileModeSegment(
            label: l10n.profileModeDefault,
            selected: mode == _ProfileMode.defaults,
            onTap: () => onSelected(_ProfileMode.defaults),
          ),
        ),
        const SizedBox(width: 4),
        Expanded(
          child: _ProfileModeSegment(
            label: l10n.profileModeCustom,
            selected: mode == _ProfileMode.custom,
            onTap: () => onSelected(_ProfileMode.custom),
          ),
        ),
      ],
    );
  }
}

class _ProfileModeSegment extends StatelessWidget {
  const _ProfileModeSegment({
    required this.label,
    required this.selected,
    this.enabled = true,
    this.onTap,
  });

  final String label;
  final bool selected;
  final bool enabled;
  final VoidCallback? onTap;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    final foreground = selected
        ? scheme.onPrimary
        : enabled
        ? scheme.onSurface
        : scheme.onSurface.withValues(alpha: 0.28);
    final background = selected ? scheme.primary : scheme.surfaceContainerHigh;
    final shape = BorderRadius.circular(999);

    return Semantics(
      button: true,
      enabled: enabled,
      selected: selected,
      child: AnimatedContainer(
        duration: const Duration(milliseconds: 180),
        curve: Curves.easeOutCubic,
        height: 36,
        clipBehavior: Clip.antiAlias,
        decoration: BoxDecoration(color: background, borderRadius: shape),
        child: Material(
          type: MaterialType.transparency,
          child: InkWell(
            onTap: enabled ? onTap : null,
            child: Center(
              child: Text(
                label,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: theme.textTheme.labelLarge?.copyWith(color: foreground),
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _CapsuleSwitch extends StatelessWidget {
  const _CapsuleSwitch({
    required this.value,
    required this.label,
    this.onChanged,
    this.enabled = true,
  });

  final bool value;
  final String label;
  final ValueChanged<bool>? onChanged;
  final bool enabled;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final canToggle = enabled && onChanged != null;
    final onTap = canToggle ? () => onChanged!(!value) : null;
    final trackColor = !enabled
        ? scheme.surfaceContainerHighest
        : value
        ? scheme.primary
        : scheme.surfaceContainerHigh;
    final borderColor = !enabled
        ? scheme.outline.withValues(alpha: 0.18)
        : value
        ? scheme.primary
        : scheme.outline.withValues(alpha: 0.35);
    final thumbColor = !enabled
        ? scheme.surfaceContainerLow
        : value
        ? scheme.onPrimary
        : scheme.surface;
    final iconColor = value
        ? scheme.primary
        : enabled
        ? scheme.onSurfaceVariant
        : scheme.outline.withValues(alpha: 0.6);

    return Semantics(
      button: true,
      enabled: enabled,
      toggled: value,
      label: label,
      onTap: onTap,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: onTap,
        child: SizedBox(
          width: 56,
          height: 30,
          child: AnimatedContainer(
            duration: const Duration(milliseconds: 200),
            curve: Curves.easeOutCubic,
            decoration: BoxDecoration(
              color: trackColor,
              border: Border.all(color: borderColor, width: 1.5),
              borderRadius: BorderRadius.circular(999),
            ),
            child: AnimatedAlign(
              duration: const Duration(milliseconds: 200),
              curve: Curves.easeOutCubic,
              alignment: value ? Alignment.centerRight : Alignment.centerLeft,
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 3),
                child: AnimatedContainer(
                  duration: const Duration(milliseconds: 200),
                  curve: Curves.easeOutCubic,
                  width: 22,
                  height: 22,
                  decoration: BoxDecoration(
                    color: thumbColor,
                    shape: BoxShape.circle,
                  ),
                  child: Icon(
                    value ? Icons.check_rounded : Icons.close_rounded,
                    size: 15,
                    color: iconColor,
                  ),
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _ConfigRow extends StatelessWidget {
  const _ConfigRow({
    required this.icon,
    required this.title,
    required this.summary,
    required this.onTap,
  });

  final IconData icon;
  final String title;
  final String summary;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final textTheme = Theme.of(context).textTheme;

    return ListRow(
      icon: Icon(icon),
      headline: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(title, style: textTheme.bodyLarge),
          const SizedBox(height: 2),
          Text(
            summary,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: textTheme.bodyMedium?.copyWith(
              color: scheme.onSurfaceVariant,
            ),
          ),
        ],
      ),
      trailing: const Icon(Icons.keyboard_arrow_right_rounded),
      onTap: onTap,
    );
  }
}

class _CapsSheet extends StatefulWidget {
  const _CapsSheet({required this.initial});

  final Set<int> initial;

  @override
  State<_CapsSheet> createState() => _CapsSheetState();
}

class _ConfigTextDialog<T> extends StatefulWidget {
  const _ConfigTextDialog({
    required this.title,
    required this.initialValue,
    required this.labelText,
    required this.helperText,
    required this.submit,
    this.hintText,
    this.keyboardType = TextInputType.text,
    this.inputFormatters = const [],
  });

  final String title;
  final String initialValue;
  final String labelText;
  final String helperText;
  final String? hintText;
  final TextInputType keyboardType;
  final List<TextInputFormatter> inputFormatters;
  final T Function(String value) submit;

  @override
  State<_ConfigTextDialog<T>> createState() => _ConfigTextDialogState<T>();
}

class _ConfigTextDialogState<T> extends State<_ConfigTextDialog<T>> {
  late final TextEditingController _controller;

  @override
  void initState() {
    super.initState();
    _controller = TextEditingController(text: widget.initialValue);
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  void _submit() {
    Navigator.of(context).pop(widget.submit(_controller.text));
  }

  @override
  Widget build(BuildContext context) {
    final l10n = AppLocalizations.of(context)!;
    return AlertDialog(
      title: Text(widget.title),
      content: TextField(
        controller: _controller,
        autofocus: true,
        keyboardType: widget.keyboardType,
        inputFormatters: widget.inputFormatters,
        decoration: InputDecoration(
          labelText: widget.labelText,
          helperText: widget.helperText,
          helperMaxLines: 2,
          hintText: widget.hintText,
          border: const OutlineInputBorder(),
        ),
        onSubmitted: (_) => _submit(),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: Text(l10n.dialogCancel),
        ),
        FilledButton(onPressed: _submit, child: Text(l10n.dialogConfirm)),
      ],
    );
  }
}

class _CapsSheetState extends State<_CapsSheet> {
  late Set<int> _selected;
  final _searchController = TextEditingController();
  String _query = '';

  @override
  void initState() {
    super.initState();
    _selected = widget.initial.toSet();
  }

  @override
  void dispose() {
    _searchController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final textTheme = Theme.of(context).textTheme;
    final l10n = AppLocalizations.of(context)!;
    final query = _query.trim().toLowerCase();
    final filtered = linuxCaps.where((cap) {
      if (query.isEmpty) return true;
      final description = _capDescription(l10n, cap.value).toLowerCase();
      return cap.name.toLowerCase().contains(query) ||
          description.contains(query) ||
          cap.value.toString() == query;
    }).toList();

    return FractionallySizedBox(
      heightFactor: 0.9,
      child: Column(
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(24, 0, 12, 8),
            child: Row(
              children: [
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(l10n.capabilities, style: textTheme.headlineSmall),
                      Text(
                        l10n.capabilitiesSelected(
                          _selected.length,
                          linuxCaps.length,
                        ),
                        style: textTheme.bodyMedium?.copyWith(
                          color: scheme.onSurfaceVariant,
                        ),
                      ),
                    ],
                  ),
                ),
                IconButton(
                  onPressed: () => Navigator.of(context).pop(),
                  icon: const Icon(Icons.close_rounded),
                  tooltip: MaterialLocalizations.of(context).closeButtonTooltip,
                ),
              ],
            ),
          ),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 24),
            child: TextField(
              controller: _searchController,
              onChanged: (value) => setState(() => _query = value),
              decoration: InputDecoration(
                hintText: l10n.capsSearchHint,
                prefixIcon: const Icon(Icons.search_rounded),
                suffixIcon: _query.isEmpty
                    ? null
                    : IconButton(
                        onPressed: () {
                          _searchController.clear();
                          setState(() => _query = '');
                        },
                        icon: const Icon(Icons.close_rounded),
                      ),
                filled: true,
                fillColor: scheme.surfaceContainerHigh,
                border: OutlineInputBorder(
                  borderRadius: BorderRadius.circular(20),
                  borderSide: BorderSide.none,
                ),
              ),
            ),
          ),
          Padding(
            padding: const EdgeInsets.fromLTRB(24, 12, 24, 8),
            child: Row(
              children: [
                Expanded(
                  child: FilledButton.tonal(
                    onPressed: () => setState(
                      () =>
                          _selected = linuxCaps.map((cap) => cap.value).toSet(),
                    ),
                    child: Text(l10n.dialogSelectAll),
                  ),
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: FilledButton.tonal(
                    onPressed: () =>
                        setState(() => _selected = {...defaultCaps}),
                    child: Text(l10n.dialogDefault),
                  ),
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: FilledButton.tonal(
                    onPressed: () => setState(() => _selected = <int>{}),
                    child: Text(l10n.dialogClear),
                  ),
                ),
              ],
            ),
          ),
          Expanded(
            child: ListView.builder(
              padding: const EdgeInsets.symmetric(horizontal: 16),
              itemCount: filtered.length,
              itemBuilder: (context, index) {
                final cap = filtered[index];
                final selected = _selected.contains(cap.value);
                return CheckboxListTile(
                  value: selected,
                  onChanged: (value) {
                    setState(() {
                      if (value ?? false) {
                        _selected.add(cap.value);
                      } else {
                        _selected.remove(cap.value);
                      }
                    });
                  },
                  controlAffinity: ListTileControlAffinity.leading,
                  title: Text('CAP_${cap.name}', style: textTheme.bodyMedium),
                  subtitle: Text(
                    _capDescription(l10n, cap.value),
                    maxLines: 2,
                    overflow: TextOverflow.ellipsis,
                    style: textTheme.labelSmall,
                  ),
                  secondary: Text(
                    '${cap.value}',
                    style: textTheme.labelSmall?.copyWith(
                      color: scheme.onSurfaceVariant,
                    ),
                  ),
                );
              },
            ),
          ),
          Padding(
            padding: const EdgeInsets.fromLTRB(24, 12, 24, 20),
            child: SizedBox(
              width: double.infinity,
              child: FilledButton(
                onPressed: () =>
                    Navigator.of(context).pop(Set<int>.from(_selected)),
                child: Text(l10n.dialogDone),
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _ModeLabel extends StatelessWidget {
  const _ModeLabel({
    required this.label,
    required this.foreground,
    required this.background,
  });

  final String label;
  final Color foreground;
  final Color background;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: background,
        borderRadius: BorderRadius.circular(8),
      ),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
        child: Text(
          label,
          style: Theme.of(context).textTheme.labelSmall
              ?.copyWith(color: foreground),
        ),
      ),
    );
  }
}

String _namespaceLabel(AppLocalizations l10n, NksuNamespace namespace) {
  return switch (namespace) {
    NksuNamespace.inherited => l10n.namespaceInheritedLabel,
    NksuNamespace.individual => l10n.namespaceIndividualLabel,
    NksuNamespace.global => l10n.namespaceGlobalLabel,
  };
}

String _appVersionLabel(AppInfo app) {
  if (app.versionName.isEmpty) {
    return app.versionCode > 0 ? '${app.versionCode}' : '';
  }
  return app.versionCode > 0
      ? '${app.versionName} (${app.versionCode})'
      : app.versionName;
}

String _namespaceDescription(AppLocalizations l10n, NksuNamespace namespace) {
  return switch (namespace) {
    NksuNamespace.inherited => l10n.namespaceInheritedDescription,
    NksuNamespace.individual => l10n.namespaceIndividualDescription,
    NksuNamespace.global => l10n.namespaceGlobalDescription,
  };
}

String _capDescription(AppLocalizations l10n, int value) {
  final descriptions = [
    l10n.capDescChown,
    l10n.capDescDacOverride,
    l10n.capDescDacReadSearch,
    l10n.capDescFowner,
    l10n.capDescFsetid,
    l10n.capDescKill,
    l10n.capDescSetgid,
    l10n.capDescSetuid,
    l10n.capDescSetpcap,
    l10n.capDescLinuxImmutable,
    l10n.capDescNetBindService,
    l10n.capDescNetBroadcast,
    l10n.capDescNetAdmin,
    l10n.capDescNetRaw,
    l10n.capDescIpcLock,
    l10n.capDescIpcOwner,
    l10n.capDescSysModule,
    l10n.capDescSysRawio,
    l10n.capDescSysChroot,
    l10n.capDescSysPtrace,
    l10n.capDescSysPacct,
    l10n.capDescSysAdmin,
    l10n.capDescSysBoot,
    l10n.capDescSysNice,
    l10n.capDescSysResource,
    l10n.capDescSysTime,
    l10n.capDescSysTtyConfig,
    l10n.capDescMknod,
    l10n.capDescLease,
    l10n.capDescAuditWrite,
    l10n.capDescAuditControl,
    l10n.capDescSetfcap,
    l10n.capDescMacOverride,
    l10n.capDescMacAdmin,
    l10n.capDescSyslog,
    l10n.capDescWakeAlarm,
    l10n.capDescBlockSuspend,
    l10n.capDescAuditRead,
    l10n.capDescPerfmon,
    l10n.capDescBpf,
    l10n.capDescCheckpointRestore,
  ];
  return value >= 0 && value < descriptions.length ? descriptions[value] : '';
}
