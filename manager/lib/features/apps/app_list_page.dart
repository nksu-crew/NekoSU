import 'dart:async';

import 'package:material_ui/material_ui.dart';
import 'package:nekosu/core/ncore_ffi.dart';
import 'package:nekosu/data/apps.dart';
import 'package:nekosu/features/apps/app_detail_page.dart';
import 'package:nekosu/l10n/app_localizations.dart';
import 'package:nekosu/shared/layout_insets.dart';
import 'package:nekosu/shared/widgets/app_icon.dart';
import 'package:nekosu/shared/widgets/app_popup_menu.dart';
import 'package:nekosu/shared/widgets/app_sliver_app_bar.dart';
import 'package:nekosu/shared/widgets/app_status_view.dart';
import 'package:nekosu/shared/widgets/app_tag.dart';
import 'package:nekosu/shared/widgets/card_group.dart';

class AppListPage extends StatefulWidget {
  const AppListPage({super.key});

  @override
  State<AppListPage> createState() => _AppListPageState();
}

class _AppListPageState extends State<AppListPage> {
  final _searchController = TextEditingController();
  final _searchFocus = FocusNode();

  Timer? _searchDebounce;
  List<AppInfo> _apps = const [];
  List<AppInfo> _visibleApps = const [];
  Map<int, AppConfig> _profiles = const {};
  AppFilter _filter = AppFilter.user;
  bool _loading = true;
  bool _searching = false;
  bool _loaded = false;
  Object? _error;

  @override
  void initState() {
    super.initState();
    _load();
  }

  @override
  void dispose() {
    _searchDebounce?.cancel();
    _searchController.dispose();
    _searchFocus.dispose();
    super.dispose();
  }

  Map<int, AppConfig> _readProfiles() {
    if (!NcoreFfi.profileApiAvailable) return const {};
    final text = NcoreFfi.profiles;
    if (text == null || text.isEmpty) return const {};
    try {
      return parseProfiles(text);
    } catch (_) {
      return const {};
    }
  }

  Future<void> _load({bool force = false}) async {
    if (force) AppIconCache.clear();
    if (!_loaded) {
      setState(() {
        _loading = true;
        _error = null;
      });
    }

    try {
      final apps = await AppsPlatform.listApps();
      final profiles = _readProfiles();
      if (!mounted) return;
      setState(() {
        _apps = apps;
        _profiles = profiles;
        _loading = false;
        _loaded = true;
        _error = null;
        _refreshVisible();
      });
    } catch (error) {
      if (!mounted) return;
      setState(() {
        _loading = false;
        _error = error;
        if (!_loaded) _visibleApps = const [];
      });
    }
  }

  void _refreshVisible() {
    final query = _searchController.text.trim().toLowerCase();
    final filtered = _apps.where((app) {
      final passFilter = switch (_filter) {
        AppFilter.all => true,
        AppFilter.launchable => app.isLaunchable,
        AppFilter.system => app.isSystem,
        AppFilter.user => !app.isSystem,
      };
      if (!passFilter) return false;
      if (query.isEmpty) return true;
      return app.name.toLowerCase().contains(query) ||
          app.packageName.toLowerCase().contains(query);
    }).toList();

    filtered.sort((a, b) {
      final aAllowed = _profiles.containsKey(a.uid);
      final bAllowed = _profiles.containsKey(b.uid);
      if (aAllowed != bAllowed) return aAllowed ? -1 : 1;
      final byName = a.name.toLowerCase().compareTo(b.name.toLowerCase());
      return byName != 0 ? byName : a.packageName.compareTo(b.packageName);
    });
    _visibleApps = filtered;
  }

  void _onSearchChanged(String _) {
    _searchDebounce?.cancel();
    _searchDebounce = Timer(const Duration(milliseconds: 150), () {
      if (!mounted) return;
      setState(_refreshVisible);
    });
  }

  void _setFilter(AppFilter filter) {
    if (_filter == filter) return;
    setState(() {
      _filter = filter;
      _refreshVisible();
    });
  }

