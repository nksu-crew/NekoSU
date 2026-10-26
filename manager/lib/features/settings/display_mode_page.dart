import 'package:material_ui/material_ui.dart';
import 'package:flutter_displaymode/flutter_displaymode.dart';
import 'package:nekosu/core/display_mode_controller.dart';
import 'package:nekosu/l10n/app_localizations.dart';
import 'package:nekosu/shared/app_feedback.dart';
import 'package:nekosu/shared/widgets/app_sliver_app_bar.dart';
import 'package:nekosu/shared/widgets/app_status_view.dart';
import 'package:nekosu/shared/widgets/card_group.dart';

class DisplayModePage extends StatefulWidget {
  const DisplayModePage({super.key});

  @override
  State<DisplayModePage> createState() => _DisplayModePageState();
}

class _DisplayModePageState extends State<DisplayModePage> {
  List<DisplayMode> _modes = const [];
  DisplayMode? _preferred;
  DisplayMode? _active;
  bool _loading = true;
  bool _applying = false;
  int? _applyingModeId;
  String? _error;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    if (mounted) {
      setState(() {
        _loading = true;
        _error = null;
      });
    }

    try {
      final modes = await FlutterDisplayMode.supported;
      final preferred = await FlutterDisplayMode.preferred;
      final active = await FlutterDisplayMode.active;
      if (!mounted) return;
      setState(() {
        _modes = modes;
        _preferred = preferred;
        _active = active;
        _loading = false;
      });
    } catch (_) {
      _showLoadError();
    }
  }

  void _showLoadError() {
    if (!mounted) return;
    setState(() {
      _loading = false;
      _error = AppLocalizations.of(context)!.displayModeLoadFailed;
    });
  }

  Future<void> _select(DisplayMode mode) async {
    if (_applying || mode.id == _preferred?.id) return;
    final l10n = AppLocalizations.of(context)!;
    setState(() {
      _applying = true;
      _applyingModeId = mode.id;
    });
    try {
      await DisplayModeController.apply(mode);
      if (!mounted) return;
      setState(() {
        _preferred = mode;
        _applying = false;
        _applyingModeId = null;
      });
    } catch (_) {
      if (!mounted) return;
      setState(() {
        _applying = false;
        _applyingModeId = null;
      });
      showAppSnackBar(context, l10n.displayModeApplyFailed);
      return;
    }

    try {
      await Future<void>.delayed(const Duration(milliseconds: 100));
      final active = await FlutterDisplayMode.active;
      if (mounted) setState(() => _active = active);
    } catch (_) {
      // The applied preference is already reflected; active mode is advisory.
    }
  }

  String _refreshRateLabel(DisplayMode mode) {
    final rate = mode.refreshRate;
    final rounded = rate.roundToDouble();
    final value = (rate - rounded).abs() < 0.05
        ? rounded.toInt().toString()
        : rate.toStringAsFixed(1);
    return '$value Hz';
  }

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final l10n = AppLocalizations.of(context)!;

    return Scaffold(
      backgroundColor: scheme.surfaceContainer,
      body: CustomScrollView(
        slivers: [
          AppSliverAppBar(
            leading: const BackButton(),
            title: Text(l10n.settingsDisplayMode),
          ),
          if (_loading)
            const SliverFillRemaining(
              hasScrollBody: false,
              child: AppLoadingView(),
            )
          else if (_error != null)
            SliverFillRemaining(
              hasScrollBody: false,
              child: AppErrorView(
                message: _error!,
                retryLabel: l10n.retry,
                onRetry: _load,
              ),
            )
          else
            SliverPadding(
              padding: EdgeInsets.fromLTRB(
                0,
                8,
                0,
                32 + MediaQuery.viewPaddingOf(context).bottom,
              ),
              sliver: SliverList(
                delegate: SliverChildListDelegate([
                  Padding(
                    padding: const EdgeInsets.fromLTRB(16, 0, 16, 12),
                    child: Text(
                      l10n.displayModeHint,
                      style: Theme.of(context).textTheme.bodySmall
                          ?.copyWith(color: scheme.outline),
                    ),
                  ),
                  CardGroup(
                    children: [
                      for (final mode in _modes)
                        _DisplayModeRow(
                          title: mode.id == DisplayMode.auto.id
                              ? l10n.displayModeAuto
                              : _refreshRateLabel(mode),
                          subtitle: mode.id == DisplayMode.auto.id
                              ? l10n.displayModeAutoSummary
                              : _modeDetails(mode, l10n),
                          selected: mode.id == _preferred?.id,
                          applying: mode.id == _applyingModeId,
                          enabled: !_applying,
                          onTap: () => _select(mode),
                        ),
                    ],
                  ),
                ]),
              ),
            ),
        ],
      ),
    );
  }

  String _modeDetails(DisplayMode mode, AppLocalizations l10n) {
    final resolution = '${mode.width} × ${mode.height}';
    return mode.id == _active?.id
        ? '$resolution · ${l10n.displayModeSystemActive}'
        : resolution;
  }
}

class _DisplayModeRow extends StatelessWidget {
  const _DisplayModeRow({
    required this.title,
    required this.subtitle,
    required this.selected,
    required this.applying,
    required this.enabled,
    required this.onTap,
  });

  final String title;
  final String subtitle;
  final bool selected;
  final bool applying;
  final bool enabled;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;

    return ListRow(
      icon: const Icon(Icons.speed_rounded),
      headline: Text(title),
      supporting: Text(subtitle),
      trailing: applying
          ? const SizedBox.square(
              dimension: 18,
              child: CircularProgressIndicator(strokeWidth: 2),
            )
          : Icon(
              selected
                  ? Icons.check_rounded
                  : Icons.radio_button_unchecked_rounded,
              color: selected ? scheme.primary : scheme.onSurfaceVariant,
            ),
      onTap: enabled ? onTap : null,
    );
  }
}
