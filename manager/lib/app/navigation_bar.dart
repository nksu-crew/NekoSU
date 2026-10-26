import 'package:material_ui/material_ui.dart';
import 'package:flutter/services.dart';
import 'package:liquid_glass_widgets/liquid_glass_widgets.dart';
import 'package:nekosu/shared/layout_insets.dart';

class NavBarTab {
  const NavBarTab({required this.label, required this.icon, this.activeIcon});
  final String label;
  final Widget icon;
  final Widget? activeIcon;
}

abstract class _NavigationBarBase extends StatelessWidget {
  const _NavigationBarBase({
    super.key,
    required this.tabs,
    required this.selectedIndex,
    required this.onTabSelected,
  });

  final List<NavBarTab> tabs;
  final int selectedIndex;
  final ValueChanged<int> onTabSelected;

  void _selectTab(int index) {
    HapticFeedback.lightImpact();
    onTabSelected(index);
  }
}

class ModernCapsuleNavBar extends _NavigationBarBase {
  const ModernCapsuleNavBar({
    super.key,
    required super.tabs,
    required super.selectedIndex,
    required super.onTabSelected,
  });

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    final count = tabs.length;

    final alignmentX = count > 1
        ? -1.0 + (selectedIndex * 2 / (count - 1))
        : 0.0;

    return SafeArea(
      child: Padding(
        padding: const EdgeInsets.only(
          bottom: capsuleNavBarBottomGap,
          left: 24,
          right: 24,
        ),
        child: Container(
          height: capsuleNavBarHeight,
          decoration: BoxDecoration(
            color: Theme.of(context).scaffoldBackgroundColor,
            borderRadius: BorderRadius.circular(34),
            boxShadow: [
              BoxShadow(
                color: scheme.shadow.withValues(alpha: 0.15),
                blurRadius: 12,
                offset: const Offset(0, 4),
              ),
            ],
          ),
          child: Stack(
            children: [
              AnimatedAlign(
                alignment: Alignment(alignmentX, 0),
                duration: const Duration(milliseconds: 400),
                curve: Curves.easeOutCubic,
                child: FractionallySizedBox(
                  widthFactor: 1 / count,
                  child: Padding(
                    padding: const EdgeInsets.all(6),
                    child: Container(
                      decoration: BoxDecoration(
                        color: scheme.primaryContainer,
                        borderRadius: BorderRadius.circular(28),
                      ),
                    ),
                  ),
                ),
              ),
              Row(
                children: List.generate(
                  count,
                  (index) => Expanded(
                    child: _TabItem(
                      tab: tabs[index],
                      selected: selectedIndex == index,
                      scheme: scheme,
                      onTap: () => _selectTab(index),
                    ),
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class GlassFloatingNavBar extends _NavigationBarBase {
  const GlassFloatingNavBar({
    super.key,
    required super.tabs,
    required super.selectedIndex,
    required super.onTabSelected,
  });

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    return SafeArea(
      top: false,
      child: GlassTabBar.bottom(
        tabs: [
          for (final tab in tabs)
            GlassTab(
              icon: tab.icon,
              activeIcon: tab.activeIcon ?? tab.icon,
              label: tab.label,
            ),
        ],
        selectedIndex: selectedIndex,
        onTabSelected: _selectTab,
        barHeight: glassNavBarHeight,
        verticalPadding: glassNavBarVerticalPadding,
        horizontalPadding: 20,
        selectedIconColor: scheme.primary,
        unselectedIconColor: scheme.onSurfaceVariant,
        selectedLabelColor: scheme.primary,
        unselectedLabelColor: scheme.onSurfaceVariant,
      ),
    );
  }
}

class ModernNavigationBar extends _NavigationBarBase {
  const ModernNavigationBar({
    super.key,
    required super.tabs,
    required super.selectedIndex,
    required super.onTabSelected,
  });

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    return NavigationBar(
      selectedIndex: selectedIndex,
      onDestinationSelected: _selectTab,
      backgroundColor: scheme.surfaceContainer,
      surfaceTintColor: Colors.transparent,
      indicatorColor: scheme.primaryContainer,
      destinations: [
        for (final tab in tabs)
          NavigationDestination(
            icon: tab.icon,
            selectedIcon: tab.activeIcon ?? tab.icon,
            label: tab.label,
          ),
      ],
    );
  }
}

class _TabItem extends StatelessWidget {
  const _TabItem({
    required this.tab,
    required this.selected,
    required this.scheme,
    required this.onTap,
  });

  final NavBarTab tab;
  final bool selected;
  final ColorScheme scheme;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final contentColor = selected
        ? scheme.onPrimaryContainer
        : scheme.onSurfaceVariant;
    final labelStyle = Theme.of(context).textTheme.labelMedium
        ?.copyWith(color: contentColor);

    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTap: onTap,
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          AnimatedSwitcher(
            duration: const Duration(milliseconds: 250),
            transitionBuilder: (child, animation) {
              return ScaleTransition(
                scale: animation,
                child: FadeTransition(opacity: animation, child: child),
              );
            },
            child: IconTheme(
              key: ValueKey<bool>(selected),
              data: IconThemeData(
                color: contentColor,
                size: selected ? 26 : 24,
              ),
              child: (selected && tab.activeIcon != null)
                  ? tab.activeIcon!
                  : tab.icon,
            ),
          ),
          const SizedBox(height: 4),
          AnimatedDefaultTextStyle(
            duration: const Duration(milliseconds: 250),
            style: labelStyle ?? TextStyle(color: contentColor),
            child: Text(
              tab.label,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
            ),
          ),
        ],
      ),
    );
  }
}
