import 'package:material_ui/material_ui.dart';
import 'package:nekosu/shared/widgets/card_group.dart';

class SectionLabel extends StatelessWidget {
  const SectionLabel({
    super.key,
    required this.text,
    this.padding = const EdgeInsets.fromLTRB(16, 0, 16, 8),
  });

  final String text;
  final EdgeInsetsGeometry padding;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: padding,
      child: Text(
        text,
        style: Theme.of(context).textTheme.titleSmall
            ?.copyWith(color: Theme.of(context).colorScheme.primary),
      ),
    );
  }
}

class SectionCard extends StatelessWidget {
  const SectionCard({super.key, required this.title, required this.children});

  final String title;
  final List<Widget> children;

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        SectionLabel(text: title),
        CardGroup(children: children),
      ],
    );
  }
}
