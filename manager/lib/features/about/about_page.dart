import 'package:flutter/material.dart';
import 'package:nekosu/core/ncore_ffi.dart';
import 'package:nekosu/core/settings_controller.dart';
import 'package:nekosu/l10n/app_localizations.dart';
import 'package:nekosu/shared/app_feedback.dart';
import 'package:nekosu/shared/widgets/app_sliver_app_bar.dart';
import 'package:nekosu/shared/widgets/card_group.dart';
import 'package:nekosu/shared/widgets/section_card.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:url_launcher/url_launcher.dart';

class AboutPage extends StatefulWidget {
  const AboutPage({super.key});

  @override
  State<AboutPage> createState() => _AboutPageState();
}

class _AboutPageState extends State<AboutPage> {
  static const _repositoryUrl = 'https://github.com/nksu-crew/NekoSU';

  String _version = SettingsController.managerVersion;
  String _channel = 'release';

  @override
  void initState() {
    super.initState();
    _loadVersion();
  }

  Future<void> _loadVersion() async {
    try {
      final info = await PackageInfo.fromPlatform();
      if (!mounted) return;
      final buildNumber = info.buildNumber.trim();
      setState(() {
        _version = buildNumber.isEmpty
            ? info.version
            : '${info.version} ($buildNumber)';
        _channel = _channelOf(info.version);
      });
    } catch (_) {
      // Keep the bundled fallback version when platform metadata is unavailable.
    }
  }

  static String _channelOf(String version) {
    if (version.contains('-canary')) return 'canary';
    if (version.contains('-beta')) return 'beta';
    return 'release';
  }

  Future<void> _openRepository() async {
    final l10n = AppLocalizations.of(context)!;
    try {
      final opened = await launchUrl(
        Uri.parse(_repositoryUrl),
        mode: LaunchMode.externalApplication,
      );
      if (!opened) throw StateError('Unable to launch repository URL');
    } catch (_) {
      if (!mounted) return;
      showAppSnackBar(context, l10n.aboutOpenLinkFailed);
    }
  }

  void _openLicenses() {
    final l10n = AppLocalizations.of(context)!;
    showLicensePage(
      context: context,
      applicationName: l10n.appTitle,
      applicationVersion: _version,
      applicationIcon: Padding(
        padding: const EdgeInsets.all(8),
        child: Image.asset('assets/images/app_icon.webp', width: 48, height: 48),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final l10n = AppLocalizations.of(context)!;

    return Scaffold(
      backgroundColor: scheme.surfaceContainer,
      body: CustomScrollView(
        slivers: [
          AppSliverAppBar(leading: const BackButton(), title: Text(l10n.about)),
          SliverPadding(
            padding: EdgeInsets.only(
              bottom: 32 + MediaQuery.viewPaddingOf(context).bottom,
            ),
            sliver: SliverList(
              delegate: SliverChildListDelegate([
                const _AboutHero(),
                const SizedBox(height: 16),
                SectionCard(
                  title: l10n.aboutInformation,
                  children: [
                    _AboutInfoRow(
                      icon: Icons.info_outline_rounded,
                      title: l10n.aboutVersion,
                      value: _version,
                    ),
                    _AboutInfoRow(
                      icon: Icons.new_releases_outlined,
                      title: l10n.aboutChannel,
                      value: switch (_channel) {
                        'canary' => l10n.aboutChannelCanary,
                        'beta' => l10n.aboutChannelBeta,
                        _ => l10n.aboutChannelRelease,
                      },
                    ),
                    _AboutInfoRow(
                      icon: Icons.gavel_rounded,
                      title: l10n.aboutLicense,
                      value: l10n.aboutLicenseShort,
                    ),
                  ],
                ),
                const SizedBox(height: 24),
                SectionCard(
                  title: l10n.aboutRuntime,
                  children: [
                    _AboutInfoRow(
                      icon: Icons.memory_rounded,
                      title: l10n.kernelVersion,
                      value: NcoreFfi.kernelVersion ?? l10n.unknownValue,
                    ),
                    _AboutInfoRow(
                      icon: Icons.extension_rounded,
                      title: l10n.moduleVersion,
                      value: NcoreFfi.moduleVersion ?? l10n.unknownValue,
                    ),
                  ],
                ),
                const SizedBox(height: 24),
                SectionCard(
                  title: l10n.aboutProject,
                  children: [
                    ListRow(
                      icon: const Icon(Icons.code_rounded),
                      headline: Text(l10n.aboutRepository),
                      supporting: Text(l10n.aboutRepositorySummary),
                      trailing: const Icon(Icons.open_in_new_rounded),
                      onTap: _openRepository,
                    ),
                    ListRow(
                      icon: const Icon(Icons.description_outlined),
                      headline: Text(l10n.aboutOpenSource),
                      supporting: Text(l10n.aboutOpenSourceSummary),
                      trailing: const Icon(Icons.chevron_right_rounded),
                      onTap: _openLicenses,
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
}

class _AboutHero extends StatelessWidget {
  const _AboutHero();

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final textTheme = Theme.of(context).textTheme;
    final l10n = AppLocalizations.of(context)!;

    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 16),
      child: Column(
        children: [
          Image.asset('assets/images/app_icon.webp', width: 64, height: 64),
          const SizedBox(height: 16),
          Text(
            l10n.appTitle,
            textAlign: TextAlign.center,
            style: textTheme.headlineMedium?.copyWith(color: scheme.onSurface),
          ),
        ],
      ),
    );
  }
}

class _AboutInfoRow extends StatelessWidget {
  const _AboutInfoRow({
    required this.icon,
    required this.title,
    required this.value,
  });

  final IconData icon;
  final String title;
  final String value;

  @override
  Widget build(BuildContext context) {
    final textTheme = Theme.of(context).textTheme;
    final scheme = Theme.of(context).colorScheme;

    return ListRow(
      icon: Icon(icon),
      headline: Text(title),
      trailing: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 190),
        child: Text(
          value,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          textAlign: TextAlign.end,
          style: textTheme.labelLarge?.copyWith(color: scheme.onSurfaceVariant),
        ),
      ),
    );
  }
}
