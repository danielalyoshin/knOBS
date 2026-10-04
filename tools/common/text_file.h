// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string>

#include "util/result.h"

namespace knobs::tools {

// The whole file, as is.
Result<std::string> ReadText(const std::filesystem::path& file);

}  // namespace knobs::tools
