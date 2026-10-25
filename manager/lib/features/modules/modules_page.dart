import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_markdown_plus/flutter_markdown_plus.dart';
import 'package:nekosu/data/module_updates.dart';
import 'package:nekosu/data/modules.dart';
import 'package:nekosu/features/modules/webui_page.dart';
import 'package:nekosu/l10n/app_localizations.dart';
import 'package:nekosu/shared/app_feedback.dart';
import 'package:nekosu/shared/layout_insets.dart';
import 'package:nekosu/shared/widgets/app_popup_menu.dart';
import 'package:nekosu/shared/widgets/app_sliver_app_bar.dart';
import 'package:nekosu/shared/widgets/app_status_view.dart';
import 'package:nekosu/shared/widgets/app_tag.dart';
import 'package:nekosu/shared/widgets/output_page.dart';
import 'package:path_provider/path_provider.dart';
import 'package:url_launcher/url_launcher.dart';

class ModulesPage extends StatefulWidget {
  const ModulesPage({super.key});

  @override
  State<ModulesPage> createState() => _ModulesPageState();
}

class _ModulesPageState extends State<ModulesPage> {
  List<ModuleInfo> _modules = const [];
  final Set<String> _busyModules = {};

  Map<String, ModuleUpdateCheck> _updateChecks = const {};
  String? _updateCheckSignature;
  int _updateCheckGeneration = 0;

  bool _loading = true;
  String? _loadError;
  bool _installing = false;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load({bool refreshUpdates = false}) async {
    setState(() {
      _loading = _modules.isEmpty;
      _loadError = null;
    });
    final result = await ModuleBridge.list();
    if (!mounted) return;
    setState(() {
      _modules = result.modules;
      _loading = false;
      _loadError = result.ok ? null : result.output.trim();
    });
    unawaited(_checkUpdates(force: refreshUpdates));
  }

  /// Re-checks `updateJson` endpoints. Checks only run when the set of modules
  /// or their installed versions changed, unless [force] is set (manual
  /// refresh), so toggling a module does not hammer every update server.
  Future<void> _checkUpdates({bool force = false}) async {
    final modules = _modules;
    final signature = modules
        .map(
          (module) => '${module.id}|${module.versionCode}|${module.updateJson}',
        )
        .join('\n');
    if (!force && signature == _updateCheckSignature) return;
    _updateCheckSignature = signature;
    final generation = ++_updateCheckGeneration;

    Map<String, ModuleUpdateCheck> checks;
    try {
      checks = await ModuleUpdateService.checkAll(modules);
    } on Object catch (error) {
      checks = {
        for (final module in modules)
          if (module.updateJson.trim().isNotEmpty)
            module.id: ModuleUpdateCheck.failed('$error'),
      };
    }
    // Update checks are best effort: a module whose update channel is
    // unreachable simply gets no update button, matching the module manager
    // behaviour. Keep the reason in the log for diagnostics.
    for (final entry in checks.entries) {
      final error = entry.value.error;
      if (error != null) {
        debugPrint('Module update check failed for ${entry.key}: $error');
      }
    }
    if (!mounted || generation != _updateCheckGeneration) return;
    setState(() => _updateChecks = checks);
  }

  Future<void> _installModule() async {
    if (_installing) return;
    final l10n = AppLocalizations.of(context)!;
    setState(() => _installing = true);
    try {
      final path = await ModuleBridge.pickZip();
      if (!mounted) return;
      if (path == null) return;
      await _installZip(path);
    } on PlatformException catch (error) {
      showAppSnackBar(context, error.message ?? error.code);
    } on MissingPluginException {
      showAppSnackBar(context, l10n.modulesInstallFailed);
    } catch (error) {
      showAppSnackBar(context, error.toString());
    } finally {
      if (mounted) setState(() => _installing = false);
    }
  }

  Future<void> _installZip(String path) async {
    final l10n = AppLocalizations.of(context)!;
    await _openOutput(
      runningTitle: l10n.modulesInstallRunning,
      successTitle: l10n.modulesInstallDone,
      failureTitle: l10n.modulesInstallFailed,
      logFileName: 'nekosu-module-install',
      task: () => ModuleBridge.installOutput(path),
    );
    if (mounted) await _load();
  }

