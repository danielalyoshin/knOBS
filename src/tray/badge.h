// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <vector>

// The tray icon's badges (docs/design.md, Tray and first run): the knob alone
// while knobs runs, a pause badge while it's paused, and a "!" while the cable
// gets no mic and that needs the user. Notifications can be hidden (Do Not
// Disturb), so the icon shows the state too. They're discs in the logo's
// colors: a white "!" on its red, and white pause bars on the knob's graphite,
// the same on a light and a dark taskbar. The badge is drawn over the icon's
// bottom-right corner, 7/16 of its size, with a clear ring that lets the
// taskbar show between the two. The knob itself isn't recolored.
namespace knobs::tray {

enum class Badge { kNone, kPaused, kAttention };

// An icon's pixels: BGRA, top-down, straight alpha, as icons hold them.
struct IconPixels {
  int size = 0;  // Width and height.
  std::vector<uint32_t> bgra;
};

// `icon` with `badge`.
IconPixels AddBadge(IconPixels icon, Badge badge);

// AddBadge on a Windows icon. Returns a new icon for the caller to destroy,
// or null.
HICON BadgedIcon(HICON icon, Badge badge);

// A square icon's pixels, or nullopt.
std::optional<IconPixels> ReadIcon(HICON icon);
// An icon from pixels, for the caller to destroy, or null.
HICON CreateIconFrom(const IconPixels& pixels);

}  // namespace knobs::tray
