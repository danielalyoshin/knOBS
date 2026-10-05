// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string>

#include "util/result.h"

namespace knobs {

// The whole file, as is. An error if it can't be opened or read.
Result<std::string> ReadText(const std::filesystem::path& file);

}  // namespace knobs
