// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "import/mic_import.h"
#include "import/obs_config.h"
#include "runtime/obs_host.h"
#include "runtime/obs_install.h"
#include "util/result.h"

// Importing the mic from OBS's settings, for the tools that take --import.
// Each step is reported on the console as it goes.
namespace knobs::tools {

struct ImportArgs {
  // OBS's settings folder, the one holding obs-studio\. Default: %AppData%,
  // or the install's config\ folder if it's portable.
  std::optional<std::filesystem::path> config_dir;
  // Which mic, by number or name (import::PickMic).
  std::string pick;
};

// Usage lines for the options above.
inline constexpr std::string_view kImportUsage =
    R"(  --obs-config <folder>    OBS's settings folder, the one holding obs-studio\.
                           Default: %AppData%, or the install's config\ folder for a
                           portable OBS.
  --pick <n|name>          Which mic to import when the collection has several: its
                           number in the list, or its name.)";

// Handles --obs-config and --pick. Returns false if `arg` is neither, and
// sets `bad` if it is one but `value` is missing.
bool ParseImportArg(std::wstring_view arg, const wchar_t* value, ImportArgs& args, bool& bad);

// Before libobs starts: finds OBS's active profile and scene collection.
Result<import::ActiveObsConfig> FindObsConfig(const ImportArgs& args, const runtime::ObsInstall& install);

// Runs libobs at the profile's sample rate and channel layout, as OBS does.
void UseProfileAudio(const import::ActiveObsConfig& config, runtime::HostOptions& options);

// Once libobs runs: reads the collection, picks the mic and runs the
// pre-flight checks.
Result<import::ImportedMic> ImportMic(const runtime::ObsApi& api, const import::ActiveObsConfig& config,
                                      const ImportArgs& args);

}  // namespace knobs::tools
