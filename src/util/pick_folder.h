// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace knobs {

// Asks the user for a folder, with Windows' folder picker. `owner` is an
// HWND or null. Returns nullopt if they cancel. Leaves the working directory
// alone, which libobs depends on (runtime::ObsRuntime::Load). Call from a
// thread that can host a COM single-threaded apartment.
std::optional<std::filesystem::path> PickFolder(void* owner, std::wstring_view title);

}  // namespace knobs
