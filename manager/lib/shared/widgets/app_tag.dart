import 'package:flutter/material.dart';

class AppTag extends StatelessWidget {
  const AppTag({
    super.key,
    required this.label,
    required this.color,
    this.backgroundOpacity = 0.15,
    this.monospace = false,
  });

  final String label;
  final Color color;
  final double backgroundOpacity;
  final bool monospace;

  @override
  Widget build(BuildContext context) {
    final baseStyle = Theme.of(context).textTheme.labelSmall
        ?.copyWith(color: color);

    return DecoratedBox(
      decoration: BoxDecoration(
        color: color.withValues(alpha: backgroundOpacity),
        borderRadius: BorderRadius.circular(50),
      ),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
        child: Text(
          label,
          style: monospace
              ? baseStyle?.copyWith(fontFamily: FontFamily.monospace)
              : baseStyle,
        ),
      ),
    );
  }
}
