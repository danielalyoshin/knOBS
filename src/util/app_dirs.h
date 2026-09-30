// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>

#include "util/result.h"

namespace knobs {

// knOBS's own state. %AppData%\obs-studio is read-only to knOBS; nothing
// here points into it.
struct AppDirs {
  std::filesystem::path roaming;  // %AppData%\knOBS
  std::filesystem::path local;    // %LocalAppData%\knOBS

  // Shadow copies of the user's OBS runtime, one folder per OBS version.
  std::filesystem::path RuntimeBase() const { return local / L"runtime"; }
  std::filesystem::path Logs() const { return local / L"logs"; }
  // Passed to obs_startup() as module_config_path.
  std::filesystem::path ModuleConfig() const { return roaming / L"module-config"; }
};

// Resolves the folders. Doesn't create them.
Result<AppDirs> GetAppDirs();

}  // namespace knobs
