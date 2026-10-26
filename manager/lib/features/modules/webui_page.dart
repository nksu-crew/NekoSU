import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:isolate';

import 'package:material_ui/material_ui.dart';
import 'package:flutter/services.dart';
import 'package:nekosu/core/ncore_ffi.dart';
import 'package:nekosu/data/modules.dart';
import 'package:nekosu/l10n/app_localizations.dart';
import 'package:nekosu/shared/app_feedback.dart';

const _webUiViewType = 'nekosu/webui';
const _webUiChannelPrefix = 'nekosu/webui/';

class WebUiPage extends StatefulWidget {
  const WebUiPage({super.key, required this.entryPath, required this.module});

  final String entryPath;
  final ModuleInfo module;

  @override
  State<WebUiPage> createState() => _WebUiPageState();
}

class _WebUiPageState extends State<WebUiPage> {
  MethodChannel? _channel;
  bool _loading = true;
  String? _error;
  bool _insetsEnabled = false;

  @override
  void dispose() {
    _channel?.setMethodCallHandler(null);
    unawaited(SystemChrome.setEnabledSystemUIMode(SystemUiMode.edgeToEdge));
    super.dispose();
  }

  void _onPlatformViewCreated(int id) {
    final channel = MethodChannel('$_webUiChannelPrefix$id');
    channel.setMethodCallHandler(_onNativeCall);
    _channel = channel;
    unawaited(channel.invokeMethod<void>('ready'));
  }

  Future<Object?> _onNativeCall(MethodCall call) async {
    switch (call.method) {
      case 'pageStarted':
        if (mounted) {
          setState(() {
            _loading = true;
            _error = null;
          });
        }
        return null;
      case 'pageFinished':
        if (mounted) setState(() => _loading = false);
        return null;
      case 'pageError':
        if (mounted) {
          setState(() {
            _loading = false;
            _error = call.arguments?.toString() ?? 'WebUI load failed';
          });
        }
        return null;
      case 'edgeToEdge':
        if (mounted) {
          setState(() => _insetsEnabled = call.arguments == true);
        }
        return null;
      case 'exec':
        return _runRootCommand(_asStringMap(call.arguments));
      case 'toast':
        showAppSnackBar(context, call.arguments?.toString() ?? '');
        return null;
      case 'exit':
        _closeWebUi();
        return null;
      case 'fullScreen':
        await SystemChrome.setEnabledSystemUIMode(
          call.arguments == true
              ? SystemUiMode.immersiveSticky
              : SystemUiMode.edgeToEdge,
        );
        return null;
      default:
        throw MissingPluginException(
          'Unsupported WebUI host method: ${call.method}',
        );
    }
  }

  static Map<String, dynamic> _asStringMap(Object? value) {
    if (value is Map) return value.cast<String, dynamic>();
    return const <String, dynamic>{};
  }

  Future<Map<String, dynamic>> _runRootCommand(
    Map<String, dynamic> payload,
  ) async {
    final command = _composeCommand(payload);
    final output = await Isolate.run(() => NcoreFfi.webUiExec(command));
    if (output == null) {
      throw StateError('WebUI command bridge is unavailable');
    }
    final decoded = jsonDecode(output);
    if (decoded is! Map) {
      throw const FormatException('Invalid WebUI command result');
    }
    return decoded.cast<String, dynamic>();
  }

  String _composeCommand(Map<String, dynamic> payload) {
    final buffer = StringBuffer();
    final options = _decodeOptions(payload['options']);

    final cwd = options['cwd']?.toString().trim() ?? '';
    if (cwd.isNotEmpty) buffer.write('cd ${_shellQuote(cwd)};');

    final environment = options['env'];
    if (environment is Map) {
      environment.forEach((key, value) {
        final name = key.toString();
        if (RegExp(r'^[A-Za-z_][A-Za-z0-9_]*$').hasMatch(name)) {
          buffer.write('export $name=${_shellQuote(value.toString())};');
        }
      });
    }

    buffer.write(payload['command']?.toString() ?? '');
    for (final argument in _decodeArguments(payload['args'])) {
      buffer.write(' ${_shellQuote(argument.toString())}');
    }
    return buffer.toString();
  }

