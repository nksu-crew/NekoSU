import 'package:material_ui/material_ui.dart';
import 'package:flutter_localizations/flutter_localizations.dart';
import 'package:nekosu/l10n/app_localizations.dart';

class ColorSchemeDiagnosticsPage extends StatelessWidget {
  const ColorSchemeDiagnosticsPage({super.key});

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final l10n = AppLocalizations.of(context)!;

    final roles = [
      ('surface', scheme.surface),
      ('surfaceContainerLowest', scheme.surfaceContainerLowest),
      ('surfaceContainerLow', scheme.surfaceContainerLow),
      ('surfaceContainer', scheme.surfaceContainer),
      ('surfaceContainerHigh', scheme.surfaceContainerHigh),
      ('surfaceContainerHighest', scheme.surfaceContainerHighest),
      ('onSurface', scheme.onSurface),
      ('primaryContainer', scheme.primaryContainer),
      ('secondaryContainer', scheme.secondaryContainer),
    ];

    for (final (name, color) in roles) {
      debugPrint(
        '$name: #${color.toARGB32().toRadixString(16).padLeft(8, '0').toUpperCase()}',
      );
    }

    return Scaffold(
      backgroundColor: scheme.surface,
      appBar: AppBar(
        title: Text(l10n.colorSchemeDiagnostics),
        backgroundColor: scheme.surfaceContainer,
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          for (final (name, color) in roles)
            _ColorTile(name: name, color: color),
        ],
      ),
    );
  }
}

class _ColorTile extends StatelessWidget {
  const _ColorTile({required this.name, required this.color});

  final String name;
  final Color color;

  @override
  Widget build(BuildContext context) {
    final hex =
        '#${color.toARGB32().toRadixString(16).padLeft(8, '0').toUpperCase()}';
    final luminance = color.computeLuminance();
    final textColor = luminance > 0.5 ? Colors.black : Colors.white;
    final textTheme = Theme.of(context).textTheme;

    return Container(
      margin: const EdgeInsets.only(bottom: 8),
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
      decoration: BoxDecoration(
        color: color,
        borderRadius: BorderRadius.circular(12),
      ),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: [
          Text(name, style: textTheme.labelLarge?.copyWith(color: textColor)),
          Text(hex, style: textTheme.bodySmall?.copyWith(color: textColor)),
        ],
      ),
    );
  }
}
