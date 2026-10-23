import 'dart:async';
import 'dart:convert';
import 'dart:isolate';

import 'package:flutter/services.dart';
import 'package:nekosu/core/ncore_ffi.dart';
import 'package:nekosu/core/output_events.dart';

class ModuleInfo {
  const ModuleInfo({
    required this.id,
    required this.name,
    required this.version,
    required this.versionCode,
    required this.author,
    required this.description,
    required this.updateJson,
    required this.enabled,
    required this.update,
    required this.remove,
    required this.hasActionScript,
    required this.hasWebUi,
    required this.metamodule,
  });

  final String id;
  final String name;
  final String version;
  final String versionCode;
  final String author;
  final String description;
  final String updateJson;
  final bool enabled;
  final bool update;
  final bool remove;
  final bool hasActionScript;
  final bool hasWebUi;
  final bool metamodule;

  String get title => name.trim().isEmpty ? id : name.trim();

  /// Numeric `versionCode` from `module.prop`, or `0` when absent/invalid.
  int get localVersionCode => int.tryParse(versionCode.trim()) ?? 0;

  factory ModuleInfo.fromJson(Map<String, dynamic> json) {
    String readString(String key) => json[key]?.toString() ?? '';
    bool readBool(String key) => json[key] == true || json[key] == 'true';

    return ModuleInfo(
      id: readString('id'),
      name: readString('name'),
      version: readString('version'),
      versionCode: readString('versionCode'),
      author: readString('author'),
      description: readString('description'),
      updateJson: readString('updateJson'),
      enabled: readBool('enabled'),
      update: readBool('update'),
      remove: readBool('remove'),
      hasActionScript: readBool('hasActionScript'),
      hasWebUi: readBool('hasWebUi'),
      metamodule: readBool('metamodule'),
    );
  }
}

class ModuleCommandResult {
  const ModuleCommandResult({required this.code, required this.output});

  final int code;
  final String output;

  bool get ok => code == 0;

  factory ModuleCommandResult.fromEnvelope(String? value) {
    if (value == null) {
      return const ModuleCommandResult(
        code: -1,
        output: 'Module FFI is unavailable',
      );
    }
    final newline = value.indexOf('\n');
    if (newline < 0) {
      return ModuleCommandResult(code: -1, output: value);
    }
    return ModuleCommandResult(
      code: int.tryParse(value.substring(0, newline)) ?? -1,
      output: value.substring(newline + 1),
    );
  }
}

class ModuleListResult {
  const ModuleListResult({
    required this.modules,
    required this.code,
    required this.output,
  });

  final List<ModuleInfo> modules;
  final int code;
  final String output;

  bool get ok => code == 0;
}

class ModuleBridge {
  ModuleBridge._();

  static const _platform = MethodChannel('nekosu/modules_platform');
  static const _outputChannel = EventChannel('nekosu/modules_platform/output');

  static Future<ModuleListResult> list() async {
    final envelope = await Isolate.run(() => NcoreFfi.moduleList());
    final result = ModuleCommandResult.fromEnvelope(envelope);
    if (!result.ok) {
      return ModuleListResult(
        modules: const [],
        code: result.code,
        output: result.output,
      );
    }
    return ModuleListResult(
      modules: _decodeModules(result.output),
      code: result.code,
      output: result.output,
    );
  }

  static List<ModuleInfo> _decodeModules(String output) {
    final start = output.indexOf('[');
    final end = output.lastIndexOf(']');
    if (start < 0 || end <= start) return const [];
    try {
      final decoded = jsonDecode(output.substring(start, end + 1));
      if (decoded is! List) return const [];
      return decoded
          .whereType<Map>()
          .map((item) => ModuleInfo.fromJson(item.cast<String, dynamic>()))
          .where((module) => module.id.isNotEmpty)
          .toList(growable: false);
    } catch (_) {
      return const [];
    }
  }

  static Future<ModuleCommandResult> run({
    required String operation,
    required String id,
  }) async {
    final operationCode = switch (operation) {
      'enable' => 0,
      'disable' => 1,
      'remove' => 2,
      'undoRemove' => 3,
      'uninstall' => 4,
      _ => -1,
    };
    if (operationCode < 0) {
      return const ModuleCommandResult(
        code: -1,
        output: 'Unknown module operation',
      );
    }
    final envelope = await Isolate.run(
      () => NcoreFfi.moduleControl(id, operationCode),
    );
    return ModuleCommandResult.fromEnvelope(envelope);
  }

  /// Opens the system document picker and returns the staged ZIP path.
  static Future<String?> pickZip() => _platform.invokeMethod<String>('pickZip');

  static Future<ModuleCommandResult> install(String path) async {
    final envelope = await Isolate.run(() => NcoreFfi.moduleInstall(path));
    return ModuleCommandResult.fromEnvelope(envelope);
  }

  static Future<ModuleCommandResult> action(String id) async {
    final envelope = await Isolate.run(() => NcoreFfi.moduleAction(id));
    return ModuleCommandResult.fromEnvelope(envelope);
  }

  static Stream<OutputEvent> installOutput(String path) {
    return _runOutput('install', path);
  }

  static Stream<OutputEvent> actionOutput(String id) {
    return _runOutput('action', id);
  }

  static Stream<OutputEvent> _runOutput(String operation, String argument) {
    final events = _outputChannel.receiveBroadcastStream({
      'operation': operation,
      'argument': argument,
    });
    return decodeOutputEvents(events);
  }

  static Future<ModuleCommandResult> prepareWebUi(String id) async {
    final envelope = await Isolate.run(() => NcoreFfi.moduleWebUi(id));
    return ModuleCommandResult.fromEnvelope(envelope);
  }
}
