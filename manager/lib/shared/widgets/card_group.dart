import 'package:flutter/material.dart';

const double cardGroupOuterRadius = 16;
const double cardGroupInnerRadius = 4;
const double cardGroupGap = 2;

const Duration cardGroupMotionDuration = Duration(milliseconds: 250);
const Curve cardGroupMotionCurve = Curves.easeInOutCubic;

BorderRadius groupShape(
  int index,
  int total, {
  double r = cardGroupOuterRadius,
}) {
  if (total <= 1) return BorderRadius.circular(r);
  if (index == 0) {
    return BorderRadius.only(
      topLeft: Radius.circular(r),
      topRight: Radius.circular(r),
    );
  }
  if (index == total - 1) {
    return BorderRadius.only(
      bottomLeft: Radius.circular(r),
      bottomRight: Radius.circular(r),
    );
  }
  return BorderRadius.zero;
}

class ListRow extends StatelessWidget {
  final Widget icon;
  final Widget headline;
  final Widget? supporting;
  final Widget trailing;
  final VoidCallback? onTap;

  const ListRow({
    super.key,
    required this.icon,
    required this.headline,
    this.supporting,
    this.trailing = const SizedBox.shrink(),
    this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    final secondaryColor = scheme.onSurfaceVariant;
    final pressReporter = onTap == null
        ? null
        : CardItem.pressReporterOf(context);

    return InkWell(
      onTap: onTap,
      onHighlightChanged: pressReporter,
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
        child: ConstrainedBox(
          constraints: const BoxConstraints(minHeight: 32),
          child: Row(
            children: [
              IconTheme.merge(
                data: IconThemeData(color: secondaryColor),
                child: icon,
              ),
              const SizedBox(width: 16),
              Expanded(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    DefaultTextStyle.merge(
                      style: theme.textTheme.bodyLarge?.copyWith(
                        color: scheme.onSurface,
                      ),
                      child: headline,
                    ),
                    if (supporting != null) ...[
                      const SizedBox(height: 2),
                      DefaultTextStyle.merge(
                        style: theme.textTheme.bodySmall?.copyWith(
                          color: secondaryColor,
                        ),
                        child: supporting!,
                      ),
                    ],
                  ],
                ),
              ),
              const SizedBox(width: 8),
              IconTheme.merge(
                data: IconThemeData(color: secondaryColor),
                child: trailing,
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class CardGroup extends StatelessWidget {
  final List<Widget> children;

  const CardGroup({super.key, required this.children});

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 16),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          for (int index = 0; index < children.length; index++) ...[
            if (index > 0) const SizedBox(height: cardGroupGap),
            CardItem(
              index: index,
              total: children.length,
              child: children[index],
            ),
          ],
        ],
      ),
    );
  }
}

class CardItem extends StatefulWidget {
  final int index;
  final int total;
  final Widget child;

  const CardItem({
    super.key,
    required this.index,
    required this.total,
    required this.child,
  });

  /// Forward a child InkWell's onHighlightChanged here.
  static ValueChanged<bool>? pressReporterOf(BuildContext context) {
    return context
        .dependOnInheritedWidgetOfExactType<_CardItemScope>()
        ?.onPressChanged;
  }

  @override
  State<CardItem> createState() => _CardItemState();
}

class _CardItemState extends State<CardItem> {
  bool _pressed = false;

  void _reportPress(bool pressed) {
    if (_pressed == pressed) return;
    setState(() => _pressed = pressed);
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    final outerRadius = BorderRadius.circular(cardGroupOuterRadius);
    final shape = _pressed
        ? outerRadius
        : BorderRadius.vertical(
            top: Radius.circular(
              widget.index == 0 ? cardGroupOuterRadius : cardGroupInnerRadius,
            ),
            bottom: Radius.circular(
              widget.index == widget.total - 1
                  ? cardGroupOuterRadius
                  : cardGroupInnerRadius,
            ),
          );

    return AnimatedContainer(
      duration: MediaQuery.disableAnimationsOf(context)
          ? Duration.zero
          : cardGroupMotionDuration,
      curve: cardGroupMotionCurve,
      clipBehavior: Clip.antiAlias,
      decoration: BoxDecoration(
        color: scheme.surfaceBright,
        borderRadius: shape,
      ),
      child: Material(
        type: MaterialType.transparency,
        child: ListTileTheme(
          data: theme.listTileTheme.copyWith(
            contentPadding: const EdgeInsets.symmetric(
              horizontal: 16,
              vertical: 4,
            ),
            titleTextStyle: theme.textTheme.bodyLarge?.copyWith(
              color: scheme.onSurface,
            ),
            subtitleTextStyle: theme.textTheme.bodySmall?.copyWith(
              color: scheme.onSurfaceVariant,
            ),
          ),
          child: _CardItemScope(
            onPressChanged: _reportPress,
            child: SizedBox(width: double.infinity, child: widget.child),
          ),
        ),
      ),
    );
  }
}

class _CardItemScope extends InheritedWidget {
  const _CardItemScope({required this.onPressChanged, required super.child});

  final ValueChanged<bool> onPressChanged;

  @override
  bool updateShouldNotify(_CardItemScope oldWidget) => false;
}
