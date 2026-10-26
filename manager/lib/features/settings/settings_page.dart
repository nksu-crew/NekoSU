import 'package:material_ui/material_ui.dart';
import 'package:flutter/services.dart';
import 'package:nekosu/core/log_export.dart';
import 'package:nekosu/core/settings_controller.dart';
import 'package:nekosu/data/kernel_features.dart';
import 'package:nekosu/features/about/about_page.dart';
import 'package:nekosu/features/settings/display_mode_page.dart';
import 'package:nekosu/l10n/app_localizations.dart';
import 'package:nekosu/shared/app_feedback.dart';
import 'package:nekosu/shared/layout_insets.dart';
import 'package:nekosu/shared/widgets/app_popup_menu.dart';
import 'package:nekosu/shared/widgets/app_sliver_app_bar.dart';
import 'package:nekosu/shared/widgets/card_group.dart';
import 'package:nekosu/shared/widgets/section_card.dart';

class SettingsPage extends StatelessWidget {
  const SettingsPage({super.key});

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: SettingsController.instance,
      builder: (context, _) {
        final l10n = AppLocalizations.of(context)!;
        final settings = SettingsController.instance;
        final navBarClearance = appNavBarClearance(context);

        return Scaffold(
          body: CustomScrollView(
            slivers: [
              AppSliverAppBar(title: Text(l10n.settingsLabel)),
              SliverPadding(
                padding: EdgeInsets.fromLTRB(0, 12, 0, navBarClearance),
                sliver: SliverList(
                  delegate: SliverChildListDelegate([
                    SectionCard(
                      title: l10n.settingsGeneral,
                      children: [
                        _SettingsChoice<AppThemeMode>(
                          value: settings.themeMode,
                          choices: [
                            _SettingsChoiceValue(
                              AppThemeMode.system,
                              l10n.themeSystem,
                            ),
                            _SettingsChoiceValue(
                              AppThemeMode.light,
                              l10n.themeLight,
                            ),
                            _SettingsChoiceValue(
                              AppThemeMode.dark,
                              l10n.themeDark,
                            ),
                          ],
                          onChanged: settings.setThemeMode,
                          child: ListRow(
                            icon: const Icon(Icons.brightness_6_outlined),
                            headline: Text(l10n.themeTitle),
                            supporting: Text(_themeModeLabel(settings, l10n)),
                            trailing: const Icon(Icons.chevron_right_rounded),
                          ),
                        ),
                        _SettingsChoice<String>(
                          value: settings.language,
                          choices: [
                            _SettingsChoiceValue('system', l10n.languageSystem),
                            _SettingsChoiceValue('en', l10n.languageEnglish),
                            _SettingsChoiceValue('zh', l10n.languageChinese),
                          ],
                          onChanged: settings.setLanguage,
                          child: ListRow(
                            icon: const Icon(Icons.translate_rounded),
                            headline: Text(l10n.settingsLanguage),
                            supporting: Text(
                              _languageLabel(settings.language, l10n),
                            ),
                            trailing: const Icon(Icons.chevron_right_rounded),
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 20),
                    SectionCard(
                      title: l10n.appearance,
                      children: [
                        _SettingsChoice<AppThemeAccent>(
                          value: settings.themeAccent,
                          choices: [
                            _SettingsChoiceValue(
                              AppThemeAccent.materialYou,
                              l10n.themeColorMaterialYou,
                            ),
                            _SettingsChoiceValue(
                              AppThemeAccent.catppuccinBlue,
                              l10n.themeColorBlue,
                            ),
                            _SettingsChoiceValue(
                              AppThemeAccent.catppuccinLavender,
                              l10n.themeColorLavender,
                            ),
                            _SettingsChoiceValue(
                              AppThemeAccent.catppuccinGreen,
                              l10n.themeColorGreen,
                            ),
                          ],
                          onChanged: settings.setThemeAccent,
                          child: ListRow(
                            icon: const Icon(Icons.palette_outlined),
                            headline: Text(l10n.settingsThemeColor),
                            supporting: Text(
                              _accentLabel(settings.themeAccent, l10n),
                            ),
                            trailing: const Icon(Icons.chevron_right_rounded),
                          ),
                        ),
                        _SettingsChoice<AppFontWeight>(
                          value: settings.fontWeight,
                          choices: [
                            _SettingsChoiceValue(
                              AppFontWeight.light,
                              l10n.fontWeightLight,
                            ),
                            _SettingsChoiceValue(
                              AppFontWeight.regular,
                              l10n.fontWeightRegular,
                            ),
                            _SettingsChoiceValue(
                              AppFontWeight.medium,
                              l10n.fontWeightMedium,
                            ),
                            _SettingsChoiceValue(
                              AppFontWeight.bold,
                              l10n.fontWeightBold,
                            ),
                          ],
                          onChanged: settings.setFontWeight,
                          child: ListRow(
                            icon: const Icon(Icons.format_size_rounded),
                            headline: Text(l10n.settingsFontWeight),
                            supporting: Text(
                              _fontWeightLabel(settings.fontWeight, l10n),
                            ),
                            trailing: const Icon(Icons.chevron_right_rounded),
                          ),
                        ),
                        ListRow(
                          icon: const Icon(Icons.speed_rounded),
                          headline: Text(l10n.settingsDisplayMode),
                          supporting: Text(l10n.settingsDisplayModeSummary),
                          trailing: const Icon(Icons.chevron_right_rounded),
                          onTap: () => Navigator.of(context).push<void>(
                            MaterialPageRoute(
                              builder: (context) => const DisplayModePage(),
                            ),
                          ),
                        ),
                        ListRow(
                          icon: const Icon(Icons.phone_android_rounded),
                          headline: Text(l10n.settingsAmoled),
                          supporting: Text(l10n.settingsAmoledSummary),
                          trailing: Switch(
                            value: settings.amoled,
                            onChanged: settings.setAmoled,
                          ),
                        ),
                        _SettingsChoice<NavBarStyle>(
                          value: settings.navBarStyle,
                          choices: [
                            _SettingsChoiceValue(
                              NavBarStyle.standard,
                              l10n.navBarStyleStandard,
                            ),
                            _SettingsChoiceValue(
                              NavBarStyle.floating,
                              l10n.navBarStyleFloating,
                            ),
                            _SettingsChoiceValue(
                              NavBarStyle.glass,
                              l10n.navBarStyleGlass,
                            ),
                          ],
                          onChanged: settings.setNavBarStyle,
                          child: ListRow(
                            icon: const Icon(Icons.space_bar_rounded),
                            headline: Text(l10n.settingsNavBarStyle),
                            supporting: Text(
                              _navBarStyleLabel(settings.navBarStyle, l10n),
                            ),
                            trailing: const Icon(Icons.chevron_right_rounded),
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 20),
                    const _KernelFeatureSection(),
                    const SizedBox(height: 20),
                    SectionCard(
                      title: l10n.settingsAbout,
                      children: [
                        ListRow(
                          icon: const Icon(Icons.info_outline_rounded),
                          headline: Text(l10n.about),
                          supporting: Text(l10n.settingsAboutDescription),
                          onTap: () => Navigator.of(context).push<void>(
                            MaterialPageRoute(
                              builder: (context) => const AboutPage(),
                            ),
                          ),
                        ),
                        const _ExportLogsRow(),
                      ],
                    ),
                  ]),
                ),
              ),
            ],
          ),
        );
      },
    );
  }

  static String _themeModeLabel(
    SettingsController settings,
    AppLocalizations l10n,
  ) {
    return switch (settings.themeMode) {
      AppThemeMode.system => l10n.themeSystem,
      AppThemeMode.light => l10n.themeLight,
      AppThemeMode.dark => l10n.themeDark,
    };
  }

  static String _accentLabel(AppThemeAccent accent, AppLocalizations l10n) {
    return switch (accent) {
      AppThemeAccent.materialYou => l10n.themeColorMaterialYou,
      AppThemeAccent.catppuccinBlue => l10n.themeColorBlue,
      AppThemeAccent.catppuccinLavender => l10n.themeColorLavender,
      AppThemeAccent.catppuccinGreen => l10n.themeColorGreen,
    };
  }

  static String _fontWeightLabel(
    AppFontWeight fontWeight,
    AppLocalizations l10n,
  ) {
    return switch (fontWeight) {
      AppFontWeight.light => l10n.fontWeightLight,
      AppFontWeight.regular => l10n.fontWeightRegular,
      AppFontWeight.medium => l10n.fontWeightMedium,
      AppFontWeight.bold => l10n.fontWeightBold,
    };
  }

  static String _languageLabel(String language, AppLocalizations l10n) {
    return switch (language) {
      'en' => l10n.languageEnglish,
      'zh' => l10n.languageChinese,
      _ => l10n.languageSystem,
    };
  }

  static String _navBarStyleLabel(NavBarStyle style, AppLocalizations l10n) {
    return switch (style) {
      NavBarStyle.standard => l10n.navBarStyleStandard,
      NavBarStyle.floating => l10n.navBarStyleFloating,
      NavBarStyle.glass => l10n.navBarStyleGlass,
    };
  }
}

class _KernelFeatureSection extends StatefulWidget {
  const _KernelFeatureSection();

  @override
  State<_KernelFeatureSection> createState() => _KernelFeatureSectionState();
}

class _KernelFeatureSectionState extends State<_KernelFeatureSection> {
  List<KernelFeatureInfo> _features = const [];
  bool _loading = true;
  bool _updating = false;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final features = await KernelFeatureBridge.list();
    if (!mounted) return;
    setState(() {
      if (features != null) _features = features;
      _loading = false;
    });
  }

  Future<void> _setEnabled(KernelFeatureInfo feature, bool enabled) async {
    if (_updating) return;
    setState(() => _updating = true);
    final succeeded = await KernelFeatureBridge.setEnabled(feature.id, enabled);
    if (!mounted) return;
    if (succeeded) {
      await _load();
    } else {
      final l10n = AppLocalizations.of(context)!;
      showAppSnackBar(context, l10n.featureUpdateFailed);
    }
    if (mounted) setState(() => _updating = false);
  }

  @override
  Widget build(BuildContext context) {
    final l10n = AppLocalizations.of(context)!;
    return SectionCard(
      title: l10n.featuresTitle,
      children: _features.isEmpty
          ? [
              ListRow(
                icon: const Icon(Icons.tune_rounded),
                headline: Text(l10n.featureEmpty),
                supporting: Text(l10n.featureEmptyDescription),
                trailing: _loading
                    ? const SizedBox.square(
                        dimension: 20,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    : const SizedBox.shrink(),
              ),
            ]
          : [
              for (final feature in _features)
                ListRow(
                  icon: Icon(_featureIcon(feature.id)),
                  headline: Text(_featureLabel(feature, l10n)),
                  supporting: Text(_featureSummary(feature, l10n)),
                  trailing: Switch(
                    value: feature.enabled,
                    onChanged: _updating
                        ? null
                        : (enabled) => _setEnabled(feature, enabled),
                  ),
                ),
            ],
    );
  }
}

IconData _featureIcon(int id) {
  return switch (id) {
    KernelFeatureBridge.selinuxHide => Icons.security_rounded,
    KernelFeatureBridge.umount => Icons.visibility_off_rounded,
    _ => Icons.tune_rounded,
  };
}

String _featureLabel(KernelFeatureInfo feature, AppLocalizations l10n) {
  return switch (feature.id) {
    KernelFeatureBridge.selinuxHide => l10n.featureSelinuxHide,
    KernelFeatureBridge.umount => l10n.featureUmount,
    _ => feature.name,
  };
}

String _featureSummary(KernelFeatureInfo feature, AppLocalizations l10n) {
  return switch (feature.id) {
    KernelFeatureBridge.selinuxHide => l10n.featureSelinuxHideDescription,
    KernelFeatureBridge.umount => l10n.featureUmountDescription,
    _ => feature.name,
  };
}

class _ExportLogsRow extends StatefulWidget {
  const _ExportLogsRow();

  @override
  State<_ExportLogsRow> createState() => _ExportLogsRowState();
}

class _ExportLogsRowState extends State<_ExportLogsRow> {
  bool _exporting = false;

  Future<void> _export() async {
    if (_exporting) return;
    final l10n = AppLocalizations.of(context)!;
    setState(() => _exporting = true);
    try {
      await LogExport.export(title: l10n.settingsExportLogs);
      if (!mounted) return;
      showAppSnackBar(context, l10n.settingsExportLogsSuccess);
    } on PlatformException catch (error) {
      showAppSnackBar(context, error.message ?? l10n.settingsExportLogsFailed);
    } on MissingPluginException {
      showAppSnackBar(context, l10n.settingsExportLogsFailed);
    } catch (_) {
      showAppSnackBar(context, l10n.settingsExportLogsFailed);
    } finally {
      if (mounted) setState(() => _exporting = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final l10n = AppLocalizations.of(context)!;
    return ListRow(
      icon: const Icon(Icons.file_upload_outlined),
      headline: Text(l10n.settingsExportLogs),
      supporting: Text(l10n.settingsExportLogsSummary),
      onTap: _exporting ? null : _export,
    );
  }
}

class _SettingsChoiceValue<T> {
  const _SettingsChoiceValue(this.value, this.label);

  final T value;
  final String label;
}

class _SettingsChoice<T> extends StatelessWidget {
  const _SettingsChoice({
    required this.value,
    required this.choices,
    required this.onChanged,
    required this.child,
  });

  final T value;
  final List<_SettingsChoiceValue<T>> choices;
  final ValueChanged<T> onChanged;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    return AppPopupMenu<T>(
      onSelected: onChanged,
      items: [
        for (final choice in choices)
          AppPopupMenuItem<T>(
            value: choice.value,
            label: choice.label,
            selected: choice.value == value,
          ),
      ],
      child: child,
    );
  }
}
