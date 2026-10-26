import 'package:material_ui/material_ui.dart';
import 'package:liquid_glass_widgets/liquid_glass_widgets.dart';
import 'package:nekosu/app/app.dart';
import 'package:nekosu/core/display_mode_controller.dart';
import 'package:nekosu/core/ncore_ffi.dart';
import 'package:nekosu/core/settings_controller.dart';
import 'package:nekosu/core/settings_store.dart';
import 'package:nekosu/data/install_status.dart';

export 'package:nekosu/app/app.dart' show MyApp;

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await LiquidGlassWidgets.initialize();
  NcoreFfi.initialize();
  await InstallStatusRepository.initialize();
  try {
    await SettingsStore.initialize();
  } catch (error) {
    debugPrint('Unable to open the settings store: $error');
  }
  await SettingsController.instance.load();
  await DisplayModeController.initialize();
  runApp(
    LiquidGlassWidgets.wrap(
      child: const MyApp(),
      brightnessResolver: Theme.maybeBrightnessOf,
    ),
  );
}
