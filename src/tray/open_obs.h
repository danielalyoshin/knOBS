// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>

#include "util/result.h"

namespace knobs::tray {

// Starts OBS from its install, in its own bin\64bit, as its shortcut does:
// this process's working directory is libobs's runtime copy.
Status OpenObs(const std::filesystem::path& install_root);

}  // namespace knobs::tray
