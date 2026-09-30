// SPDX-License-Identifier: GPL-2.0-or-later
//
// knobs-smoke: checks the M0 runtime bootstrap end to end. Finds OBS, makes
// sure the runtime copy is intact, loads obs.dll from it, starts libobs with
// the two modules and shuts down, reporting each step.
//
// Mic capture is opt-in (--capture-seconds). Without it the mic is never
// opened. With it, captured audio stays in memory: only the frame count and
// peak level are kept, and nothing is recorded or played back.

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app_info.h"
#include "common/console.h"
#include "runtime/obs_install.h"
#include "runtime/obs_layout.h"
#include "runtime/obs_log.h"
#include "runtime/obs_runtime.h"
#include "runtime/obs_session.h"
#include "runtime/runtime_copy.h"
#include "util/app_dirs.h"
#include "util/win_strings.h"

namespace {

using namespace knobs;
using namespace knobs::runtime;
using namespace knobs::tools;
namespace fs = std::filesystem;

constexpr double kMaxCaptureSeconds = 60;
constexpr size_t kKeptLogs = 20;

std::string Usage() {
  return std::format(R"(Usage: knobs-smoke [options]

Checks that {} can run the installed OBS's libobs from its runtime copy.

  --obs-dir <folder>      Use this OBS install instead of searching for one.
  --pick-obs-dir          Choose the OBS install with a folder picker.
  --refresh-runtime       Recopy the runtime even if an intact copy exists.
  --list-files            Print the files the runtime copy needs.
  --video none|dummy      Start without video (default) or with the dummy canvas.
  --capture-seconds <n>   Opt in to capturing the default mic for n seconds (at most
                          {:g}) through a gain filter. Off by default. Only the frame
                          count and peak level are kept; nothing is recorded.
  --verbose               Echo the libobs log, including debug lines.
)",
                     kDisplayName, kMaxCaptureSeconds);
}

struct Options {
  std::optional<fs::path> obs_dir;
  bool pick_obs_dir = false;
  bool refresh_runtime = false;
  bool list_files = false;
  VideoMode video = VideoMode::kNone;
  double capture_seconds = 0;  // Mic capture is opt-in.
  bool verbose = false;
};

std::optional<Options> ParseArgs(int argc, wchar_t** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view arg = argv[i];
    const wchar_t* value = i + 1 < argc ? argv[i + 1] : nullptr;
    if (arg == L"--obs-dir" && value) {
      options.obs_dir = fs::absolute(value);
      ++i;
    } else if (arg == L"--pick-obs-dir") {
      options.pick_obs_dir = true;
    } else if (arg == L"--refresh-runtime") {
      options.refresh_runtime = true;
    } else if (arg == L"--list-files") {
      options.list_files = true;
    } else if (arg == L"--video" && value && (value == std::wstring_view(L"none") ||
                                              value == std::wstring_view(L"dummy"))) {
      options.video = value == std::wstring_view(L"dummy") ? VideoMode::kDummy : VideoMode::kNone;
      ++i;
    } else if (arg == L"--capture-seconds" && value) {
      wchar_t* end = nullptr;
      options.capture_seconds = std::wcstod(value, &end);
      if (*end != L'\0' || !std::isfinite(options.capture_seconds) ||
          options.capture_seconds < 0 || options.capture_seconds > kMaxCaptureSeconds) {
        return std::nullopt;
      }
      ++i;
    } else if (arg == L"--verbose") {
      options.verbose = true;
    } else {
      return std::nullopt;
    }
  }
  return options;
}