  Future<void> _runAction(ModuleInfo module) async {
    if (_busyModules.contains(module.id)) return;
    final l10n = AppLocalizations.of(context)!;
    setState(() => _busyModules.add(module.id));
    try {
      await _openOutput(
        runningTitle: l10n.modulesActionRunning,
        successTitle: l10n.modulesActionDone,
        failureTitle: l10n.modulesActionFailed,
        logFileName: 'nekosu-module-action-${module.id}',
        task: () => ModuleBridge.actionOutput(module.id),
      );
      if (mounted) await _load();
    } finally {
      if (mounted) setState(() => _busyModules.remove(module.id));
    }
  }

  Future<void> _openOutput({
    required String runningTitle,
    required String successTitle,
    required String failureTitle,
    required String logFileName,
    required OutputTask task,
  }) {
    final l10n = AppLocalizations.of(context)!;
    return Navigator.of(context).push(
      MaterialPageRoute<void>(
        builder: (context) => OutputPage(
          task: task,
          runningTitle: runningTitle,
          successTitle: successTitle,
          failureTitle: failureTitle,
          logFileName: logFileName,
          completionAction: OutputAction(
            label: l10n.dialogDone,
            icon: Icons.check_rounded,
            onPressed: (context) => Navigator.of(context).pop(),
          ),
        ),
      ),
    );
  }

  Future<bool> _runModuleOperation(ModuleInfo module, String operation) async {
    if (_busyModules.contains(module.id)) return false;
    setState(() => _busyModules.add(module.id));
    final result = await ModuleBridge.run(operation: operation, id: module.id);
    if (!mounted) return result.ok;
    setState(() => _busyModules.remove(module.id));
    if (!result.ok) {
      showAppSnackBar(
        context,
        result.output.trim().isEmpty
            ? 'Operation failed'
            : result.output.trim(),
      );
      return false;
    } else {
      await _load();
      return true;
    }
  }

  Future<void> _setEnabled(ModuleInfo module, bool enabled) async {
    await _runModuleOperation(module, enabled ? 'enable' : 'disable');
  }

  Future<void> _removeModule(ModuleInfo module) async {
    final succeeded = await _runModuleOperation(module, 'remove');
    if (mounted && succeeded) {
      final l10n = AppLocalizations.of(context)!;
      showAppSnackBar(context, l10n.modulesRemoveHint);
    }
  }

  Future<void> _undoRemove(ModuleInfo module) async {
    await _runModuleOperation(module, 'undoRemove');
  }

