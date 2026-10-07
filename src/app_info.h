// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#ifndef RC_INVOKED
#include <string_view>
#endif

// The display name: "knobs", all lowercase (it was "knOBS" until 2026-10-05).
// This is its only definition, in case it changes again (docs/design.md, The
// name). It also names knobs's %AppData% and %LocalAppData% folders; Windows
// paths are case-insensitive, so a rename that only changes case keeps them.
#define KNOBS_DISPLAY_NAME "knobs"

// The resource compiler reads this file too, for the name.
#ifndef RC_INVOKED

namespace knobs {

inline constexpr std::string_view kDisplayName = KNOBS_DISPLAY_NAME;
inline constexpr std::wstring_view kDisplayNameW = L"" KNOBS_DISPLAY_NAME;

// The setup guide: the README's Setup section, which the first run links to.
inline constexpr std::string_view kSetupGuideUrl = "https://github.com/danielalyoshin/knobs#setup";

}  // namespace knobs

#endif  // RC_INVOKED
