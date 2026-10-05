// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "util/result.h"

namespace knobs {

// The whole file, as is. An error if it can't be opened or read.
Result<std::string> ReadText(const std::filesystem::path& file);

// Replaces `file` with `text`, creating its folder if needed. The text goes
// to a temporary file first, which is flushed to disk and renamed over
// `file`, so a reader never finds half of it.
Status WriteText(const std::filesystem::path& file, std::string_view text);

}  // namespace knobs
