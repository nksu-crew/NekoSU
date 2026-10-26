import 'dart:collection';
import 'dart:typed_data';

import 'package:material_ui/material_ui.dart';
import 'package:nekosu/data/apps.dart';

class AppIconCache {
  AppIconCache._();

  static final LinkedHashMap<String, Future<Uint8List?>> _cache =
      LinkedHashMap<String, Future<Uint8List?>>();
  static const _maxEntries = 320;

  static Future<Uint8List?> load(String packageName) {
    final cached = _cache.remove(packageName);
    if (cached != null) {
      _cache[packageName] = cached;
      return cached;
    }

    final future = AppsPlatform.getIcon(packageName);
    _cache[packageName] = future;
    while (_cache.length > _maxEntries) {
      _cache.remove(_cache.keys.first);
    }
    return future;
  }

  static void clear() => _cache.clear();
}

class AppIcon extends StatelessWidget {
  const AppIcon({super.key, required this.packageName, this.size = 42});

  final String packageName;
  final double size;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final radius = BorderRadius.circular(size * 0.24);

    return SizedBox.square(
      dimension: size,
      child: ClipRRect(
        borderRadius: radius,
        child: FutureBuilder<Uint8List?>(
          future: AppIconCache.load(packageName),
          builder: (context, snapshot) {
            final bytes = snapshot.data;
            if (bytes != null && bytes.isNotEmpty) {
              return Image.memory(
                bytes,
                fit: BoxFit.cover,
                gaplessPlayback: true,
                errorBuilder: (_, _, _) => _placeholder(scheme),
              );
            }
            return _placeholder(scheme);
          },
        ),
      ),
    );
  }

  Widget _placeholder(ColorScheme scheme) {
    return ColoredBox(
      color: scheme.surfaceContainerHighest,
      child: Center(
        child: Icon(
          Icons.android_rounded,
          size: size * 0.52,
          color: scheme.primary.withValues(alpha: 0.38),
        ),
      ),
    );
  }
}
