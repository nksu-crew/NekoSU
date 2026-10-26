import 'package:material_ui/material_ui.dart';
import 'package:nekosu/core/settings_store.dart';

enum AppThemeMode { system, light, dark }

enum AppThemeAccent {
  materialYou,
  catppuccinBlue,
  catppuccinLavender,
  catppuccinGreen,
}

enum AppFontWeight {
  light(300),
  regular(400),
  medium(500),
  bold(700);

  const AppFontWeight(this.value);

  final int value;
}

enum NavBarStyle { standard, floating, glass }

class SettingsController extends ChangeNotifier {
  SettingsController._();

  static final SettingsController instance = SettingsController._();

  static const _themeModeKey = 'theme_mode';
  static const _themeAccentKey = 'theme_color';
  static const _fontWeightKey = 'font_weight';
  static const _amoledKey = 'amoled_mode';
  static const _navBarStyleKey = 'nav_bar_style';
  static const _languageKey = 'language';
  static const _displayModeKey = 'display_mode';

  static const managerVersion = '1.0.0';
  static const fallbackSeed = Colors.teal;

  /// Keys owned by this controller; anything else in the Hive box is stale.
  static const persistedKeys = <String>{
    _themeModeKey,
    _themeAccentKey,
    _fontWeightKey,
    _amoledKey,
    _navBarStyleKey,
    _languageKey,
    _displayModeKey,
  };

  AppThemeMode themeMode = AppThemeMode.system;
  AppThemeAccent themeAccent = AppThemeAccent.materialYou;
  AppFontWeight fontWeight = AppFontWeight.regular;
  bool amoled = false;
  NavBarStyle navBarStyle = NavBarStyle.standard;
  String language = 'system';
  String? displayMode;

  ThemeMode get flutterThemeMode => switch (themeMode) {
    AppThemeMode.system => ThemeMode.system,
    AppThemeMode.light => ThemeMode.light,
    AppThemeMode.dark => ThemeMode.dark,
  };

  Locale? get locale => language == 'system' ? null : Locale(language);

  bool get usesDynamicColor => themeAccent == AppThemeAccent.materialYou;

  Color get seedColor => switch (themeAccent) {
    AppThemeAccent.materialYou => fallbackSeed,
    AppThemeAccent.catppuccinBlue => const Color(0xFF89B4FA),
    AppThemeAccent.catppuccinLavender => const Color(0xFFB4BEFE),
    AppThemeAccent.catppuccinGreen => const Color(0xFFA6E3A1),
  };

  Future<void> load() async {
    try {
      await SettingsStore.prune(persistedKeys);
      themeMode = _themeModeFromIndex(SettingsStore.read<int>(_themeModeKey));
      themeAccent = _accentFromIndex(SettingsStore.read<int>(_themeAccentKey));
      fontWeight = _fontWeightFromIndex(
        SettingsStore.read<int>(_fontWeightKey),
      );
      amoled = SettingsStore.read<bool>(_amoledKey) == true;
      final savedNavBarStyle = SettingsStore.read<String>(_navBarStyleKey);
      navBarStyle = NavBarStyle.values.firstWhere(
        (style) => style.name == savedNavBarStyle,
        orElse: () => NavBarStyle.standard,
      );
      displayMode = SettingsStore.read<String>(_displayModeKey);
      final savedLanguage = SettingsStore.read<String>(_languageKey);
      if (savedLanguage == 'en' || savedLanguage == 'zh') {
        language = savedLanguage!;
      }
      notifyListeners();
    } catch (error) {
      debugPrint('Unable to load settings: $error');
    }
  }

  Future<void> setThemeMode(AppThemeMode value) async {
    if (themeMode == value) return;
    themeMode = value;
    notifyListeners();
    await _save(_themeModeKey, value.index);
  }

  Future<void> setThemeAccent(AppThemeAccent value) async {
    if (themeAccent == value) return;
    themeAccent = value;
    notifyListeners();
    await _save(_themeAccentKey, value.index);
  }

  Future<void> setAmoled(bool value) async {
    if (amoled == value) return;
    amoled = value;
    notifyListeners();
    await _save(_amoledKey, value);
  }

  Future<void> setFontWeight(AppFontWeight value) async {
    if (fontWeight == value) return;
    fontWeight = value;
    notifyListeners();
    await _save(_fontWeightKey, value.index);
  }

  Future<void> setNavBarStyle(NavBarStyle value) async {
    if (navBarStyle == value) return;
    navBarStyle = value;
    notifyListeners();
    await _save(_navBarStyleKey, value.name);
  }

  Future<void> setLanguage(String value) async {
    if (language == value) return;
    language = value;
    notifyListeners();
    await _save(_languageKey, value);
  }

  Future<void> setDisplayMode(String value) async {
    if (displayMode == value) return;
    displayMode = value;
    notifyListeners();
    await _save(_displayModeKey, value);
  }

  Future<void> _save(String key, Object value) async {
    try {
      await SettingsStore.write(key, value);
    } catch (error) {
      debugPrint('Unable to save setting $key: $error');
    }
  }

  static AppThemeMode _themeModeFromIndex(Object? value) {
    final index = value is num ? value.toInt() : 0;
    return index >= 0 && index < AppThemeMode.values.length
        ? AppThemeMode.values[index]
        : AppThemeMode.system;
  }

  static AppThemeAccent _accentFromIndex(Object? value) {
    final index = value is num ? value.toInt() : 0;
    return index >= 0 && index < AppThemeAccent.values.length
        ? AppThemeAccent.values[index]
        : AppThemeAccent.materialYou;
  }

  static AppFontWeight _fontWeightFromIndex(Object? value) {
    final index = value is num ? value.toInt() : AppFontWeight.regular.index;
    return index >= 0 && index < AppFontWeight.values.length
        ? AppFontWeight.values[index]
        : AppFontWeight.regular;
  }
}
