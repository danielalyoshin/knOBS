// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/menu_theme.h"

#include <windows.h>

namespace knobs::tray {
namespace {

// uxtheme's undocumented PreferredAppMode.
enum class PreferredAppMode { kDefault, kAllowDark, kForceDark, kForceLight };

using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode mode);
using FlushMenuThemesFn = void(WINAPI*)();

// Ordinals in uxtheme.dll. 135 was AllowDarkModeForApp(bool) before Windows
// 10 1903 (build 18362), so it's only used from that build on.
constexpr WORD kSetPreferredAppMode = 135;
constexpr WORD kFlushMenuThemes = 136;
constexpr DWORD kFirstBuild = 18362;

struct UxTheme {
  SetPreferredAppModeFn set_preferred_app_mode = nullptr;
  FlushMenuThemesFn flush_menu_themes = nullptr;
};

bool AtLeastBuild(DWORD build) {
  OSVERSIONINFOEXW version = {sizeof(version)};
  version.dwMajorVersion = 10;
  version.dwBuildNumber = build;
  DWORDLONG mask = 0;
  mask = VerSetConditionMask(mask, VER_MAJORVERSION, VER_GREATER_EQUAL);
  mask = VerSetConditionMask(mask, VER_BUILDNUMBER, VER_GREATER_EQUAL);
  return VerifyVersionInfoW(&version, VER_MAJORVERSION | VER_BUILDNUMBER, mask) != FALSE;
}

const UxTheme& GetUxTheme() {
  static const UxTheme ux = [] {
    UxTheme found;
    if (!AtLeastBuild(kFirstBuild)) return found;
    // Stays loaded: user32 and comctl32 already use it.
    const HMODULE module = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return found;
    const auto set = GetProcAddress(module, MAKEINTRESOURCEA(kSetPreferredAppMode));
    const auto flush = GetProcAddress(module, MAKEINTRESOURCEA(kFlushMenuThemes));
    if (!set || !flush) return found;
    found.set_preferred_app_mode = reinterpret_cast<SetPreferredAppModeFn>(reinterpret_cast<void*>(set));
    found.flush_menu_themes = reinterpret_cast<FlushMenuThemesFn>(reinterpret_cast<void*>(flush));
    return found;
  }();
  return ux;
}

bool HighContrast() {
  HIGHCONTRASTW contrast = {sizeof(contrast)};
  return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
         (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

}  // namespace

bool WindowsModeIsDark() {
  DWORD light = 1;
  DWORD size = sizeof(light);
  const LSTATUS status =
      RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                   L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
  return status == ERROR_SUCCESS && light == 0;
}

void ApplyMenuTheme(MenuTheme theme) {
  const UxTheme& ux = GetUxTheme();
  if (!ux.set_preferred_app_mode) return;
  PreferredAppMode mode = PreferredAppMode::kForceLight;
  if (theme == MenuTheme::kDark) {
    mode = PreferredAppMode::kForceDark;
  } else if (theme == MenuTheme::kSystem) {
    if (HighContrast()) {
      mode = PreferredAppMode::kDefault;
    } else if (WindowsModeIsDark()) {
      mode = PreferredAppMode::kForceDark;
    }
  }
  ux.set_preferred_app_mode(mode);
  ux.flush_menu_themes();
}

}  // namespace knobs::tray