double Megabytes(uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

// --- Process inspection --------------------------------------------------------

std::wstring Canonical(const fs::path& path) {
  std::error_code ec;
  return fs::weakly_canonical(fs::absolute(path, ec), ec).wstring();
}

bool IsUnder(const fs::path& path, const fs::path& root) {
  const std::wstring p = Canonical(path);
  const std::wstring r = Canonical(root);
  return p.size() > r.size() && (p[r.size()] == L'\\' || p[r.size()] == L'/') &&
         CompareStringOrdinal(p.c_str(), static_cast<int>(r.size()), r.c_str(),
                              static_cast<int>(r.size()), TRUE) == CSTR_EQUAL;
}

struct LoadedModules {
  std::vector<std::string> from_runtime;
  std::vector<std::string> from_install;
};

LoadedModules ListLoadedModules(const fs::path& runtime_root, const fs::path& install_root) {
  LoadedModules result;
  HMODULE modules[1024];
  DWORD needed = 0;
  if (!EnumProcessModulesEx(GetCurrentProcess(), modules, sizeof(modules), &needed,
                            LIST_MODULES_ALL)) {
    return result;
  }
  const size_t count = std::min<size_t>(needed / sizeof(HMODULE), std::size(modules));
  for (size_t i = 0; i < count; ++i) {
    wchar_t name[MAX_PATH * 2];
    if (!GetModuleFileNameW(modules[i], name, static_cast<DWORD>(std::size(name)))) continue;
    if (IsUnder(name, runtime_root)) result.from_runtime.push_back(ToUtf8(fs::path(name).filename()));
    if (IsUnder(name, install_root)) result.from_install.push_back(ToUtf8(std::wstring_view(name)));
  }
  return result;
}

struct ProcessSample {
  std::chrono::steady_clock::time_point at;
  double cpu_seconds = 0;
  uint64_t working_set = 0;
  uint64_t private_bytes = 0;
  unsigned threads = 0;
};

ProcessSample SampleProcess() {
  ProcessSample sample;
  sample.at = std::chrono::steady_clock::now();
  FILETIME created, exited, kernel, user;
  if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
    auto seconds = [](FILETIME t) {
      return static_cast<double>((uint64_t{t.dwHighDateTime} << 32) | t.dwLowDateTime) / 1e7;
    };
    sample.cpu_seconds = seconds(kernel) + seconds(user);
  }
  PROCESS_MEMORY_COUNTERS_EX memory = {};
  memory.cb = sizeof(memory);
  if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                           sizeof(memory))) {
    sample.working_set = memory.WorkingSetSize;
    sample.private_bytes = memory.PrivateUsage;
  }
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot != INVALID_HANDLE_VALUE) {
    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
      if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++sample.threads;
    }
    CloseHandle(snapshot);
  }
  return sample;
}

// --- Checks ------------------------------------------------------------------

void CheckModuleData(const ObsApi& api, ObsSession& session, const fs::path& runtime_root) {
  // Keys whose en-US text differs from the key, so a hit proves the locale
  // file was found and parsed.
  constexpr std::pair<std::string_view, const char*> kLocaleProbes[] = {
      {"win-wasapi", "AudioInput"},
      {"obs-filters", "NoiseGate"},
  };
  for (const auto& [name, key] : kLocaleProbes) {
    obs_module_t* module = session.module(name);
    char* file = api.obs_find_module_file(module, "locale/en-US.ini");
    const char* text = nullptr;
    const bool in_copy = file && IsUnder(FromUtf8(file), runtime_root);
    const bool found = api.obs_module_get_locale_string(module, key, &text);
    Check(in_copy && found, std::format("module {}", name),
          in_copy && found ? std::format("{} = \"{}\"", key, text)
          : !in_copy       ? std::format("locale file not in the runtime copy: {}", file ? file : "(none)")
                           : std::format("locale key {} not found", key));
    api.bfree(file);
  }

  // Relative to the working directory, which ObsRuntime::Load set.
  char* effect = api.obs_find_data_file("default.effect");
  const bool in_copy = effect && IsUnder(FromUtf8(effect), runtime_root);
  Check(in_copy, "libobs data",
        effect ? std::format("default.effect -> {}", ToUtf8(Canonical(FromUtf8(effect))))
               : "default.effect not found");
  api.bfree(effect);
}

struct CaptureState {
  std::mutex mutex;
  uint64_t frames = 0;
  float peak = 0;
};

void OnAudio(void* param, obs_source_t*, const audio_data* audio, bool) {
  auto* state = static_cast<CaptureState*>(param);
  float peak = 0;
  // Planar float after libobs's resampler; stereo, per the session options.
  for (size_t channel = 0; channel < 2 && audio->data[channel]; ++channel) {
    const auto* samples = reinterpret_cast<const float*>(audio->data[channel]);
    for (uint32_t i = 0; i < audio->frames; ++i) peak = std::max(peak, std::fabs(samples[i]));
  }
  std::lock_guard lock(state->mutex);
  state->frames += audio->frames;
  state->peak = std::max(state->peak, peak);
}

