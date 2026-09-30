// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <concepts>
#include <filesystem>
#include <string>
#include <string_view>

namespace knobs {

std::string ToUtf8(std::wstring_view wide);
// Only for actual paths; strings would convert to both.
template <typename Path>
  requires std::same_as<Path, std::filesystem::path>
std::string ToUtf8(const Path& path) {
  return ToUtf8(std::wstring_view(path.native()));
}
std::wstring FromUtf8(std::string_view utf8);

// UTF-8 with forward slashes, the form libobs expects for paths.
std::string ToObsPath(const std::filesystem::path& path);

// ASCII-only lowercase, for comparing DLL names.
std::string AsciiLower(std::string_view text);

// Windows' description of a Win32 error code, plus the code itself.
std::string DescribeWinError(unsigned long code);

}  // namespace knobs
