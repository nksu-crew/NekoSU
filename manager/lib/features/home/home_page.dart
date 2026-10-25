import 'dart:io';

import 'package:device_info_plus/device_info_plus.dart';
import 'package:flutter/material.dart';
import 'package:nekosu/core/ncore_ffi.dart';
import 'package:nekosu/data/install_status.dart';
import 'package:nekosu/features/about/about_page.dart';
import 'package:nekosu/features/install/install_page.dart';
import 'package:nekosu/l10n/app_localizations.dart';
import 'package:nekosu/shared/layout_insets.dart';
import 'package:nekosu/shared/widgets/app_sliver_app_bar.dart';
import 'package:nekosu/shared/widgets/card_group.dart';
import 'package:nekosu/shared/widgets/section_card.dart';

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> with WidgetsBindingObserver {
  static const _statusRepository = InstallStatusRepository();

  late InstallationState _installation;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _installation = _loadInstallation();
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) {
      _refreshStatus();
    }
  }

  void _refreshStatus() {
    final installation = _loadInstallation();
    if (!mounted || _installation == installation) return;
    setState(() => _installation = installation);
  }

  InstallationState _loadInstallation() {
    try {
      return _statusRepository.load();
    } catch (error) {
      debugPrint('Unable to refresh NekoSU installation state: $error');
      return const InstallationState.unknown();
    }
  }

  @override
  Widget build(BuildContext context) {
    return _HomeScreenContent(
      installation: _installation,
      onInstall: _openInstaller,
    );
  }

  Future<void> _openInstaller() async {
    await Navigator.of(
      context,
    ).push<bool>(MaterialPageRoute(builder: (context) => const InstallPage()));
    if (!mounted) return;
    _refreshStatus();
  }
}

class _HomeScreenContent extends StatelessWidget {
  const _HomeScreenContent({
    required this.installation,
    required this.onInstall,
  });

  final InstallationState installation;
  final VoidCallback onInstall;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final l10n = AppLocalizations.of(context)!;
    final navBarClearance = appNavBarClearance(context);

