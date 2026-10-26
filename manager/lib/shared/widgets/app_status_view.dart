import 'package:material_ui/material_ui.dart';

class AppLoadingView extends StatelessWidget {
  const AppLoadingView({super.key, this.label});

  final String? label;

  @override
  Widget build(BuildContext context) {
    if (label == null) {
      return const Center(child: CircularProgressIndicator());
    }

    final scheme = Theme.of(context).colorScheme;
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          const SizedBox.square(
            dimension: 36,
            child: CircularProgressIndicator(strokeWidth: 2.5),
          ),
          const SizedBox(height: 16),
          Text(
            label!,
            style: Theme.of(context).textTheme.bodySmall?.copyWith(
              color: scheme.onSurfaceVariant.withValues(alpha: 0.5),
            ),
          ),
        ],
      ),
    );
  }
}

class AppErrorView extends StatelessWidget {
  const AppErrorView({
    super.key,
    required this.message,
    this.detail,
    required this.retryLabel,
    required this.onRetry,
    this.padding = EdgeInsets.zero,
    this.tonalAction = true,
  });

  final String message;
  final String? detail;
  final String retryLabel;
  final VoidCallback onRetry;
  final EdgeInsetsGeometry padding;
  final bool tonalAction;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final textTheme = Theme.of(context).textTheme;
    final errorDetail = detail?.trim();
    final action = tonalAction
        ? FilledButton.tonal(onPressed: onRetry, child: Text(retryLabel))
        : FilledButton(onPressed: onRetry, child: Text(retryLabel));

    return Center(
      child: Padding(
        padding: padding,
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(
              Icons.error_outline_rounded,
              size: 40,
              color: scheme.error.withValues(alpha: 0.55),
            ),
            const SizedBox(height: 12),
            Text(message, style: textTheme.bodyMedium),
            if (errorDetail != null && errorDetail.isNotEmpty) ...[
              const SizedBox(height: 8),
              Text(
                errorDetail,
                textAlign: TextAlign.center,
                style: textTheme.bodySmall?.copyWith(
                  color: scheme.onSurfaceVariant,
                ),
              ),
            ],
            const SizedBox(height: 16),
            action,
          ],
        ),
      ),
    );
  }
}
