import 'package:material_ui/material_ui.dart';

void showAppSnackBar(BuildContext context, String message) {
  if (!context.mounted || message.isEmpty) return;
  ScaffoldMessenger.of(context)
    ..hideCurrentSnackBar()
    ..showSnackBar(SnackBar(content: Text(message)));
}
