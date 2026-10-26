import 'dart:async';

import 'package:material_ui/material_ui.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:nekosu/core/output_events.dart';
import 'package:nekosu/l10n/app_localizations.dart';
import 'package:nekosu/shared/widgets/output_page.dart';

void main() {
  testWidgets('output page streams text and reports success', (tester) async {
    final events = StreamController<OutputEvent>();
    addTearDown(events.close);

    await tester.pumpWidget(
      MaterialApp(
        locale: const Locale('en'),
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: OutputPage(
          task: () => events.stream,
          runningTitle: 'Running',
          successTitle: 'Succeeded',
          failureTitle: 'Failed',
        ),
      ),
    );

    expect(find.text('Running'), findsOneWidget);

    events.add(const OutputChunk('hello\n'));
    await tester.pump();
    final output = tester.widget<SelectableText>(find.byType(SelectableText));
    expect(output.data, contains('hello'));

    events.add(const OutputFinished(0));
    await tester.pump();
    expect(find.text('Succeeded'), findsOneWidget);
  });
}
