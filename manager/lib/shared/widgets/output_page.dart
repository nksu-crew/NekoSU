import 'dart:async';

import 'package:material_ui/material_ui.dart';
import 'package:nekosu/core/output_events.dart';
import 'package:nekosu/core/output_log.dart';
import 'package:nekosu/l10n/app_localizations.dart';
import 'package:nekosu/shared/app_feedback.dart';

typedef OutputTask = Stream<OutputEvent> Function();

enum OutputStatus { running, succeeded, failed }

class OutputAction {
  const OutputAction({
    required this.label,
    required this.icon,
    required this.onPressed,
  });

  final String label;
  final IconData icon;
  final void Function(BuildContext context) onPressed;
}

/// Full-screen, live command output based on the BakaSU/KernelSU install page.
class OutputPage extends StatefulWidget {
  const OutputPage({
    super.key,
    required this.task,
    required this.runningTitle,
    required this.successTitle,
    required this.failureTitle,
    this.logFileName = 'nekosu-output',
    this.completionAction,
    this.onFinished,
  });

  final OutputTask task;
  final String runningTitle;
  final String successTitle;
  final String failureTitle;
  final String logFileName;
  final OutputAction? completionAction;
  final ValueChanged<OutputStatus>? onFinished;

  @override
  State<OutputPage> createState() => _OutputPageState();
}

class _OutputPageState extends State<OutputPage> {
  static const _maxOutputLength = 1024 * 1024;
  static final _ansiEscape = RegExp('\x1B\\[[0-?]*[ -/]*[@-~]');

  final _scrollController = ScrollController();
  StreamSubscription<OutputEvent>? _subscription;
  String _output = '';
  OutputStatus _status = OutputStatus.running;
  bool _saving = false;

  @override
  void initState() {
    super.initState();
    _subscription = widget.task().listen(
      _handleEvent,
      onError: _handleError,
      onDone: _handleDone,
    );
  }

  @override
  void dispose() {
    unawaited(_subscription?.cancel());
    _scrollController.dispose();
    super.dispose();
  }

  void _handleEvent(OutputEvent event) {
    if (!mounted) return;
    switch (event) {
      case OutputChunk(:final text):
        if (text.isEmpty) return;
        setState(() {
          _output += _sanitize(text);
          if (_output.length > _maxOutputLength) {
            _output = _output.substring(_output.length - _maxOutputLength);
          }
        });
        _scrollToEnd();
      case OutputFinished(:final exitCode):
        _finish(exitCode == 0 ? OutputStatus.succeeded : OutputStatus.failed);
    }
  }

  void _handleError(Object error, StackTrace _) {
    if (!mounted) return;
    _appendError(error.toString());
    _finish(OutputStatus.failed);
  }

  void _handleDone() {
    if (mounted && _status == OutputStatus.running) {
      _finish(OutputStatus.failed);
    }
  }

  void _finish(OutputStatus status) {
    if (!mounted || _status != OutputStatus.running) return;
    setState(() => _status = status);
    _scrollToEnd();
    widget.onFinished?.call(status);
  }

  void _appendError(String message) {
    if (!mounted || message.trim().isEmpty) return;
    setState(() {
      if (_output.isNotEmpty && !_output.endsWith('\n')) _output += '\n';
      _output += message.trim();
    });
  }

  void _scrollToEnd() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || !_scrollController.hasClients) return;
      _scrollController.jumpTo(_scrollController.position.maxScrollExtent);
    });
  }

  static String _sanitize(String value) {
    return value.replaceAll(_ansiEscape, '').replaceAll('\r', '');
  }

  String get _title => switch (_status) {
    OutputStatus.running => widget.runningTitle,
    OutputStatus.succeeded => widget.successTitle,
    OutputStatus.failed => widget.failureTitle,
  };

  Color _titleColor(ColorScheme scheme) => switch (_status) {
    OutputStatus.running => scheme.primary,
    OutputStatus.succeeded => scheme.tertiary,
    OutputStatus.failed => scheme.error,
  };

  String _logName() {
    final now = DateTime.now();
    String two(int value) => value.toString().padLeft(2, '0');
    final timestamp =
        '${now.year}-${two(now.month)}-${two(now.day)}-'
        '${two(now.hour)}-${two(now.minute)}-${two(now.second)}';
    return '${widget.logFileName}-$timestamp.log';
  }

  Future<void> _save() async {
    if (_saving || _output.isEmpty) return;
    final l10n = AppLocalizations.of(context)!;
    final fileName = _logName();
    setState(() => _saving = true);
    try {
      final savedName = await OutputLog.save(
        fileName: fileName,
        content: _output,
      );
      if (!mounted) return;
      showAppSnackBar(context, l10n.outputLogSaved(savedName ?? fileName));
    } catch (_) {
      showAppSnackBar(context, l10n.outputLogSaveFailed);
    } finally {
      if (mounted) setState(() => _saving = false);
    }
  }

  void _close() {
    if (_status == OutputStatus.running) return;
    Navigator.of(context).pop();
  }

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final textTheme = Theme.of(context).textTheme;
    final l10n = AppLocalizations.of(context)!;
    final running = _status == OutputStatus.running;
    final action = widget.completionAction;

    return PopScope(
      canPop: !running,
      child: Scaffold(
        backgroundColor: scheme.surfaceContainer,
        floatingActionButton: !running && action != null
            ? FloatingActionButton.extended(
                onPressed: () => action.onPressed(context),
                icon: Icon(action.icon),
                label: Text(action.label),
              )
            : null,
        body: SafeArea(
          bottom: false,
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Padding(
                padding: const EdgeInsets.fromLTRB(12, 8, 8, 4),
                child: Row(
                  children: [
                    IconButton.filledTonal(
                      onPressed: running ? null : _close,
                      tooltip: MaterialLocalizations.of(context)
                          .backButtonTooltip,
                      icon: const Icon(Icons.arrow_back_rounded),
                    ),
                    const SizedBox(width: 12),
                    Expanded(
                      child: Text(
                        _title,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: textTheme.titleLarge?.copyWith(
                          color: _titleColor(scheme),
                        ),
                      ),
                    ),
                    const SizedBox(width: 8),
                    if (_saving)
                      const Padding(
                        padding: EdgeInsets.all(12),
                        child: SizedBox.square(
                          dimension: 20,
                          child: CircularProgressIndicator(strokeWidth: 2),
                        ),
                      )
                    else
                      IconButton(
                        onPressed: _output.isEmpty ? null : _save,
                        tooltip: l10n.outputSaveLog,
                        icon: const Icon(Icons.save_alt_rounded),
                      ),
                  ],
                ),
              ),
              Expanded(
                child: Stack(
                  fit: StackFit.expand,
                  children: [
                    if (running && _output.isEmpty)
                      const Center(
                        child: SizedBox.square(
                          dimension: 32,
                          child: CircularProgressIndicator(strokeWidth: 2.5),
                        ),
                      ),
                    SingleChildScrollView(
                      controller: _scrollController,
                      padding: EdgeInsets.fromLTRB(
                        20,
                        16,
                        20,
                        104 + MediaQuery.viewPaddingOf(context).bottom,
                      ),
                      child: SelectableText(
                        _output.isEmpty
                            ? (running
                                  ? l10n.outputWaiting
                                  : l10n.outputNoOutput)
                            : _output,
                        style: textTheme.bodyMedium?.copyWith(
                          fontFamily: 'monospace',
                          fontSize: 14,
                          height: 1.6,
                          color: _output.isEmpty
                              ? scheme.onSurfaceVariant
                              : scheme.onSurface,
                        ),
                      ),
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
