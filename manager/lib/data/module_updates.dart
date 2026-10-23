import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:nekosu/data/modules.dart';

/// Metadata published behind a module's `updateJson` endpoint.
///
/// The schema matches the KernelSU/MMRL module repository format:
/// `{"version": "...", "versionCode": 1, "zipUrl": "...", "changelog": "..."}`.
class ModuleUpdate {
  const ModuleUpdate({
    required this.version,
    required this.versionCode,
    required this.zipUrl,
    required this.changelogUrl,
  });

  final String version;
  final int versionCode;
  final String zipUrl;
  final String? changelogUrl;

  /// Parses update metadata, returning `null` when required fields are missing.
  static ModuleUpdate? parse(Object? json) {
    if (json is! Map) return null;
    final version = _stringValue(json['version']);
    final versionCode = _intValue(json['versionCode']);
    final zipUrl = _stringValue(json['zipUrl']);
    if (versionCode == null || versionCode <= 0 || zipUrl.isEmpty) return null;
    return ModuleUpdate(
      version: version.isEmpty ? '$versionCode' : version,
      versionCode: versionCode,
      zipUrl: zipUrl,
      changelogUrl: _httpUrl(_stringValue(json['changelog'])),
    );
  }

  static String _stringValue(Object? value) {
    if (value == null) return '';
    return value.toString().trim();
  }

  static int? _intValue(Object? value) {
    if (value is int) return value;
    if (value is double) {
      return value == value.truncateToDouble() ? value.toInt() : null;
    }
    if (value is String) return int.tryParse(value.trim());
    return null;
  }

  static String? _httpUrl(String value) {
    if (value.isEmpty) return null;
    final uri = Uri.tryParse(value);
    if (uri == null || !isHttpUri(uri)) return null;
    return value;
  }

  static bool isHttpUri(Uri uri) =>
      uri.isScheme('http') || uri.isScheme('https');
}

/// Outcome of checking a single module against its `updateJson` endpoint.
class ModuleUpdateCheck {
  const ModuleUpdateCheck.none() : update = null, error = null;

  const ModuleUpdateCheck.available(ModuleUpdate this.update) : error = null;

  const ModuleUpdateCheck.failed(String this.error) : update = null;

  final ModuleUpdate? update;
  final String? error;

  bool get hasUpdate => update != null;

  bool get failed => error != null;
}

/// Raised when update metadata cannot be fetched or a module ZIP cannot be
/// downloaded. The message is shown to the user verbatim.
class ModuleUpdateException implements Exception {
  const ModuleUpdateException(this.message);

  final String message;

  @override
  String toString() => message;
}

/// Fetches and validates module update metadata.
class ModuleUpdateService {
  ModuleUpdateService._();

  static const _connectTimeout = Duration(seconds: 10);
  static const _requestTimeout = Duration(seconds: 20);
  static const _maxMetadataBytes = 256 * 1024;
  static const _maxChangelogBytes = 256 * 1024;

  /// Checks every module that declares `updateJson`, at most [concurrency]
  /// requests at a time, and returns the result keyed by module id.
  static Future<Map<String, ModuleUpdateCheck>> checkAll(
    List<ModuleInfo> modules, {
    int concurrency = 4,
  }) async {
    final targets = modules
        .where((module) => module.updateJson.trim().isNotEmpty)
        .toList(growable: false);
    final checks = <String, ModuleUpdateCheck>{};
    var next = 0;

    Future<void> worker() async {
      while (next < targets.length) {
        final module = targets[next++];
        checks[module.id] = await check(module);
      }
    }

    final workers = targets.length < concurrency ? targets.length : concurrency;
    await Future.wait([for (var i = 0; i < workers; i++) worker()]);
    return checks;
  }

