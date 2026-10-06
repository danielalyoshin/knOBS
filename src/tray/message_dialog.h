// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <windows.h>

#include <string>

// What the tray's windows share: the knob icon, and the task dialog that
// tells the user something, with a Close button (an error, About).
namespace knobs::tray {

// The knob (IDI_KNOBS in `instance`) at LIM_SMALL's or LIM_LARGE's size, as
// an icon for the caller to destroy, or null.
HICON LoadAppIcon(HINSTANCE instance, int size);

struct MessageDialog {
  // The window it's about, which it opens over and keeps from being clicked
  // until it's closed. Null: owned by nobody, so that it gets a taskbar
  // button and can't hide behind other windows unseen.
  HWND owner = nullptr;
  std::wstring title;  // Empty: knobs's name.
  // One of Windows' icons (TD_WARNING_ICON), or else `custom_icon`.
  PCWSTR icon = nullptr;
  HICON custom_icon = nullptr;
  std::wstring instruction, content, footer;
};

// Shows `dialog`, and returns once it's closed.
void ShowMessageDialog(HINSTANCE instance, const MessageDialog& dialog);

}  // namespace knobs::tray
