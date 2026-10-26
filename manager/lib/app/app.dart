import 'package:dynamic_color/dynamic_color.dart';
import 'package:material_ui/material_ui.dart';
import 'package:nekosu/app/app_shell.dart';
import 'package:nekosu/app/app_theme.dart';
import 'package:nekosu/core/settings_controller.dart';
import 'package:nekosu/l10n/app_localizations.dart';

class MyApp extends StatefulWidget {
  const MyApp({super.key});

  @override
  State<MyApp> createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  @override
  void initState() {
    super.initState();
    SettingsController.instance.addListener(_settingsChanged);
  }

  @override
  void dispose() {
    SettingsController.instance.removeListener(_settingsChanged);
    super.dispose();
  }

  void _settingsChanged() {
    if (mounted) setState(() {});
  }

  @override
  Widget build(BuildContext context) {
    final settings = SettingsController.instance;
    return DynamicColorBuilder(
      builder: (lightDynamic, darkDynamic) {
        final lightSeed = settings.usesDynamicColor
            ? lightDynamic?.primary ?? SettingsController.fallbackSeed
            : settings.seedColor;
        final darkSeed = settings.usesDynamicColor
            ? darkDynamic?.primary ?? SettingsController.fallbackSeed
            : settings.seedColor;
        final lightScheme = ColorScheme.fromSeed(
          seedColor: lightSeed,
          brightness: Brightness.light,
        );
        final darkBase = ColorScheme.fromSeed(
          seedColor: darkSeed,
          brightness: Brightness.dark,
        );
        final darkScheme = settings.amoled
            ? darkBase.copyWith(
                surface: Colors.black,
                surfaceContainer: Colors.black,
              )
            : darkBase;

        return MaterialApp(
          onGenerateTitle: (context) => AppLocalizations.of(context)!.appTitle,
          localizationsDelegates: const <LocalizationsDelegate<dynamic>>[
            AppLocalizations.delegate,
            ...GlobalMaterialLocalizations.delegates,
          ],
          supportedLocales: const [Locale('en', ''), Locale('zh', '')],
          locale: settings.locale,
          theme: AppTheme.build(
            lightScheme,
            fontWeight: settings.fontWeight.value,
          ),
          darkTheme: AppTheme.build(
            darkScheme,
            fontWeight: settings.fontWeight.value,
          ),
          themeMode: settings.flutterThemeMode,
          builder: (context, child) =>
              // Legacy third-party widgets (for example liquid_glass_widgets
              // and flutter_markdown_plus) still read Theme and Material
              // localizations from package:flutter/material.dart.
              // ignore: deprecated_member_use
              MaterialUiCompatibilityBridge(
                child: Material(
                  type: MaterialType.transparency,
                  child: child ?? const SizedBox.shrink(),
                ),
              ),
          home: const AppShell(),
        );
      },
    );
  }
}