    return Scaffold(
      backgroundColor: scheme.surfaceContainer,
      body: CustomScrollView(
        slivers: [
          AppSliverAppBar(title: Text(l10n.appTitle)),
          SliverPadding(
            padding: EdgeInsets.fromLTRB(0, 12, 0, navBarClearance),
            sliver: SliverToBoxAdapter(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  Padding(
                    padding: const EdgeInsets.symmetric(horizontal: 16),
                    child: _StatusCard(
                      installation: installation,
                      onTap: onInstall,
                    ),
                  ),
                  const SizedBox(height: 13),
                  SectionLabel(
                    text: l10n.deviceAndKernel,
                    padding: const EdgeInsets.only(left: 16, top: 4),
                  ),
                  const SizedBox(height: 13),
                  const _DeviceInfoList(),
                  const SizedBox(height: 13),
                  SectionLabel(
                    text: l10n.about,
                    padding: const EdgeInsets.only(left: 16, top: 4),
                  ),
                  const SizedBox(height: 13),
                  const _AboutItem(),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _StatusCard extends StatelessWidget {
  const _StatusCard({required this.installation, required this.onTap});

  final InstallationState installation;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final textTheme = Theme.of(context).textTheme;
    final l10n = AppLocalizations.of(context)!;

    final status = installation.status;
    final isWorking = installation.isRunning;
    final isUpdate = status == InstallStatus.needUpdate;

    final containerColor = status == InstallStatus.unknown
        ? scheme.surfaceContainerHighest
        : isWorking
        ? scheme.secondaryContainer
        : isUpdate
        ? scheme.tertiaryContainer
        : scheme.errorContainer;
    final contentColor = status == InstallStatus.unknown
        ? scheme.onSurfaceVariant
        : isWorking
        ? scheme.onSecondaryContainer
        : isUpdate
        ? scheme.onTertiaryContainer
        : scheme.onErrorContainer;
    final icon = status == InstallStatus.unknown
        ? Icons.help_outline_rounded
        : isWorking
        ? Icons.check_circle_rounded
        : isUpdate
        ? Icons.system_update_rounded
        : Icons.warning_rounded;
    final title = status == InstallStatus.unknown
        ? l10n.unknownStatus
        : isWorking
        ? l10n.running
        : isUpdate
        ? l10n.needUpdate
        : l10n.notInstalled;
    final supportingText = isWorking
        ? installation.managerVersion
        : isUpdate
        ? l10n.clickToUpdate
        : status == InstallStatus.notInstalled
        ? l10n.clickToInstall
        : null;

    return AnimatedContainer(
      duration: const Duration(milliseconds: 300),
      curve: Curves.easeOutCubic,
      decoration: BoxDecoration(
        color: containerColor,
        borderRadius: BorderRadius.circular(28),
      ),
      clipBehavior: Clip.antiAlias,
      child: Material(
        color: Colors.transparent,
        child: InkWell(
          onTap: onTap,
          child: Padding(
            padding: const EdgeInsets.all(20),
            child: Row(
              children: [
                Icon(icon, color: contentColor, size: 32),
                const SizedBox(width: 16),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      Text(
                        title,
                        style: textTheme.titleMedium?.copyWith(
                          color: contentColor,
                        ),
                      ),
                      if (supportingText != null) ...[
                        const SizedBox(height: 3),
                        Text(
                          supportingText,
                          style: textTheme.bodyMedium?.copyWith(
                            color: contentColor.withValues(alpha: 0.8),
                          ),
                        ),
                      ],
                    ],
                  ),
                ),
                if (status != InstallStatus.unknown)
                  _ModeLabel(
                    label: installation.isGki ? 'GKI' : 'LKM',
                    color: contentColor,
                    containerColor: contentColor.withValues(alpha: 0.16),
                  ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _ModeLabel extends StatelessWidget {
  const _ModeLabel({
    required this.label,
    required this.color,
    required this.containerColor,
  });

  final String label;
  final Color color;
  final Color containerColor;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 4),
      decoration: BoxDecoration(
        color: containerColor,
        borderRadius: BorderRadius.circular(50),
      ),
      child: Text(
        label,
        maxLines: 1,
        style: Theme.of(context).textTheme.labelMedium?.copyWith(color: color),
      ),
    );
  }
}

class _DeviceInfoList extends StatefulWidget {
  const _DeviceInfoList();

  @override
  State<_DeviceInfoList> createState() => _DeviceInfoListState();
}

class _DeviceInfoListState extends State<_DeviceInfoList> {
  String _kernelVersion = '...';
  String _androidVersion = '...';
  String _model = '...';
  String _fingerprint = '...';

  @override
  void initState() {
    super.initState();
    _loadDeviceInfo();
  }

  Future<void> _loadDeviceInfo() async {
    final kernelVersion = await _loadKernelVersion();
    if (!Platform.isAndroid) {
      _setFallback(kernelVersion);
      return;
    }

    try {
      final androidInfo = await DeviceInfoPlugin().androidInfo;
      if (!mounted) return;
      setState(() {
        _kernelVersion = kernelVersion;
        _androidVersion = androidInfo.version.release;
        _model = '${androidInfo.manufacturer} ${androidInfo.model}';
        _fingerprint = androidInfo.fingerprint;
      });
    } catch (_) {
      _setFallback(kernelVersion);
    }
  }

  Future<String> _loadKernelVersion() async {
    if (!Platform.isAndroid) return Platform.operatingSystem;
    final release = NcoreFfi.kernelRelease;
    if (release != null && release.isNotEmpty) return release;
    try {
      final result = await Process.run('/system/bin/uname', const ['-r']);
      if (result.exitCode == 0) {
        final version = result.stdout.toString().trim();
        if (version.isNotEmpty) return version;
      }
    } catch (_) {}
    return 'Unknown';
  }

  void _setFallback(String kernelVersion) {
    if (!mounted) return;
    setState(() {
      _kernelVersion = kernelVersion;
      _androidVersion = Platform.operatingSystem;
      _model = 'Unknown';
      _fingerprint = 'Unknown';
    });
  }

  @override
  Widget build(BuildContext context) {
    final l10n = AppLocalizations.of(context)!;
    final entries = [
      _InfoEntry(label: l10n.kernelVersion, value: _kernelVersion),
      _InfoEntry(label: l10n.androidVersion, value: _androidVersion),
      _InfoEntry(label: l10n.deviceModel, value: _model),
      _InfoEntry(label: l10n.fingerprint, value: _fingerprint, monospace: true),
    ];

    return CardGroup(
      children: [for (final entry in entries) _DeviceInfoRow(entry: entry)],
    );
  }
}

class _InfoEntry {
  const _InfoEntry({
    required this.label,
    required this.value,
    this.monospace = false,
  });

  final String label;
  final String value;
  final bool monospace;
}

class _DeviceInfoRow extends StatelessWidget {
  const _DeviceInfoRow({required this.entry});

  final _InfoEntry entry;

  @override
  Widget build(BuildContext context) {
    return ListTile(
      title: Text(entry.label),
      subtitle: Text(
        entry.value,
        maxLines: 3,
        overflow: TextOverflow.ellipsis,
        style: entry.monospace
            ? const TextStyle(fontFamily: 'monospace')
            : null,
      ),
    );
  }
}

class _AboutItem extends StatelessWidget {
  const _AboutItem();

  @override
  Widget build(BuildContext context) {
    final l10n = AppLocalizations.of(context)!;

    return CardGroup(
      children: [
        ListRow(
          icon: const Icon(Icons.info_outline_rounded),
          headline: Text(l10n.aboutCardTitle),
          supporting: Text(
            l10n.aboutCardDescription,
            maxLines: 2,
            overflow: TextOverflow.ellipsis,
          ),
          trailing: const Icon(Icons.chevron_right_rounded),
          onTap: () => Navigator.of(context).push<void>(
            MaterialPageRoute(builder: (context) => const AboutPage()),
          ),
        ),
      ],
    );
  }
}