// Captures the default recording device through a gain filter, with no video
// pipeline or the dummy one. Only runs when the user opted in.
void CheckCapture(const ObsApi& api, ObsSession& session, const Options& options) {
  obs_data_t* mic_settings = api.obs_data_create();
  api.obs_data_set_string(mic_settings, "device_id", "default");
  obs_source_t* mic = api.obs_source_create_private("wasapi_input_capture", "smoke mic", mic_settings);
  api.obs_data_release(mic_settings);

  obs_data_t* gain_settings = api.obs_data_create();
  api.obs_data_set_double(gain_settings, "db", 0.0);
  obs_source_t* gain = api.obs_source_create_private("gain_filter", "smoke gain", gain_settings);
  api.obs_data_release(gain_settings);

  if (!mic || !gain) {
    Check(false, "audio", std::format("couldn't create {}", !mic ? "wasapi_input_capture" : "gain_filter"));
    api.obs_source_release(gain);
    api.obs_source_release(mic);
    return;
  }

  CaptureState state;
  api.obs_source_filter_add(mic, gain);
  api.obs_source_add_audio_capture_callback(mic, OnAudio, &state);
  api.obs_source_inc_active(mic);

  const ProcessSample before = SampleProcess();
  std::this_thread::sleep_for(std::chrono::duration<double>(options.capture_seconds));
  const ProcessSample after = SampleProcess();

  api.obs_source_dec_active(mic);
  api.obs_source_remove_audio_capture_callback(mic, OnAudio, &state);
  api.obs_source_filter_remove(mic, gain);
  api.obs_source_release(gain);
  api.obs_source_release(mic);
  session.DrainDestroyQueue();

  const double elapsed = std::chrono::duration<double>(after.at - before.at).count();
  {
    std::lock_guard lock(state.mutex);
    const std::string level = state.peak > 0 ? std::format("{:.1f} dBFS", 20 * std::log10(state.peak))
                                             : std::string("silence");
    Check(state.frames > 0, "audio",
          state.frames > 0
              ? std::format("{} frames in {:.1f} s after gain_filter, peak {}", state.frames, elapsed, level)
              : "no audio from the default recording device (is one connected?)");
  }

  Report(Outcome::kNote, "cost",
         std::format("{:.2f}% of one core while capturing, working set {:.1f} MB, private "
                     "{:.1f} MB, {} threads",
                     100.0 * (after.cpu_seconds - before.cpu_seconds) / elapsed,
                     Megabytes(after.working_set), Megabytes(after.private_bytes), after.threads));
}

void OnActivate(void* param, calldata_t*) { static_cast<std::atomic<bool>*>(param)->store(true); }

// Whether sources get their activate callback, which libobs only fires from
// the video tick. Uses libobs's built-in audio_line source, which opens no
// device.
void CheckActivation(const ObsApi& api, ObsSession& session, VideoMode video) {
  obs_source_t* source = api.obs_source_create_private("audio_line", "smoke activation", nullptr);
  if (!source) {
    Check(false, "activation", "couldn't create an audio_line source");
    return;
  }
  std::atomic<bool> activated = false;
  signal_handler_t* signals = api.obs_source_get_signal_handler(source);
  api.signal_handler_connect(signals, "activate", OnActivate, &activated);
  api.obs_source_inc_active(source);

  // The dummy canvas ticks once a second. Without video, wait longer than a
  // tick would take, to show that none comes.
  using namespace std::chrono_literals;
  const auto deadline =
      std::chrono::steady_clock::now() + (video == VideoMode::kDummy ? 3000ms : 1500ms);
  while (!activated && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(50ms);
  }

  api.obs_source_dec_active(source);
  api.signal_handler_disconnect(signals, "activate", OnActivate, &activated);
  api.obs_source_release(source);
  session.DrainDestroyQueue();

  if (video == VideoMode::kDummy) {
    Check(activated, "activation",
          activated ? "activate fired from the video tick"
                    : "activate never fired, even with the dummy video tick");
  } else {
    Report(Outcome::kNote, "activation",
           activated ? "activate fired without video (unexpected)"
                     : "activate never fires without a video tick, so win-wasapi's "
                       "reconnect thread wouldn't run");
  }
}

// The invariant: nothing is loaded from the OBS install folder.
void CheckDllOrigin(const fs::path& runtime_root, const fs::path& install_root) {
  const LoadedModules loaded = ListLoadedModules(runtime_root, install_root);
  std::string names;
  for (const std::string& name : loaded.from_runtime) names += " " + name;
  Check(loaded.from_install.empty(), "DLL origin",
        loaded.from_install.empty()
            ? std::format("none from the OBS install; from the runtime copy:{}", names)
            : std::format("loaded from the OBS install: {}", loaded.from_install.front()));
}

// Keeps libobs logging into `log` until the session is gone.
struct LogAttachment {
  const ObsApi& api;
  ObsLog& log;
  LogAttachment(const ObsApi& a, ObsLog& l) : api(a), log(l) { log.Attach(api); }
  ~LogAttachment() { log.Detach(api); }
};