  Future<void> _uninstallModule(ModuleInfo module) async {
    final l10n = AppLocalizations.of(context)!;
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(l10n.modulesUninstall),
        content: Text(l10n.modulesUninstallConfirm(module.title)),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context, false),
            child: Text(l10n.dialogCancel),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(context, true),
            child: Text(l10n.dialogConfirm),
          ),
        ],
      ),
    );
    if (confirmed == true) {
      await _runModuleOperation(module, 'uninstall');
    }
  }

  Future<void> _updateModule(ModuleInfo module, ModuleUpdate update) async {
    if (_busyModules.contains(module.id)) return;
    final l10n = AppLocalizations.of(context)!;
    final confirmed = await _confirmUpdate(module, update);
    if (confirmed != true || !mounted) return;

    setState(() => _busyModules.add(module.id));
    final navigator = Navigator.of(context, rootNavigator: true);
    final progress = ValueNotifier<double?>(null);
    var dialogShown = false;
    String? stagedPath;
    try {
      showDialog<void>(
        context: context,
        barrierDismissible: false,
        builder: (context) => PopScope(
          canPop: false,
          child: AlertDialog(
            title: Text(l10n.modulesUpdateDownloading),
            content: SizedBox(
              width: 260,
              child: ValueListenableBuilder<double?>(
                valueListenable: progress,
                builder: (context, value, _) =>
                    LinearProgressIndicator(value: value),
              ),
            ),
          ),
        ),
      ).ignore();
      dialogShown = true;
      final directory = await getTemporaryDirectory();
      final safeId = module.id.replaceAll(RegExp(r'[^A-Za-z0-9._-]'), '_');
      final staged = File('${directory.path}/nekosu-update-$safeId.zip');
      await ModuleUpdateService.download(
        update,
        destination: staged,
        onProgress: (received, total) {
          if (total > 0) progress.value = received / total;
        },
      );
      stagedPath = staged.path;
    } on ModuleUpdateException catch (error) {
      if (mounted) {
        showAppSnackBar(
          context,
          '${l10n.modulesUpdateDownloadFailed}: ${error.message}',
        );
      }
    } on Object catch (error) {
      if (mounted) {
        showAppSnackBar(context, '${l10n.modulesUpdateDownloadFailed}: $error');
      }
    } finally {
      if (dialogShown) navigator.pop();
      progress.dispose();
      if (mounted) setState(() => _busyModules.remove(module.id));
    }

    if (stagedPath != null && mounted) {
      try {
        await _installZip(stagedPath);
      } on Object catch (error) {
        if (mounted) showAppSnackBar(context, '$error');
      }
    }
  }

  Future<bool?> _confirmUpdate(ModuleInfo module, ModuleUpdate update) {
    final scheme = Theme.of(context).colorScheme;
    return showGeneralDialog<bool>(
      context: context,
      barrierDismissible: true,
      barrierLabel: MaterialLocalizations.of(context).modalBarrierDismissLabel,
      barrierColor: scheme.scrim.withValues(alpha: 0.4),
      transitionDuration: const Duration(milliseconds: 260),
      pageBuilder: (context, animation, secondaryAnimation) =>
          _ModuleUpdateDialog(
            module: module,
            update: update,
            openChangelog: _openChangelog,
          ),
      transitionBuilder: (context, animation, secondaryAnimation, child) {
        final curved = CurvedAnimation(
          parent: animation,
          curve: Curves.easeOutCubic,
          reverseCurve: Curves.easeInCubic,
        );
        final slide = Tween<Offset>(
          begin: const Offset(0, 0.04),
          end: Offset.zero,
        ).animate(curved);
        return FadeTransition(
          opacity: curved,
          child: SlideTransition(
            position: slide,
            child: ScaleTransition(
              scale: Tween<double>(begin: 0.95, end: 1).animate(curved),
              child: child,
            ),
          ),
        );
      },
    );
  }

  Future<void> _openChangelog(String url) async {
    final l10n = AppLocalizations.of(context)!;
    try {
      final launched = await launchUrl(
        Uri.parse(url),
        mode: LaunchMode.externalApplication,
      );
      if (!launched && mounted) {
        showAppSnackBar(context, l10n.modulesUpdateChangelogFailed);
      }
    } on Object catch (error) {
      if (mounted) {
        showAppSnackBar(
          context,
          '${l10n.modulesUpdateChangelogFailed}: $error',
        );
      }
    }
  }

  Future<void> _openWebUi(ModuleInfo module) async {
    final result = await ModuleBridge.prepareWebUi(module.id);
    if (!mounted) return;
    if (!result.ok) {
      showAppSnackBar(
        context,
        result.output.trim().isEmpty
            ? 'Unable to open WebUI'
            : result.output.trim(),
      );
      return;
    }
    final entryPath = result.output.trim();
    if (entryPath.isEmpty) {
      showAppSnackBar(context, 'Unable to open WebUI');
      return;
    }
    await Navigator.of(context).push(
      MaterialPageRoute<void>(
        builder: (context) => WebUiPage(entryPath: entryPath, module: module),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final l10n = AppLocalizations.of(context)!;
    final navBarClearance = appNavBarClearance(context);
    final scheme = Theme.of(context).colorScheme;

    return Scaffold(
      backgroundColor: scheme.surfaceContainer,
      floatingActionButton: Padding(
        padding: EdgeInsets.only(bottom: navBarClearance),
        child: FloatingActionButton(
          onPressed: _installModule,
          tooltip: l10n.modulesInstall,
          child: const Icon(Icons.add_rounded),
        ),
      ),
      floatingActionButtonLocation: FloatingActionButtonLocation.endFloat,
      body: RefreshIndicator(
        color: scheme.primary,
        backgroundColor: scheme.surfaceBright,
        onRefresh: () => _load(refreshUpdates: true),
        child: CustomScrollView(
          physics: const AlwaysScrollableScrollPhysics(),
          slivers: [
            AppSliverAppBar(
              title: Text(l10n.modulesLabel),
              actions: [
                IconButton(
                  onPressed: () => _load(refreshUpdates: true),
                  icon: const Icon(Icons.refresh_rounded),
                  tooltip: l10n.retry,
                ),
              ],
            ),
            if (_loading)
              SliverFillRemaining(
                hasScrollBody: false,
                child: Padding(
                  padding: EdgeInsets.only(bottom: navBarClearance),
                  child: const AppLoadingView(),
                ),
              )
            else if (_modules.isEmpty)
              SliverFillRemaining(
                hasScrollBody: false,
                child: Padding(
                  padding: EdgeInsets.fromLTRB(
                    32,
                    16,
                    32,
                    navBarClearance + 72,
                  ),
                  child: _ModulesEmptyState(
                    failed: _loadError != null,
                    error: _loadError,
                    onRetry: _load,
                  ),
                ),
              )
            else
              SliverPadding(
                padding: EdgeInsets.fromLTRB(16, 12, 16, navBarClearance + 76),
                sliver: SliverList.builder(
                  itemCount: _modules.length,
                  itemBuilder: (context, index) {
                    final module = _modules[index];
                    final updateCheck = _updateChecks[module.id];
                    final update = updateCheck?.update;
                    return Padding(
                      padding: EdgeInsets.only(
                        bottom: index == _modules.length - 1 ? 0 : 10,
                      ),
                      child: _ModuleCard(
                        module: module,
                        updateCheck: updateCheck,
                        busy: _busyModules.contains(module.id),
                        onToggle: (enabled) => _setEnabled(module, enabled),
                        onAction: () => _runAction(module),
                        onWebUi: () => _openWebUi(module),
                        onUpdate: update == null
                            ? null
                            : () => _updateModule(module, update),
                        onRemove: () => _removeModule(module),
                        onUndoRemove: () => _undoRemove(module),
                        onUninstall: () => _uninstallModule(module),
                      ),
                    );
                  },
                ),
              ),
          ],
        ),
      ),
    );
  }
}

class _ModuleUpdateDialog extends StatefulWidget {
  const _ModuleUpdateDialog({
    required this.module,
    required this.update,
    required this.openChangelog,
  });

  final ModuleInfo module;
  final ModuleUpdate update;
  final Future<void> Function(String url) openChangelog;

  @override
  State<_ModuleUpdateDialog> createState() => _ModuleUpdateDialogState();
}

class _ModuleUpdateDialogState extends State<_ModuleUpdateDialog> {
  Future<String>? _changelog;

  @override
  void initState() {
    super.initState();
    _loadChangelog();
  }

  void _loadChangelog() {
    _changelog = widget.update.changelogUrl == null
        ? null
        : ModuleUpdateService.fetchChangelog(widget.update);
  }

  @override
  Widget build(BuildContext context) {
    final l10n = AppLocalizations.of(context)!;
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    final changelogUrl = widget.update.changelogUrl;
    return AlertDialog(
      title: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Container(
            width: 48,
            height: 48,
            decoration: BoxDecoration(
              color: scheme.secondaryContainer,
              borderRadius: BorderRadius.circular(14),
            ),
            child: Icon(
              Icons.system_update_rounded,
              color: scheme.onSecondaryContainer,
            ),
          ),
          const SizedBox(width: 16),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  l10n.modulesUpdateNewVersionTitle,
                  style: theme.textTheme.titleLarge?.copyWith(
                    fontWeight: FontWeight.w600,
                  ),
                ),
                const SizedBox(height: 4),
                Text(
                  widget.module.title,
                  maxLines: 2,
                  overflow: TextOverflow.ellipsis,
                  style: theme.textTheme.bodyMedium?.copyWith(
                    color: scheme.onSurfaceVariant,
                  ),
                ),
                const SizedBox(height: 8),
                Row(
                  children: [
                    DecoratedBox(
                      decoration: BoxDecoration(
                        color: scheme.secondaryContainer,
                        borderRadius: BorderRadius.circular(8),
                      ),
                      child: Padding(
                        padding: const EdgeInsets.symmetric(
                          horizontal: 8,
                          vertical: 3,
                        ),
                        child: Text(
                          widget.update.version,
                          style: theme.textTheme.labelMedium?.copyWith(
                            color: scheme.onSecondaryContainer,
                            fontWeight: FontWeight.w600,
                          ),
                        ),
                      ),
                    ),
                    if (changelogUrl != null) ...[
                      const SizedBox(width: 12),
                      Flexible(
                        child: InkWell(
                          onTap: () =>
                              unawaited(widget.openChangelog(changelogUrl)),
                          borderRadius: BorderRadius.circular(6),
                          child: Padding(
                            padding: const EdgeInsets.symmetric(
                              horizontal: 2,
                              vertical: 2,
                            ),
                            child: Row(
                              mainAxisSize: MainAxisSize.min,
                              children: [
                                Text(
                                  l10n.modulesUpdateChangelog,
                                  style: theme.textTheme.bodyMedium?.copyWith(
                                    color: scheme.primary,
                                    fontWeight: FontWeight.w500,
                                  ),
                                ),
                                const SizedBox(width: 2),
                                Icon(
                                  Icons.open_in_new_rounded,
                                  size: 16,
                                  color: scheme.primary,
                                ),
                              ],
                            ),
                          ),
                        ),
                      ),
                    ],
                  ],
                ),
              ],
            ),
          ),
        ],
      ),
      content: SizedBox(
        width: double.maxFinite,
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxHeight: 325),
          child: AnimatedSize(
            duration: const Duration(milliseconds: 220),
            curve: Curves.easeOutCubic,
            alignment: Alignment.topCenter,
            child: Container(
              clipBehavior: Clip.antiAlias,
              decoration: BoxDecoration(
                color: scheme.surfaceBright,
                borderRadius: BorderRadius.circular(16),
              ),
              child: Padding(
                padding: const EdgeInsets.all(16),
                child: SingleChildScrollView(
                  child: changelogUrl == null
                      ? Text(
                          l10n.modulesUpdateChangelogEmpty,
                          style: theme.textTheme.bodyMedium?.copyWith(
                            color: scheme.onSurfaceVariant,
                          ),
                        )
                      : _buildChangelog(
                          l10n: l10n,
                          theme: theme,
                          scheme: scheme,
                        ),
                ),
              ),
            ),
          ),
        ),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context, false),
          child: Text(l10n.modulesUpdateLater),
        ),
        FilledButton.icon(
          onPressed: () => Navigator.pop(context, true),
          icon: const Icon(Icons.system_update_rounded, size: 18),
          label: Text(l10n.modulesUpdateNow),
          style: FilledButton.styleFrom(
            shape: const StadiumBorder(),
            padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 12),
          ),
        ),
      ],
    );
  }

  Widget _buildChangelog({
    required AppLocalizations l10n,
    required ThemeData theme,
    required ColorScheme scheme,
  }) {
    return FutureBuilder<String>(
      future: _changelog,
      builder: (context, snapshot) {
        final Widget child;
        if (snapshot.connectionState != ConnectionState.done) {
          child = const Center(
            key: ValueKey('changelog-loading'),
            child: Padding(
              padding: EdgeInsets.symmetric(vertical: 24),
              child: SizedBox(
                width: 24,
                height: 24,
                child: CircularProgressIndicator(strokeWidth: 2),
              ),
            ),
          );
        } else if (snapshot.hasError) {
          child = Column(
            key: const ValueKey('changelog-error'),
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(
                '${snapshot.error}',
                style: theme.textTheme.bodyMedium?.copyWith(
                  color: scheme.error,
                ),
              ),
              TextButton.icon(
                onPressed: () => setState(_loadChangelog),
                icon: const Icon(Icons.refresh_rounded, size: 16),
                label: Text(l10n.retry),
              ),
            ],
          );
        } else {
          final changelog = snapshot.data?.trim() ?? '';
          if (changelog.isEmpty) {
            child = Text(
              l10n.modulesUpdateChangelogEmpty,
              key: const ValueKey('changelog-empty'),
              style: theme.textTheme.bodyMedium?.copyWith(
                color: scheme.onSurfaceVariant,
              ),
            );
          } else {
            child = KeyedSubtree(
              key: const ValueKey('changelog-content'),
              child: MarkdownBody(
                data: changelog,
                selectable: true,
                styleSheet: _markdownStyle(theme, scheme),
                onTapLink: (text, href, title) {
                  if (href == null) return;
                  final uri = Uri.tryParse(href);
                  if (uri == null || !ModuleUpdate.isHttpUri(uri)) return;
                  unawaited(widget.openChangelog(href));
                },
              ),
            );
          }
        }
        return AnimatedSwitcher(
          duration: const Duration(milliseconds: 180),
          switchInCurve: Curves.easeOut,
          switchOutCurve: Curves.easeIn,
          child: child,
        );
      },
    );
  }

  MarkdownStyleSheet _markdownStyle(ThemeData theme, ColorScheme scheme) {
    final base = MarkdownStyleSheet.fromTheme(theme);
    final body = theme.textTheme.bodyMedium?.copyWith(height: 1.5);
    return base.copyWith(
      p: body,
      pPadding: const EdgeInsets.only(bottom: 8),
      a: body?.copyWith(
        color: scheme.primary,
        decoration: TextDecoration.underline,
        decorationColor: scheme.primary,
      ),
      em: body?.copyWith(fontStyle: FontStyle.italic),
      strong: body?.copyWith(fontWeight: FontWeight.w600),
      h1: theme.textTheme.titleLarge?.copyWith(fontWeight: FontWeight.w600),
      h2: theme.textTheme.titleMedium?.copyWith(fontWeight: FontWeight.w600),
      h3: theme.textTheme.titleSmall?.copyWith(fontWeight: FontWeight.w600),
      h4: theme.textTheme.bodyLarge?.copyWith(fontWeight: FontWeight.w600),
      h5: theme.textTheme.bodyMedium?.copyWith(fontWeight: FontWeight.w600),
      h6: theme.textTheme.bodyMedium?.copyWith(fontWeight: FontWeight.w600),
      code: theme.textTheme.bodySmall?.copyWith(
        fontFamily: 'monospace',
        backgroundColor: scheme.surfaceContainerHighest,
        color: scheme.onSurface,
      ),
      codeblockDecoration: BoxDecoration(
        color: scheme.surfaceContainerHighest,
        borderRadius: BorderRadius.circular(8),
      ),
      codeblockPadding: const EdgeInsets.all(12),
      blockquote: body?.copyWith(color: scheme.onSurfaceVariant),
      blockquoteDecoration: BoxDecoration(
        color: scheme.surfaceContainerHighest,
        border: Border(left: BorderSide(color: scheme.primary, width: 3)),
      ),
      blockquotePadding: const EdgeInsets.fromLTRB(12, 8, 8, 8),
      horizontalRuleDecoration: BoxDecoration(
        border: Border(top: BorderSide(color: scheme.outlineVariant)),
      ),
      tableBorder: TableBorder.all(color: scheme.outlineVariant),
      tableHead: theme.textTheme.bodySmall?.copyWith(
        fontWeight: FontWeight.w600,
      ),
      tableBody: theme.textTheme.bodySmall,
      tableCellsPadding: const EdgeInsets.fromLTRB(8, 4, 8, 4),
      listBullet: body,
      listIndent: 20,
      blockSpacing: 10,
    );
  }
}