  static Map<String, dynamic> _decodeOptions(Object? raw) {
    final decoded = _decodeJson(raw);
    return decoded is Map ? decoded.cast<String, dynamic>() : const {};
  }

  static List<dynamic> _decodeArguments(Object? raw) {
    final decoded = _decodeJson(raw);
    return decoded is List ? decoded : const [];
  }

  static Object? _decodeJson(Object? raw) {
    if (raw is! String || raw.trim().isEmpty) return raw;
    try {
      return jsonDecode(raw);
    } catch (_) {
      return null;
    }
  }

  static String _shellQuote(String value) {
    return "'${value.replaceAll("'", "'\\''")}'";
  }

  void _closeWebUi() {
    if (!mounted) return;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) Navigator.of(context).pop();
    });
  }

  Future<void> _goBack() async {
    final channel = _channel;
    if (channel != null) {
      try {
        final canGoBack =
            await channel.invokeMethod<bool>('canGoBack') ?? false;
        if (canGoBack) {
          await channel.invokeMethod<void>('goBack');
          return;
        }
      } on PlatformException {
        if (mounted) Navigator.of(context).pop();
        return;
      }
    }
    if (mounted) Navigator.of(context).pop();
  }

  void _retry() {
    if (!mounted) return;
    setState(() {
      _loading = true;
      _error = null;
    });
    unawaited(_channel?.invokeMethod<void>('reload'));
  }

  Map<String, Object?> _creationParams(ColorScheme scheme) {
    final module = widget.module;
    return {
      'moduleId': module.id,
      'webRoot': File(widget.entryPath).parent.path,
      'entryName': File(widget.entryPath).uri.pathSegments.last,
      'colorsCss': _buildColorsCss(scheme),
      'module': {
        'id': module.id,
        'name': module.name,
        'version': module.version,
        'versionCode': int.tryParse(module.versionCode) ?? 0,
        'author': module.author,
        'description': module.description,
        'enabled': module.enabled,
      },
    };
  }

  static String _buildColorsCss(ColorScheme scheme) {
    final colors = <String, Color>{
      'primary': scheme.primary,
      'onPrimary': scheme.onPrimary,
      'primaryContainer': scheme.primaryContainer,
      'onPrimaryContainer': scheme.onPrimaryContainer,
      'inversePrimary': scheme.inversePrimary,
      'secondary': scheme.secondary,
      'onSecondary': scheme.onSecondary,
      'secondaryContainer': scheme.secondaryContainer,
      'onSecondaryContainer': scheme.onSecondaryContainer,
      'tertiary': scheme.tertiary,
      'onTertiary': scheme.onTertiary,
      'tertiaryContainer': scheme.tertiaryContainer,
      'onTertiaryContainer': scheme.onTertiaryContainer,
      'background': scheme.surface,
      'onBackground': scheme.onSurface,
      'surface': scheme.surface,
      'tonalSurface': scheme.surfaceContainer,
      'onSurface': scheme.onSurface,
      'surfaceVariant': scheme.surfaceVariant,
      'onSurfaceVariant': scheme.onSurfaceVariant,
      'surfaceTint': scheme.surfaceTint,
      'inverseSurface': scheme.inverseSurface,
      'inverseOnSurface': scheme.onInverseSurface,
      'error': scheme.error,
      'onError': scheme.onError,
      'errorContainer': scheme.errorContainer,
      'onErrorContainer': scheme.onErrorContainer,
      'outline': scheme.outline,
      'outlineVariant': scheme.outlineVariant,
      'scrim': scheme.scrim,
      'surfaceBright': scheme.surfaceBright,
      'surfaceDim': scheme.surfaceDim,
      'surfaceContainer': scheme.surfaceContainer,
      'surfaceContainerHigh': scheme.surfaceContainerHigh,
      'surfaceContainerHighest': scheme.surfaceContainerHighest,
      'surfaceContainerLow': scheme.surfaceContainerLow,
      'surfaceContainerLowest': scheme.surfaceContainerLowest,
      'filledTonalButtonContentColor': scheme.onPrimaryContainer,
      'filledTonalButtonContainerColor': scheme.secondaryContainer,
      'filledTonalButtonDisabledContentColor': scheme.onSurfaceVariant,
      'filledTonalButtonDisabledContainerColor': scheme.surfaceVariant,
      'filledCardContentColor': scheme.onPrimaryContainer,
      'filledCardContainerColor': scheme.primaryContainer,
      'filledCardDisabledContentColor': scheme.onSurfaceVariant,
      'filledCardDisabledContainerColor': scheme.surfaceVariant,
    };

    final buffer = StringBuffer(':root {\n');
    colors.forEach((name, color) {
      buffer.writeln('  --$name: ${_cssColor(color)};');
    });
    buffer.write('}');
    return buffer.toString();
  }

  static String _cssColor(Color color) {
    final value = color.toARGB32();
    final alpha = (value >> 24) & 0xff;
    final red = (value >> 16) & 0xff;
    final green = (value >> 8) & 0xff;
    final blue = value & 0xff;
    final rgb = [
      red,
      green,
      blue,
    ].map((channel) => channel.toRadixString(16).padLeft(2, '0')).join();
    if (alpha == 0xff) return '#$rgb';
    return '#$rgb${alpha.toRadixString(16).padLeft(2, '0')}';
  }

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final textTheme = Theme.of(context).textTheme;
    final l10n = AppLocalizations.of(context)!;

    return PopScope(
      canPop: false,
      onPopInvokedWithResult: (didPop, _) {
        if (!didPop) unawaited(_goBack());
      },
      child: Scaffold(
        backgroundColor: scheme.surfaceContainer,
        body: Stack(
          fit: StackFit.expand,
          children: [
            Padding(
              padding: _insetsEnabled
                  ? MediaQuery.viewPaddingOf(context)
                  : EdgeInsets.zero,
              child: AndroidView(
                viewType: _webUiViewType,
                creationParams: _creationParams(scheme),
                creationParamsCodec: const StandardMessageCodec(),
                onPlatformViewCreated: _onPlatformViewCreated,
              ),
            ),
            if (_loading && _error == null)
              IgnorePointer(
                child: ColoredBox(
                  color: scheme.surfaceContainer.withValues(alpha: 0.72),
                  child: const Center(child: CircularProgressIndicator()),
                ),
              ),
            if (_error != null)
              ColoredBox(
                color: scheme.surfaceContainer,
                child: Center(
                  child: SingleChildScrollView(
                    padding: const EdgeInsets.all(32),
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        Icon(
                          Icons.web_asset_off_rounded,
                          size: 48,
                          color: scheme.error,
                        ),
                        const SizedBox(height: 16),
                        Text(
                          l10n.webUiLoadFailed,
                          textAlign: TextAlign.center,
                          style: textTheme.titleMedium,
                        ),
                        const SizedBox(height: 8),
                        Text(
                          _error!,
                          textAlign: TextAlign.center,
                          style: textTheme.bodySmall?.copyWith(
                            color: scheme.onSurfaceVariant,
                          ),
                        ),
                        const SizedBox(height: 24),
                        Wrap(
                          spacing: 12,
                          runSpacing: 12,
                          alignment: WrapAlignment.center,
                          children: [
                            OutlinedButton.icon(
                              onPressed: () => Navigator.of(context).pop(),
                              icon: const Icon(Icons.close_rounded),
                              label: Text(l10n.modulesClose),
                            ),
                            FilledButton.icon(
                              onPressed: _retry,
                              icon: const Icon(Icons.refresh_rounded),
                              label: Text(l10n.retry),
                            ),
                          ],
                        ),
                      ],
                    ),
                  ),
                ),
              ),
          ],
        ),
      ),
    );
  }
}