int Run(const Options& options) {
  Print(std::format("{} runtime smoke test\n", kDisplayName));

  auto dirs = GetAppDirs();
  if (!dirs) {
    Check(false, "app folders", dirs.error());
    return kExitFail;
  }

  // Find OBS.
  const bool explicit_dir = options.obs_dir || options.pick_obs_dir;
  std::optional<fs::path> chosen = options.obs_dir;
  if (options.pick_obs_dir) {
    chosen = PickObsInstallFolder(nullptr);
    if (!chosen) {
      Check(false, "find OBS", "no folder chosen");
      return kExitFail;
    }
  }
  auto install = chosen ? InspectObsInstall(*chosen) : FindObsInstall();
  if (!install) {
    // Skip (for CTest) only when OBS isn't installed at all. A broken install
    // is a failure.
    if (!explicit_dir && ObsInstallCandidates().empty()) {
      Report(Outcome::kNote, "find OBS", install.error());
      Print("SKIP (OBS isn't installed)\n");
      return kExitSkip;
    }
    Check(false, "find OBS", install.error());
    return kExitFail;
  }
  Check(true, "find OBS", std::format("OBS {} in {}", install->version.ToString(), ToUtf8(install->root)));

  if (options.list_files) {
    auto file_set = PlanRuntimeCopy(install->root);
    if (!file_set) {
      Check(false, "file set", file_set.error());
      return kExitFail;
    }
    Print(std::format("       {} files, {:.1f} MB:\n", file_set->files.size(), Megabytes(file_set->total_bytes)));
    for (const fs::path& file : file_set->files) Print(std::format("         {}\n", ToObsPath(file)));
    std::string system;
    for (const std::string& dll : file_set->system_dlls) system += " " + dll;
    Print(std::format("       Imported from the system, not copied:{}\n", system));
  }

  // Runtime copy.
  auto copy = EnsureRuntimeCopy(*install, dirs->RuntimeBase(), options.refresh_runtime);
  if (!copy) {
    Check(false, "runtime copy", copy.error());
    return kExitFail;
  }
  Check(true, "runtime copy",
        std::format("{} ({} {} files, {:.1f} MB)", ToUtf8(copy->root), copy->reused ? "reused," : "copied",
                    copy->file_count, Megabytes(copy->total_bytes)));

  auto log = OpenNewLog(dirs->Logs(), L"smoke ", kKeptLogs);
  if (!log) {
    Check(false, "log", log.error());
    return kExitFail;
  }
  (*log)->set_echo(options.verbose);
  (*log)->set_verbose(options.verbose);

  // Load obs.dll from the copy.
  auto runtime = ObsRuntime::Load(copy->root);
  if (!runtime) {
    Check(false, "load obs.dll", runtime.error());
    return kExitFail;
  }
  const ObsApi& api = (*runtime)->api();
  Check(true, "load obs.dll",
        std::format("libobs {}, {} functions resolved, supported ({})", api.obs_get_version_string(),
                    ObsApiSize(), DescribeSupportedObsVersions()));

  {
    LogAttachment attachment(api, **log);
    (*log)->Write(LOG_INFO, std::format("{} smoke test, runtime {}", kDisplayName, ToUtf8(copy->root)));

    SessionOptions session_options;
    session_options.module_config_dir = dirs->ModuleConfig();
    session_options.video = options.video;
    auto session = ObsSession::Start(**runtime, session_options);
    if (!session) {
      Check(false, "start libobs", session.error());
    } else {
      Check(true, "start libobs",
            options.video == VideoMode::kNone ? "48 kHz stereo, no video (no obs_reset_video)"
                                              : "48 kHz stereo, dummy video 8x8 @ 1 fps");
      CheckModuleData(api, **session, copy->root);
      CheckActivation(api, **session, options.video);
      if (options.capture_seconds > 0) {
        Report(Outcome::kNote, "mic",
               std::format("opening the default recording device for {:g} s", options.capture_seconds));
        CheckCapture(api, **session, options);
      } else {
        Report(Outcome::kNote, "audio", "mic not opened (opt in with --capture-seconds <n>)");
      }
      CheckDllOrigin(copy->root, install->root);

      const long leaks = (*session)->Shutdown();
      Check(leaks == 0, "shutdown",
            leaks == 0 ? "obs_shutdown clean, 0 leaked allocations"
                       : std::format("{} libobs allocations leaked", leaks));
    }
  }

  Report(Outcome::kNote, "libobs log",
         std::format("{} ({} warnings/errors)", ToUtf8((*log)->path()), (*log)->problem_count()));
  const auto problems = (*log)->RecentProblems();
  const size_t shown = std::min<size_t>(problems.size(), 20);
  for (size_t i = problems.size() - shown; i < problems.size(); ++i) {
    Print(std::format("         {}\n", problems[i]));
  }

  Print(AnyFailed() ? "FAIL\n" : "PASS\n");
  return AnyFailed() ? kExitFail : kExitPass;
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
