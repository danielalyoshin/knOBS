// SPDX-License-Identifier: GPL-2.0-or-later
//
// knobs-import: imports the mic from OBS's settings the way knOBS will, and
// reports each step: the active profile and scene collection, the mics in
// it, the pre-flight checks, and the chain libobs's loader builds from it.
//
// OBS's settings are only read. No audio device is opened: the chain loads as
// a push source (common/push_source.h) instead of the mic's own source.

#include <windows.h>

#include <cwchar>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include "app_info.h"
#include "audio/audio_devices.h"
#include "audio/live_chain.h"
#include "common/console.h"
#include "common/obs_import.h"
#include "common/push_source.h"
#include "runtime/obs_host.h"
#include "runtime/obs_install.h"
#include "util/win_strings.h"

namespace {

using namespace knobs;
using namespace knobs::tools;
namespace fs = std::filesystem;

std::string Usage() {
  return std::format(R"(Usage: knobs-import [options]

Imports the mic from OBS's active profile and scene collection the way {} will:
finds them, lists the mics, runs the pre-flight checks and loads the mic's
filter chain with libobs's own loader. OBS's settings are only read, and no
audio device is opened.

{}
  --save <file.json>       Write the mic's source object as {} loads it, e.g. for
                           knobs-harness --source. The load callbacks a mic from
                           "sources" gets aren't recorded in it; --import is exact.
  --obs-dir <folder>       Use this OBS install instead of searching for one.
  --verbose                Echo the libobs log, including debug lines.
)",
                     kDisplayName, kImportUsage, kDisplayName);
}

struct Options {
  ImportArgs import_args;
  std::optional<fs::path> save;
  std::optional<fs::path> obs_dir;
  bool verbose = false;
};

std::optional<Options> ParseArgs(int argc, wchar_t** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view arg = argv[i];
    const wchar_t* value = i + 1 < argc ? argv[i + 1] : nullptr;
    if (arg == L"--verbose") {
      options.verbose = true;
      continue;
    }
    bool bad = false;
    if (ParseImportArg(arg, value, options.import_args, bad)) {
      if (bad) return std::nullopt;
    } else if (!value) {
      return std::nullopt;
    } else if (arg == L"--save") {
      options.save = fs::absolute(value);
    } else if (arg == L"--obs-dir") {
      options.obs_dir = fs::absolute(value);
    } else {
      return std::nullopt;
    }
    ++i;
  }
  return options;
}

// Says which devices the import refers to, by name, and warns about missing
// ones. Lists them without opening any.
void ReportDevices(const runtime::ObsApi& api, const import::ImportedMic& imported,
                   const import::ActiveObsConfig& config) {
  const auto mics = audio::ListMicDevices(api);
  const auto mic = audio::FindDevice(mics, imported.mic.device_id, "recording device");
  if (mic) {
    Check(true, "mic device", std::format("\"{}\"", mic->name));
  } else {
    Report(Outcome::kWarn, "mic device", std::format("{} isn't connected", imported.mic.device_id));
  }
  const auto outputs = audio::ListMonitoringDevices(api);
  const auto output = audio::FindDevice(outputs, config.audio.monitoring_device_id, "playback device");
  if (output) {
    Check(true, "monitoring device", std::format("\"{}\"", output->name));
  } else {
    Report(Outcome::kWarn, "monitoring device",
           std::format("\"{}\" ({}) isn't connected", config.audio.monitoring_device_name,
                       config.audio.monitoring_device_id));
  }
}

int Run(const Options& options) {
  Print(std::format("{} import\n", kDisplayName));
  if (!options.obs_dir && runtime::ObsInstallCandidates().empty()) {
    Report(Outcome::kNote, "find OBS", "OBS isn't installed");
    Print("SKIP (OBS isn't installed)\n");
    return kExitSkip;
  }
  auto install = options.obs_dir ? runtime::InspectObsInstall(*options.obs_dir) : runtime::FindObsInstall();
  if (!install) {
    Check(false, "find OBS", install.error());
    return kExitFail;
  }
  auto config = FindObsConfig(options.import_args, *install);
  if (!config) {
    Check(false, "OBS settings", config.error());
    return kExitFail;
  }

  runtime::HostOptions host_options;
  host_options.obs_dir = install->root;
  host_options.log_prefix = L"import ";
  host_options.verbose = options.verbose;
  UseProfileAudio(*config, host_options);
  auto host = runtime::ObsHost::Start(host_options);
  if (!host) {
    Check(false, "start libobs", host.error());
    return kExitFail;
  }
  const runtime::ObsApi& api = (*host)->api();
  Check(true, "start libobs", std::format("OBS {}, no video, no audio devices", install->version.ToString()));

  auto imported = ImportMic(api, *config, options.import_args);
  if (!imported) {
    Check(false, "import", imported.error());
    return FinishRun(**host);
  }
  RegisterPushSource(api);
  auto source = audio::LoadSourceJson(
      api, imported->source_json, {.type_id = kPushSourceId, .load_callbacks = imported->load_callbacks()});
  if (!source) {
    Check(false, "load", source.error());
    return FinishRun(**host);
  }
  ReportChain(audio::DescribeChain(api, *source));
  api.obs_source_release(*source);
  (*host)->session().DrainDestroyQueue();
  Check(true, "load",
        std::format("libobs loaded the chain{}; {} warning(s)",
                    imported->load_callbacks() ? " and ran its load callbacks" : "", imported->warning_count()));
  ReportDevices(api, *imported, *config);

  if (options.save) {
    std::ofstream out(*options.save, std::ios::binary | std::ios::trunc);
    out << imported->source_json;
    out.close();
    Check(static_cast<bool>(out), "save",
          out ? ToUtf8(*options.save) : std::format("couldn't write {}", ToUtf8(*options.save)));
  }
  return FinishRun(**host);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // Plain LoadLibrary calls skip PATH and the working directory; see
  // ObsRuntime::Load.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  SetConsoleOutputCP(CP_UTF8);
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == std::wstring_view(L"--help") || argv[i] == std::wstring_view(L"-h")) {
      Print(Usage());
      return kExitPass;
    }
  }
  const auto options = ParseArgs(argc, argv);
  if (!options) {
    Print(Usage());
    return kExitUsage;
  }
  return Run(*options);
}
