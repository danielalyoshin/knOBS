// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string_view>

#include "runtime/obs_layout.h"
#include "util/result.h"

namespace knobs::tray {

// Starts OBS from its install, in its own bin\64bit, as its shortcut does:
// this process's working directory is libobs's runtime copy. Tests start a
// stand-in for OBS under a name of its own (`exe`), so that a knobs or an OBS
// that's running doesn't take it for OBS.
Status OpenObs(const std::filesystem::path& install_root, std::wstring_view exe = runtime::kObsExe);

}  // namespace knobs::tray
