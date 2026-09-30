// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string_view>

// The display name may become "Knobs" before release (see plan.md), so this
// is its only definition. It also names knOBS's %AppData% and %LocalAppData%
// folders; Windows paths are case-insensitive, so a rename keeps them.
#define KNOBS_DISPLAY_NAME "knOBS"

namespace knobs {

inline constexpr std::string_view kDisplayName = KNOBS_DISPLAY_NAME;
inline constexpr std::wstring_view kDisplayNameW = L"" KNOBS_DISPLAY_NAME;

}  // namespace knobs