  /// Checks one module. Network and format failures are returned as failed
  /// checks so a single broken update channel cannot hide the other modules.
  static Future<ModuleUpdateCheck> check(ModuleInfo module) async {
    final updateJson = module.updateJson.trim();
    final uri = Uri.tryParse(updateJson);
    if (uri == null || !ModuleUpdate.isHttpUri(uri)) {
      return ModuleUpdateCheck.failed('Invalid updateJson URL: $updateJson');
    }

    final localVersionCode = module.localVersionCode;
    if (localVersionCode <= 0) {
      // Without a numeric versionCode there is nothing to compare against.
      // Surface it instead of silently skipping the update channel.
      return ModuleUpdateCheck.failed(
        'Module has no numeric versionCode in module.prop',
      );
    }

    final client = HttpClient()..connectionTimeout = _connectTimeout;
    try {
      final request = await client.getUrl(uri).timeout(_requestTimeout);
      request.headers.set(HttpHeaders.acceptHeader, 'application/json');
      request.headers.set(HttpHeaders.userAgentHeader, 'NekoSU');
      final response = await request.close().timeout(_requestTimeout);
      if (response.statusCode != HttpStatus.ok) {
        return ModuleUpdateCheck.failed(
          'HTTP ${response.statusCode} from $updateJson',
        );
      }
      final body = await _readBody(response);
      final text = utf8.decode(body).replaceFirst('\uFEFF', '');
      final update = ModuleUpdate.parse(jsonDecode(text));
      if (update == null) {
        return ModuleUpdateCheck.failed(
          'Invalid update metadata from $updateJson',
        );
      }
      final zipUri = Uri.tryParse(update.zipUrl);
      if (zipUri == null || !ModuleUpdate.isHttpUri(zipUri)) {
        return ModuleUpdateCheck.failed('Invalid zipUrl in $updateJson');
      }
      return update.versionCode > localVersionCode
          ? ModuleUpdateCheck.available(update)
          : const ModuleUpdateCheck.none();
    } on TimeoutException {
      return ModuleUpdateCheck.failed('Timed out while checking $updateJson');
    } on Object catch (error) {
      return ModuleUpdateCheck.failed('$error');
    } finally {
      client.close(force: true);
    }
  }

  /// Fetches the complete changelog text behind [update]'s `changelog` URL.
  static Future<String> fetchChangelog(ModuleUpdate update) async {
    final changelogUrl = update.changelogUrl;
    if (changelogUrl == null) {
      throw const ModuleUpdateException('No changelog URL provided');
    }
    final uri = Uri.tryParse(changelogUrl);
    if (uri == null || !ModuleUpdate.isHttpUri(uri)) {
      throw ModuleUpdateException('Invalid changelog URL: $changelogUrl');
    }

    final client = HttpClient()..connectionTimeout = _connectTimeout;
    try {
      final request = await client.getUrl(uri).timeout(_requestTimeout);
      request.headers.set(HttpHeaders.userAgentHeader, 'NekoSU');
      final response = await request.close().timeout(_requestTimeout);
      if (response.statusCode != HttpStatus.ok) {
        throw ModuleUpdateException(
          'HTTP ${response.statusCode} from $changelogUrl',
        );
      }
      final body = await _readBody(response, maxBytes: _maxChangelogBytes);
      return utf8.decode(body, allowMalformed: true).replaceFirst('\uFEFF', '');
    } on TimeoutException {
      throw ModuleUpdateException(
        'Timed out while loading the changelog from $changelogUrl',
      );
    } finally {
      client.close(force: true);
    }
  }

  /// Downloads [update] to [destination], replacing any existing file.
  ///
  /// [onProgress] receives the bytes written and the expected total, which is
  /// `-1` when the server does not send a content length.
  static Future<void> download(
    ModuleUpdate update, {
    required File destination,
    void Function(int received, int total)? onProgress,
  }) async {
    final uri = Uri.tryParse(update.zipUrl);
    if (uri == null || !ModuleUpdate.isHttpUri(uri)) {
      throw ModuleUpdateException('Invalid zipUrl: ${update.zipUrl}');
    }

    await destination.parent.create(recursive: true);
    final client = HttpClient()..connectionTimeout = _connectTimeout;
    var completed = false;
    try {
      final request = await client.getUrl(uri).timeout(_requestTimeout);
      request.headers.set(HttpHeaders.userAgentHeader, 'NekoSU');
      final response = await request.close().timeout(_requestTimeout);
      if (response.statusCode != HttpStatus.ok) {
        throw ModuleUpdateException(
          'HTTP ${response.statusCode} from ${update.zipUrl}',
        );
      }

      final total = response.contentLength;
      final sink = destination.openWrite();
      var received = 0;
      try {
        await for (final chunk in response.timeout(_requestTimeout)) {
          sink.add(chunk);
          received += chunk.length;
          onProgress?.call(received, total);
        }
      } finally {
        await sink.close();
      }
      if (received == 0) {
        throw ModuleUpdateException('Downloaded update is empty');
      }
      completed = true;
    } finally {
      client.close(force: true);
      if (!completed) {
        try {
          if (await destination.exists()) await destination.delete();
        } on FileSystemException {
          // The partial archive is unusable either way; keep the original
          // download error rather than masking it with a cleanup failure.
        }
      }
    }
  }

  static Future<List<int>> _readBody(
    HttpClientResponse response, {
    int maxBytes = _maxMetadataBytes,
  }) async {
    final bytes = <int>[];
    await for (final chunk in response.timeout(_requestTimeout)) {
      bytes.addAll(chunk);
      if (bytes.length > maxBytes) {
        throw ModuleUpdateException(
          'Response is larger than the $maxBytes byte limit',
        );
      }
    }
    return bytes;
  }
}
