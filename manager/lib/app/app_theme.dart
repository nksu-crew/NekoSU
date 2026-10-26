import 'dart:ui' show FontVariation;

import 'package:cupertino_ui/cupertino_ui.dart' show CupertinoPageTransitionsBuilder;
import 'package:material_ui/material_ui.dart';

abstract final class AppTheme {
  static const fontFamily = 'MI_Sans_Regular';

  static ThemeData build(ColorScheme scheme, {int fontWeight = 400}) {
    final textTheme = _applyFontWeight(_buildTextTheme(scheme), fontWeight);
    return ThemeData(
      useMaterial3: true,
      colorScheme: scheme,
      fontFamily: fontFamily,
      textTheme: textTheme,
      scaffoldBackgroundColor: scheme.surfaceContainer,
      appBarTheme: AppBarTheme(
        backgroundColor: scheme.surfaceContainer,
        surfaceTintColor: Colors.transparent,
        scrolledUnderElevation: 0,
        elevation: 0,
        titleTextStyle: textTheme.titleLarge?.copyWith(color: scheme.onSurface),
      ),
      pageTransitionsTheme: const PageTransitionsTheme(
        builders: {
          TargetPlatform.android: CupertinoPageTransitionsBuilder(),
          TargetPlatform.iOS: CupertinoPageTransitionsBuilder(),
          TargetPlatform.macOS: CupertinoPageTransitionsBuilder(),
          TargetPlatform.linux: FadeUpwardsPageTransitionsBuilder(),
          TargetPlatform.windows: FadeUpwardsPageTransitionsBuilder(),
        },
      ),
    );
  }

  static TextTheme _buildTextTheme(ColorScheme scheme) {
    final base = Typography.material2021(platform: TargetPlatform.android).black
        .apply(
          fontFamily: fontFamily,
          bodyColor: scheme.onSurface,
          displayColor: scheme.onSurface,
        );

    return base.copyWith(
      headlineSmall: base.headlineSmall?.copyWith(
        fontSize: 24,
        height: 1.33,
        fontWeight: FontWeight.w400,
        letterSpacing: 0,
      ),
      titleLarge: base.titleLarge?.copyWith(
        fontSize: 22,
        height: 1.27,
        fontWeight: FontWeight.w400,
        letterSpacing: 0,
      ),
      titleMedium: base.titleMedium?.copyWith(
        fontSize: 16,
        height: 1.5,
        fontWeight: FontWeight.w500,
        letterSpacing: 0,
      ),
      titleSmall: base.titleSmall?.copyWith(
        fontSize: 14,
        height: 1.43,
        fontWeight: FontWeight.w500,
        letterSpacing: 0,
      ),
      bodyLarge: base.bodyLarge?.copyWith(
        fontSize: 16,
        height: 1.5,
        fontWeight: FontWeight.w400,
        letterSpacing: 0,
      ),
      bodyMedium: base.bodyMedium?.copyWith(
        fontSize: 14,
        height: 1.43,
        fontWeight: FontWeight.w400,
        letterSpacing: 0,
      ),
      bodySmall: base.bodySmall?.copyWith(
        fontSize: 12,
        height: 1.33,
        fontWeight: FontWeight.w400,
        letterSpacing: 0,
      ),
      labelLarge: base.labelLarge?.copyWith(
        fontSize: 14,
        height: 1.43,
        fontWeight: FontWeight.w500,
        letterSpacing: 0,
      ),
      labelMedium: base.labelMedium?.copyWith(
        fontSize: 12,
        height: 1.33,
        fontWeight: FontWeight.w500,
        letterSpacing: 0,
      ),
      labelSmall: base.labelSmall?.copyWith(
        fontSize: 11,
        height: 1.45,
        fontWeight: FontWeight.w500,
        letterSpacing: 0,
      ),
    );
  }

  static TextTheme _applyFontWeight(TextTheme theme, int targetWeight) {
    TextStyle? adjust(TextStyle? style) {
      if (style == null) return null;
      final current = style.fontWeight?.value ?? FontWeight.w400.value;
      final adjusted = (current + targetWeight - 400).clamp(300, 700);
      return style.copyWith(
        fontWeight: FontWeight.values[(adjusted ~/ 100) - 1],
        fontVariations: [FontVariation('wght', adjusted.toDouble())],
      );
    }

    return theme.copyWith(
      displayLarge: adjust(theme.displayLarge),
      displayMedium: adjust(theme.displayMedium),
      displaySmall: adjust(theme.displaySmall),
      headlineLarge: adjust(theme.headlineLarge),
      headlineMedium: adjust(theme.headlineMedium),
      headlineSmall: adjust(theme.headlineSmall),
      titleLarge: adjust(theme.titleLarge),
      titleMedium: adjust(theme.titleMedium),
      titleSmall: adjust(theme.titleSmall),
      bodyLarge: adjust(theme.bodyLarge),
      bodyMedium: adjust(theme.bodyMedium),
      bodySmall: adjust(theme.bodySmall),
      labelLarge: adjust(theme.labelLarge),
      labelMedium: adjust(theme.labelMedium),
      labelSmall: adjust(theme.labelSmall),
    );
  }
}
