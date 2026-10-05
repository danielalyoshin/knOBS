// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

#include "util/result.h"

// "Start with Windows": a value under HKCU's Run key, which Windows runs at
// sign-in unless it's turned off in Task Manager's Startup apps. Task
// Manager keeps that switch in Explorer\StartupApproved\Run, as a binary
// value whose first byte is odd while the entry is off.
namespace knobs::tray {

struct RunEntry {
  std::wstring name;     // The value's name.
  std::wstring command;  // "C:\…\knobs.exe" --startup
  // Under HKEY_CURRENT_USER. Tests point these elsewhere.
  std::wstring run_key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
  std::wstring approved_key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
};

// Whether Windows will run `entry.command` at sign-in: the value holds that
// command, ignoring case, and Task Manager hasn't turned it off.
bool StartsWithWindows(const RunEntry& entry);

// Writes or deletes the value. Turning it on also clears Task Manager's
// switch for it, since asking for it in knobs is as explicit as turning it
// on there; turning it off deletes both.
Status SetStartWithWindows(const RunEntry& entry, bool on);

}  // namespace knobs::tray
