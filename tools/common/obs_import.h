// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "import/mic_import.h"
#include "import/obs_config.h"
#include "runtime/obs_host.h"
#include "util/result.h"

// How the tools start libobs and import the mic from OBS's settings. Each
// step is reported on the console as it goes.
namespace knobs::tools {

struct ImportArgs {
  // OBS's settings folder, the one holding obs-studio\. Default: %AppData%,
  // or the install's config\ folder if it's portable.
  std::optional<std::filesystem::path> config_dir;
  // Which mic, by number or name (import::PickMic).
  std::string pick;

  // Whether any was given: they only mean something with an import.
  bool given() const { return config_dir || !pick.empty(); }
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

struct ToolStartOptions {
  std::optional<std::filesystem::path> obs_dir;  // --obs-dir
  std::wstring log_prefix;                       // See runtime::HostOptions.
  bool verbose = false;
  // Import the mic: find OBS's active profile, and run libobs at its sample
  // rate and channel layout, as OBS does.
  bool import = false;
  ImportArgs import_args;
};

struct StartedTool {
  std::unique_ptr<runtime::ObsHost> host;
  std::optional<import::ActiveObsConfig> config;  // When importing.
};

// Finds the OBS install, finds OBS's active profile when importing, and
// starts libobs. If that fails, it's reported and the result is the exit
// code to end with: kExitSkip when OBS isn't installed, kExitFail otherwise.
std::variant<StartedTool, int> StartTool(const ToolStartOptions& options);

// Once libobs runs: reads the active scene collection, picks the mic and
// runs the pre-flight checks.
Result<import::ImportedMic> ImportMic(const runtime::ObsApi& api, const import::ActiveObsConfig& config,
                                      const ImportArgs& args);

}  // namespace knobs::tools