class _ModuleCard extends StatefulWidget {
  const _ModuleCard({
    required this.module,
    required this.updateCheck,
    required this.busy,
    required this.onToggle,
    required this.onAction,
    required this.onWebUi,
    required this.onUpdate,
    required this.onRemove,
    required this.onUndoRemove,
    required this.onUninstall,
  });

  final ModuleInfo module;
  final ModuleUpdateCheck? updateCheck;
  final bool busy;
  final ValueChanged<bool> onToggle;
  final VoidCallback onAction;
  final VoidCallback onWebUi;
  final VoidCallback? onUpdate;
  final VoidCallback onRemove;
  final VoidCallback onUndoRemove;
  final VoidCallback onUninstall;

  @override
  State<_ModuleCard> createState() => _ModuleCardState();
}

enum _ModuleMenu { uninstall }

class _ModuleCardState extends State<_ModuleCard> {
  bool _expanded = false;

  @override
  Widget build(BuildContext context) {
    final l10n = AppLocalizations.of(context)!;
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    final module = widget.module;
    final update = widget.updateCheck?.update;
    final pendingRemoval = module.remove;
    final textDecoration = pendingRemoval ? TextDecoration.lineThrough : null;
    final actionsVisible = !pendingRemoval && module.enabled;
    final actionEnabled = actionsVisible && !widget.busy;
    final showUpdate = update != null && !pendingRemoval && !widget.busy;
    final hasDescription = module.description.trim().isNotEmpty;
    final actionCount = [
      module.hasActionScript,
      module.hasWebUi,
    ].where((available) => available).length;
    final showActionText = actionCount <= 1;

    return Material(
      color: scheme.surfaceBright,
      borderRadius: BorderRadius.circular(20),
      clipBehavior: Clip.antiAlias,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          InkWell(
            onTap: hasDescription
                ? () => setState(() => _expanded = !_expanded)
                : null,
            child: Padding(
              padding: const EdgeInsets.fromLTRB(16, 14, 12, 10),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Text(
                              module.title,
                              style: theme.textTheme.titleMedium?.copyWith(
                                decoration: textDecoration,
                              ),
                            ),
                            if (module.version.isNotEmpty)
                              Text(
                                '${l10n.modulesVersion}: ${module.version}',
                                style: theme.textTheme.bodyMedium?.copyWith(
                                  color: scheme.onSurfaceVariant,
                                  decoration: textDecoration,
                                ),
                              ),
                            if (module.author.isNotEmpty)
                              Text(
                                '${l10n.modulesAuthor}: ${module.author}',
                                style: theme.textTheme.bodyMedium?.copyWith(
                                  color: scheme.onSurfaceVariant,
                                  decoration: textDecoration,
                                ),
                              ),
                          ],
                        ),
                      ),
                      const SizedBox(width: 12),
                      if (widget.busy)
                        const Padding(
                          padding: EdgeInsets.all(10),
                          child: SizedBox(
                            width: 20,
                            height: 20,
                            child: CircularProgressIndicator(strokeWidth: 2),
                          ),
                        )
                      else
                        Switch(
                          value: module.enabled,
                          onChanged: pendingRemoval ? null : widget.onToggle,
                        ),
                    ],
                  ),
                  if (hasDescription) ...[
                    const SizedBox(height: 8),
                    AnimatedSize(
                      duration: const Duration(milliseconds: 220),
                      curve: Curves.easeOutCubic,
                      alignment: Alignment.topCenter,
                      child: Text(
                        module.description,
                        maxLines: _expanded ? null : 3,
                        overflow: _expanded
                            ? TextOverflow.visible
                            : TextOverflow.ellipsis,
                        style: theme.textTheme.bodyMedium?.copyWith(
                          color: scheme.onSurfaceVariant,
                          decoration: textDecoration,
                        ),
                      ),
                    ),
                  ],
                  if (module.metamodule || module.update) ...[
                    const SizedBox(height: 8),
                    Wrap(
                      spacing: 6,
                      runSpacing: 6,
                      children: [
                        if (module.metamodule)
                          AppTag(
                            label: l10n.modulesMetamodule,
                            color: scheme.primary,
                          ),
                        if (module.update)
                          AppTag(
                            label: l10n.modulesUpdate,
                            color: scheme.tertiary,
                          ),
                      ],
                    ),
                  ],
                ],
              ),
            ),
          ),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16),
            child: Divider(
              height: 1,
              color: scheme.outlineVariant.withValues(alpha: 0.5),
            ),
          ),
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 4, 16, 10),
            child: Row(
              children: [
                Expanded(
                  child: Wrap(
                    spacing: 8,
                    runSpacing: 8,
                    children: [
                      if (actionsVisible && module.hasActionScript)
                        _ModuleCardButton(
                          onPressed: actionEnabled ? widget.onAction : null,
                          icon: Icons.play_arrow_rounded,
                          label: showActionText ? l10n.modulesAction : null,
                        ),
                      if (actionsVisible && module.hasWebUi)
                        _ModuleCardButton(
                          onPressed: actionEnabled ? widget.onWebUi : null,
                          icon: Icons.code_rounded,
                          label: showActionText ? l10n.modulesWebUi : null,
                        ),
                      AnimatedSize(
                        duration: const Duration(milliseconds: 220),
                        curve: Curves.easeOutCubic,
                        alignment: Alignment.centerLeft,
                        child: AnimatedSwitcher(
                          duration: const Duration(milliseconds: 180),
                          switchInCurve: Curves.easeOut,
                          switchOutCurve: Curves.easeIn,
                          transitionBuilder: (child, animation) {
                            return FadeTransition(
                              opacity: animation,
                              child: ScaleTransition(
                                scale: Tween<double>(
                                  begin: 0.85,
                                  end: 1,
                                ).animate(animation),
                                child: child,
                              ),
                            );
                          },
                          child: showUpdate
                              ? KeyedSubtree(
                                  key: const ValueKey('module-update'),
                                  child: _ModuleCardButton(
                                    onPressed: widget.onUpdate,
                                    icon: Icons.system_update_rounded,
                                    label: showActionText
                                        ? l10n.modulesUpdateNow
                                        : null,
                                    tooltip: showActionText
                                        ? null
                                        : l10n.modulesUpdateNow,
                                  ),
                                )
                              : const SizedBox.shrink(
                                  key: ValueKey('module-update-empty'),
                                ),
                        ),
                      ),
                    ],
                  ),
                ),
                const SizedBox(width: 8),
                _ModuleCardButton(
                  onPressed: widget.busy
                      ? null
                      : pendingRemoval
                      ? widget.onUndoRemove
                      : widget.onRemove,
                  icon: pendingRemoval
                      ? Icons.refresh_rounded
                      : Icons.delete_rounded,
                  label: pendingRemoval
                      ? l10n.modulesUndoRemove
                      : l10n.modulesRemove,
                ),
                const SizedBox(width: 4),
                AppPopupMenu<_ModuleMenu>(
                  enabled: !widget.busy,
                  tooltip: l10n.modulesMore,
                  icon: Icons.more_vert_rounded,
                  onSelected: (_) => widget.onUninstall(),
                  items: [
                    AppPopupMenuItem(
                      value: _ModuleMenu.uninstall,
                      label: l10n.modulesUninstall,
                      destructive: true,
                    ),
                  ],
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class _ModuleCardButton extends StatelessWidget {
  const _ModuleCardButton({
    required this.onPressed,
    required this.icon,
    this.label,
    this.tooltip,
  });

  final VoidCallback? onPressed;
  final IconData icon;
  final String? label;
  final String? tooltip;

  @override
  Widget build(BuildContext context) {
    final textStyle = Theme.of(context).textTheme.labelMedium;
    final button = FilledButton.tonal(
      onPressed: onPressed,
      style: FilledButton.styleFrom(
        minimumSize: const Size(52, 36),
        padding: const EdgeInsets.symmetric(horizontal: 12),
        tapTargetSize: MaterialTapTargetSize.shrinkWrap,
        textStyle: textStyle,
        shape: const StadiumBorder(),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(icon, size: 20),
          if (label != null) ...[const SizedBox(width: 7), Text(label!)],
        ],
      ),
    );
    if (tooltip == null) return button;
    return Tooltip(message: tooltip!, child: button);
  }
}

class _ModulesEmptyState extends StatelessWidget {
  const _ModulesEmptyState({
    required this.failed,
    required this.error,
    required this.onRetry,
  });

  final bool failed;
  final String? error;
  final VoidCallback onRetry;

  @override
  Widget build(BuildContext context) {
    final l10n = AppLocalizations.of(context)!;
    final scheme = Theme.of(context).colorScheme;
    final textTheme = Theme.of(context).textTheme;

    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(
            failed ? Icons.error_outline_rounded : Icons.extension_outlined,
            size: 52,
            color: failed ? scheme.error : scheme.onSurfaceVariant,
          ),
          const SizedBox(height: 14),
          Text(
            failed ? l10n.modulesLoadFailed : l10n.modulesEmpty,
            textAlign: TextAlign.center,
            style: textTheme.titleMedium,
          ),
          const SizedBox(height: 6),
          Text(
            failed && error != null && error!.trim().isNotEmpty
                ? error!.trim()
                : l10n.modulesEmptyHint,
            textAlign: TextAlign.center,
            style: textTheme.bodyMedium?.copyWith(
              color: scheme.onSurfaceVariant,
            ),
          ),
          const SizedBox(height: 20),
          if (failed)
            OutlinedButton.icon(
              onPressed: onRetry,
              icon: const Icon(Icons.refresh_rounded),
              label: Text(l10n.retry),
            ),
        ],
      ),
    );
  }
}
