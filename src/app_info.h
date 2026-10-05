// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string_view>

// The display name: "knobs", all lowercase (it was "knOBS" until 2026-10-05).
// This is its only definition, in case it changes again (see plan.md). It also
// names knobs's %AppData% and %LocalAppData% folders; Windows paths are
// case-insensitive, so a rename that only changes case keeps them.
#define KNOBS_DISPLAY_NAME "knobs"

namespace knobs {

inline constexpr std::string_view kDisplayName = KNOBS_DISPLAY_NAME;
inline constexpr std::wstring_view kDisplayNameW = L"" KNOBS_DISPLAY_NAME;

}  // namespace knobs
