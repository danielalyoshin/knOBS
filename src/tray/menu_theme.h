// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace knobs::tray {

enum class MenuTheme {
  // Follow Windows' mode, the one the taskbar uses, which the tray menu opens
  // from. Light under high contrast, where Windows draws menus itself.
  kSystem,
  kLight,
  kDark,
};

// Whether Windows' mode (Settings > Personalization > Colors) is dark.
bool WindowsModeIsDark();

// Makes this process's popup menus light or dark, through uxtheme's
// SetPreferredAppMode, which is exported by ordinal only (Windows 10 1903
// and later). Without it, menus stay light. Call again when the mode
// changes (WM_SETTINGCHANGE with "ImmersiveColorSet").
void ApplyMenuTheme(MenuTheme theme);

}  // namespace knobs::tray
