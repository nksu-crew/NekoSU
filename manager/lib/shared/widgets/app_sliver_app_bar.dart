import 'package:material_ui/material_ui.dart';

class AppSliverAppBar extends StatelessWidget {
  const AppSliverAppBar({super.key, this.title, this.leading, this.actions});

  final Widget? title;
  final Widget? leading;
  final List<Widget>? actions;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    return SliverAppBar(
      pinned: true,
      title: title,
      leading: leading,
      actions: actions,
      backgroundColor: scheme.surfaceContainer,
      surfaceTintColor: Colors.transparent,
      scrolledUnderElevation: 0,
      elevation: 0,
    );
  }
}