  void _startSearch() {
    setState(() => _searching = true);
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) _searchFocus.requestFocus();
    });
  }

  void _stopSearch() {
    _searchDebounce?.cancel();
    _searchController.clear();
    _searchFocus.unfocus();
    setState(() {
      _searching = false;
      _refreshVisible();
    });
  }

  String _filterLabel(AppLocalizations l10n) {
    return switch (_filter) {
      AppFilter.all => l10n.filterAllApps,
      AppFilter.launchable => l10n.filterLaunchableApps,
      AppFilter.system => l10n.filterSystemApps,
      AppFilter.user => l10n.filterUserApps,
    };
  }

  Future<void> _openApp(AppInfo app) async {
    await Navigator.of(context).push<void>(
      MaterialPageRoute(
        builder: (context) =>
            AppDetailPage(app: app, config: _profiles[app.uid]),
      ),
    );
    if (!mounted) return;
    setState(() {
      _profiles = _readProfiles();
      _refreshVisible();
    });
  }

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final l10n = AppLocalizations.of(context)!;
    final navBarClearance = appNavBarClearance(context);

    return Scaffold(
      backgroundColor: scheme.surfaceContainer,
      body: RefreshIndicator(
        color: scheme.primary,
        backgroundColor: scheme.surfaceBright,
        onRefresh: () => _load(force: true),
        child: CustomScrollView(
          physics: const AlwaysScrollableScrollPhysics(),
          slivers: [
            AppSliverAppBar(
              leading: _searching
                  ? IconButton(
                      onPressed: _stopSearch,
                      icon: const Icon(Icons.close_rounded),
                      tooltip: MaterialLocalizations.of(context)
                          .closeButtonTooltip,
                    )
                  : null,
              title: _searching
                  ? TextField(
                      controller: _searchController,
                      focusNode: _searchFocus,
                      autofocus: true,
                      textInputAction: TextInputAction.search,
                      style: Theme.of(context).textTheme.titleMedium,
                      decoration: InputDecoration(
                        hintText: l10n.searchAppsHint,
                        border: InputBorder.none,
                        isDense: true,
                      ),
                      onChanged: _onSearchChanged,
                    )
                  : Text(_filterLabel(l10n)),
              actions: [
                if (!_searching)
                  IconButton(
                    onPressed: _startSearch,
                    icon: const Icon(Icons.search_rounded),
                    tooltip: l10n.searchAppsHint,
                  ),
                AppPopupMenu<AppFilter>(
                  icon: Icons.filter_list_rounded,
                  tooltip: l10n.filterApps,
                  onSelected: _setFilter,
                  items: [
                    for (final filter in AppFilter.values)
                      AppPopupMenuItem(
                        value: filter,
                        label: switch (filter) {
                          AppFilter.all => l10n.filterAllApps,
                          AppFilter.launchable => l10n.filterLaunchableApps,
                          AppFilter.system => l10n.filterSystemApps,
                          AppFilter.user => l10n.filterUserApps,
                        },
                        selected: filter == _filter,
                      ),
                  ],
                ),
              ],
            ),
            if (_loading)
              SliverFillRemaining(
                hasScrollBody: false,
                child: Padding(
                  padding: EdgeInsets.only(bottom: navBarClearance),
                  child: AppLoadingView(label: l10n.loadingApps),
                ),
              )
            else if (_error != null && !_loaded)
              SliverFillRemaining(
                hasScrollBody: false,
                child: Padding(
                  padding: EdgeInsets.only(bottom: navBarClearance),
                  child: AppErrorView(
                    message: l10n.loadAppsFailed,
                    retryLabel: l10n.retry,
                    onRetry: () => _load(),
                  ),
                ),
              )
            else if (_visibleApps.isEmpty)
              SliverFillRemaining(
                hasScrollBody: false,
                child: Padding(
                  padding: EdgeInsets.only(bottom: navBarClearance),
                  child: _EmptyState(
                    query: _searchController.text.trim(),
                    searching: _searching,
                    l10n: l10n,
                  ),
                ),
              )
            else
              SliverPadding(
                padding: EdgeInsets.fromLTRB(16, 12, 16, navBarClearance),
                sliver: SliverList.builder(
                  itemCount: _visibleApps.length,
                  itemBuilder: (context, index) {
                    final app = _visibleApps[index];
                    return Padding(
                      padding: EdgeInsets.only(
                        bottom: index == _visibleApps.length - 1 ? 0 : 2,
                      ),
                      child: _AppRow(
                        app: app,
                        allowed: _profiles.containsKey(app.uid),
                        shape: groupShape(index, _visibleApps.length),
                        onTap: () => _openApp(app),
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

class _AppRow extends StatelessWidget {
  const _AppRow({
    required this.app,
    required this.allowed,
    required this.shape,
    required this.onTap,
  });

  final AppInfo app;
  final bool allowed;
  final BorderRadius shape;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final textTheme = Theme.of(context).textTheme;
    final l10n = AppLocalizations.of(context)!;

    return ClipRRect(
      borderRadius: shape,
      child: Material(
        color: scheme.surfaceBright,
        child: InkWell(
          onTap: onTap,
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
            child: Row(
              children: [
                AppIcon(packageName: app.packageName),
                const SizedBox(width: 14),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      Text(
                        app.name,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: textTheme.bodyLarge,
                      ),
                      Text(
                        app.packageName,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: textTheme.bodyMedium?.copyWith(
                          color: scheme.onSurfaceVariant.withValues(alpha: 0.7),
                        ),
                      ),
                    ],
                  ),
                ),
                if (allowed || app.isSystem) ...[
                  const SizedBox(width: 8),
                  Wrap(
                    spacing: 4,
                    children: [
                      if (allowed)
                        AppTag(
                          label: l10n.tagAllowed,
                          color: scheme.primary,
                          backgroundOpacity: 0.12,
                        ),
                      if (app.isSystem)
                        AppTag(
                          label: l10n.tagSystem,
                          color: scheme.secondary,
                          backgroundOpacity: 0.12,
                        ),
                    ],
                  ),
                ],
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _EmptyState extends StatelessWidget {
  const _EmptyState({
    required this.query,
    required this.searching,
    required this.l10n,
  });

  final String query;
  final bool searching;
  final AppLocalizations l10n;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(
            searching ? Icons.search_rounded : Icons.android_rounded,
            size: 40,
            color: scheme.onSurfaceVariant.withValues(alpha: 0.25),
          ),
          const SizedBox(height: 8),
          Text(
            searching && query.isNotEmpty
                ? l10n.noMatchingApps(query)
                : l10n.noApps,
            style: Theme.of(context).textTheme.bodyMedium?.copyWith(
              color: scheme.onSurfaceVariant.withValues(alpha: 0.5),
            ),
          ),
        ],
      ),
    );
  }
}
