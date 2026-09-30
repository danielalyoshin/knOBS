// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "util/result.h"

namespace knobs::runtime {

// Names of the DLLs a 64-bit PE file imports, normal imports first, then
// delay-loaded ones, as spelled in the file. Reads the file through a
// read-only mapping; nothing is loaded or executed.
Result<std::vector<std::string>> ReadDllImports(const std::filesystem::path& file);

}  // namespace knobs::runtime
