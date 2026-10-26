import 'package:material_ui/material_ui.dart';

class AppTag extends StatelessWidget {
  const AppTag({
    super.key,
    required this.label,
    required this.color,
    this.backgroundOpacity = 0.15,
  });

  final String label;
  final Color color;
  final double backgroundOpacity;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: color.withValues(alpha: backgroundOpacity),
        borderRadius: BorderRadius.circular(50),
      ),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
        child: Text(
          label,
          style: Theme.of(context).textTheme.labelSmall?.copyWith(color: color),
        ),
      ),
    );
  }
}
