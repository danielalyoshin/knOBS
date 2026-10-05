// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <vector>

#include "util/result.h"

// Pictures of knobs-tray's menu, for checking how it looks.
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

}  // namespace knobs::tools
