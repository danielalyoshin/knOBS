// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "util/result.h"

// Pictures of knobs-tray's menu, dialogs, notifications and icons, for
// checking how they look.
namespace knobs::tools {

// The popup menus this thread has open (window class #32768), outermost
// first.
std::vector<HWND> OpenMenus();

// The rectangle around `windows`, in screen pixels.
std::optional<RECT> BoundsOf(const std::vector<HWND>& windows);

// Saves what the screen shows inside `rect` as a PNG. The thread needs COM.
Status SaveScreenPng(const RECT& rect, const std::filesystem::path& file);

// Saves `window` as a PNG, as it draws itself, whatever covers it. The
// thread needs COM.
Status SaveWindowPng(HWND window, const std::filesystem::path& file);

// A dialog (window class #32770) this thread shows, or null.
HWND OpenDialog();

// Saves opaque pixels, BGRA and top-down, as a PNG. The thread needs COM.
Status SavePixelsPng(int width, int height, const std::vector<uint32_t>& bgra, const std::filesystem::path& file);

// Where Windows shows notifications: the bottom-right corner of the primary
// monitor's work area, as wide as a notification and tall enough for one
// with a few lines of text.
RECT NotificationArea();

}  // namespace knobs::tools
