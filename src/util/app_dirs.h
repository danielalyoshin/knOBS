// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string_view>

#include "util/result.h"

namespace knobs {

// knobs's own state. %AppData%\obs-studio is read-only to knobs; nothing
// here points into it.
struct AppDirs {
  std::filesystem::path roaming;  // %AppData%\knobs, or <exe folder>\data
  std::filesystem::path local;    // %LocalAppData%\knobs, or <exe folder>\data
  bool portable = false;          // Kept beside the exe (kPortableMarker).

  // Shadow copies of the user's OBS runtime, one folder per OBS version.
  std::filesystem::path RuntimeBase() const { return local / L"runtime"; }
  std::filesystem::path Logs() const { return local / L"logs"; }
  // Passed to obs_startup() as module_config_path.
  std::filesystem::path ModuleConfig() const { return roaming / L"module-config"; }
};

// A file beside the exe that makes knobs portable, as OBS's file of the same
// name makes OBS portable: then knobs keeps all of its state in a data folder
// beside the exe, and nothing in %AppData% or %LocalAppData%. The portable
// zip ships with it.
inline constexpr std::wstring_view kPortableMarker = L"portable_mode.txt";

// The folders for a program in `exe_folder`: <exe_folder>\data when the
// folder holds kPortableMarker, else knobs's folders in `roaming_app_data`
// and `local_app_data` (%AppData% and %LocalAppData%).
AppDirs AppDirsFor(const std::filesystem::path& exe_folder, const std::filesystem::path& roaming_app_data,
                   const std::filesystem::path& local_app_data);

// The folders for the running program. Doesn't create them.
Result<AppDirs> GetAppDirs();

// The user's %AppData% itself, where OBS keeps its settings. Portable or not,
// knobs reads OBS's settings from there.
Result<std::filesystem::path> UserAppData();

}  // namespace knobs
