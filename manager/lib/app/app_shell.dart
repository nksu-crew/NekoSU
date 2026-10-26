import 'package:material_ui/material_ui.dart';
import 'package:nekosu/app/navigation_bar.dart';
import 'package:nekosu/core/settings_controller.dart';
import 'package:nekosu/features/apps/app_list_page.dart';
import 'package:nekosu/features/home/home_page.dart';
import 'package:nekosu/features/modules/modules_page.dart';
import 'package:nekosu/features/settings/settings_page.dart';
import 'package:nekosu/l10n/app_localizations.dart';
import 'package:nekosu/shared/layout_insets.dart';

class AppShell extends StatefulWidget {
  const AppShell({super.key});

  @override
  State<AppShell> createState() => _AppShellState();
}

class _AppShellState extends State<AppShell> {
  int _selectedIndex = 0;

  @override
  Widget build(BuildContext context) {
    final l10n = AppLocalizations.of(context)!;
    final settings = SettingsController.instance;

    final pages = [
      const HomePage(),
      const AppListPage(),
      const ModulesPage(),
      const SettingsPage(),
    ];

    final tabs = [
      NavBarTab(
        label: l10n.homeLabel,
        icon: const Icon(Icons.home_outlined),
        activeIcon: const Icon(Icons.home),
      ),
      NavBarTab(
        label: l10n.appsLabel,
        icon: const Icon(Icons.apps_outlined),
        activeIcon: const Icon(Icons.apps_rounded),
      ),
      NavBarTab(
        label: l10n.modulesLabel,
        icon: const Icon(Icons.extension_outlined),
        activeIcon: const Icon(Icons.extension_rounded),
      ),
      NavBarTab(
        label: l10n.settingsLabel,
        icon: const Icon(Icons.settings_outlined),
        activeIcon: const Icon(Icons.settings),
      ),
    ];

    return AnimatedBuilder(
      animation: settings,
      builder: (context, _) {
        final navBar = switch (settings.navBarStyle) {
          NavBarStyle.standard => ModernNavigationBar(
            selectedIndex: _selectedIndex,
            onTabSelected: _selectTab,
            tabs: tabs,
          ),
          NavBarStyle.floating => ModernCapsuleNavBar(
            selectedIndex: _selectedIndex,
            onTabSelected: _selectTab,
            tabs: tabs,
          ),
          NavBarStyle.glass => GlassFloatingNavBar(
            selectedIndex: _selectedIndex,
            onTabSelected: _selectTab,
            tabs: tabs,
          ),
        };
        return Scaffold(
          extendBody: settings.navBarStyle != NavBarStyle.standard,
          body: AppNavBarScope(
            style: settings.navBarStyle,
            child: _TabPageSwitcher(
              index: _selectedIndex,
              child: pages[_selectedIndex],
            ),
          ),
          bottomNavigationBar: navBar,
        );
      },
    );
  }

  void _selectTab(int index) {
    setState(() => _selectedIndex = index);
  }
}

class _TabPageSwitcher extends StatelessWidget {
  const _TabPageSwitcher({required this.index, required this.child});

  final int index;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    return AnimatedSwitcher(
      duration: const Duration(milliseconds: 70),
      transitionBuilder: (child, animation) =>
          FadeTransition(opacity: animation, child: child),
      layoutBuilder: (currentChild, previousChildren) => Stack(
        fit: StackFit.expand,
        children: [...previousChildren, if (currentChild != null) currentChild],
      ),
      child: KeyedSubtree(key: ValueKey(index), child: child),
    );
  }
}
