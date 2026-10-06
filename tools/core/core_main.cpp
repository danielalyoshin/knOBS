// SPDX-License-Identifier: GPL-2.0-or-later
//
// knobs-core: runs knobs's always-on core (src/core) with no tray, and prints
// its state each time it changes. The core finds OBS, imports the mic, and
// follows OBS starting and exiting and audio devices coming and going.
//
// By default no audio device is opened: the chain loads through libobs's
// loader as a stand-in source with no device (common/push_source.h), and
// nothing is monitored. Only --live opens the mic and the cable, and it says
// so first.

#include <windows.h>
#include <conio.h>
#include <objbase.h>
#include <psapi.h>

#include <atomic>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "app_info.h"
#include "audio/audio_devices.h"
#include "audio/device_watch.h"
#include "audio/live_chain.h"
#include "common/console.h"
#include "common/obs_import.h"
#include "common/push_source.h"
#include "core/core.h"
#include "core/obs_backend.h"
#include "runtime/obs_install.h"
#include "util/app_dirs.h"
#include "util/win_strings.h"

namespace {

using namespace knobs;
using namespace knobs::tools;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

constexpr int kMaxSeconds = 24 * 3600;

std::string Usage() {
  return std::format(R"(Usage: knobs-core [options]

Runs {0}'s always-on core with no tray and prints its state each time it
changes. The core finds OBS, imports the mic from OBS's active profile and
scene collection, and follows OBS (pausing while obs64.exe runs and importing
again when it exits) and audio devices coming and going (rebuilding the chain
when the mic or the cable comes back).

No audio device is opened unless --live is given: the chain loads through OBS's
loader as a stand-in source with no device, and isn't monitored. The stand-in
counts as delivering audio.

Options:
  --live                   Open the mic and send it to the cable, as {0} will.
  --stall <s>              Without --live: the stand-in stops delivering audio s
                           seconds after each start, so the core's watchdog
                           rebuilds it.
  --seconds <s>            Stop after s seconds. Default: run until Ctrl+C or q.
  --ignore-obs             Don't watch for OBS: carry on as if it never runs.
  --cable <name|id>        Send the mic to this playback device. Default: the OBS
                           profile's monitoring device.
{1}
  --obs-dir <folder>       Use this OBS install instead of searching for one.
  --verbose                Echo the libobs log, including debug lines.

Keys while it runs: p pauses, r resumes, i imports again, q quits.
)",
                     kDisplayName, kImportUsage);
}

struct Options {
  bool live = false;
  std::optional<int> stall_seconds;
  int seconds = 0;
  bool ignore_obs = false;
  std::string cable;
  ImportArgs import_args;
  std::optional<fs::path> obs_dir;
  bool verbose = false;
};

std::optional<int> ParseSeconds(const wchar_t* value, int min) {
  wchar_t* end = nullptr;
  const long seconds = std::wcstol(value, &end, 10);
  if (end == value || *end != L'\0' || seconds < min || seconds > kMaxSeconds) return std::nullopt;
  return static_cast<int>(seconds);
}

std::optional<Options> ParseArgs(int argc, wchar_t** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view arg = argv[i];
    const wchar_t* value = i + 1 < argc ? argv[i + 1] : nullptr;
    bool ok = true;
    bool takes_value = true;
    if (arg == L"--live") {
      options.live = true;
      takes_value = false;
    } else if (arg == L"--ignore-obs") {
      options.ignore_obs = true;
      takes_value = false;
    } else if (arg == L"--verbose") {
      options.verbose = true;
      takes_value = false;
    } else if (bool bad = false; ParseImportArg(arg, value, options.import_args, bad)) {
      ok = !bad;
    } else if (!value) {
      ok = false;
    } else if (arg == L"--seconds") {
      const auto seconds = ParseSeconds(value, 1);
      ok = seconds.has_value();
      options.seconds = seconds.value_or(0);
    } else if (arg == L"--stall") {
      options.stall_seconds = ParseSeconds(value, 0);
      ok = options.stall_seconds.has_value();
    } else if (arg == L"--cable") {
      options.cable = ToUtf8(std::wstring_view(value));
    } else if (arg == L"--obs-dir") {
      options.obs_dir = fs::absolute(value);
    } else {
      ok = false;
    }
    if (!ok) return std::nullopt;
    if (takes_value) ++i;
  }
  if (options.live && options.stall_seconds) return std::nullopt;
  return options;
}

std::atomic<bool> g_interrupted = false;

BOOL WINAPI OnConsoleCtrl(DWORD type) {
  if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
  g_interrupted = true;
  return TRUE;
}

// The chain without its devices: libobs's loader builds it from the mic's
// source object as it does for the mic, with the source type swapped for the
// push source, and nothing is monitored. Everything else is the real backend.
class DryBackend : public core::ObsBackend {
 public:
  DryBackend(core::ObsBackendOptions options, std::optional<int> stall_seconds)
      : ObsBackend(std::move(options)), stall_seconds_(stall_seconds) {}
  // Before ObsBackend shuts libobs down.
  ~DryBackend() override { StopChain(); }

  Status StartChain(const core::ChainPlan& plan) override {
    runtime::ObsHost* obs = host();
    if (!obs) return Error{"libobs isn't running."};
    if (!registered_) {
      RegisterPushSource(obs->api());
      registered_ = true;
    }
    StopChain();
    auto source = audio::LoadSourceJson(obs->api(), plan.source_json,
                                        {.type_id = kPushSourceId, .load_callbacks = plan.load_callbacks});
    if (!source) return Error{source.error()};
    source_ = *source;
    started_ = Clock::now();
    Log(std::format("Dry run: loaded the chain as a stand-in source, not monitored to \"{}\".", plan.cable.name));
    return Ok{};
  }

  void StopChain() override {
    if (!source_) return;
    host()->api().obs_source_release(source_);
    host()->session().DrainDestroyQueue();
    source_ = nullptr;
  }

  // 10 ms packets since the start, as win-wasapi delivers them, up to the
  // stall.
  uint64_t ChainPackets() override {
    Clock::duration delivered = Clock::now() - started_;
    if (stall_seconds_) delivered = std::min<Clock::duration>(delivered, std::chrono::seconds(*stall_seconds_));
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(delivered).count() / 10);
  }

 private:
  std::optional<int> stall_seconds_;
  bool registered_ = false;
  obs_source_t* source_ = nullptr;
  Clock::time_point started_;
};

// Prints each snapshot as it comes, with the time since the start.
class PrintingObserver : public core::Observer {
 public:
  void OnSnapshot(const core::Snapshot& snapshot) override {
    std::lock_guard lock(mutex_);
    const double seconds = std::chrono::duration<double>(Clock::now() - start_).count();
    const std::string time = std::format("[{:8.2f} s]", seconds);
    Print(std::format("{} {}\n", time, core::DescribeSnapshot(snapshot)));
    const std::string indent(time.size() + 1, ' ');
    if (!last_ || snapshot.obs_running != last_->obs_running) {
      Print(std::format("{}OBS {}\n", indent, snapshot.obs_running ? "is running" : "isn't running"));
    }
    if (last_ && snapshot.chain_revision != last_->chain_revision && last_->chain_revision > 0) {
      Print(std::format("{}the mic or its chain changed in OBS (revision {})\n", indent, snapshot.chain_revision));
    }
    if (!last_ || snapshot.notes != last_->notes) {
      for (const import::ImportNote& note : snapshot.notes) {
        Print(std::format("{}{}: {}\n", indent, note.warning ? "warning" : "note", note.text));
      }
    }
    if (snapshot.state == core::State::kFailed) failed_ = true;
    if (snapshot.state != core::State::kStarting && !first_) first_ = snapshot.state;
    last_ = snapshot;
  }

  bool failed() {
    std::lock_guard lock(mutex_);
    return failed_;
  }
  std::optional<core::State> first_state() {
    std::lock_guard lock(mutex_);
    return first_;
  }

 private:
  std::mutex mutex_;
  const Clock::time_point start_ = Clock::now();
  std::optional<core::Snapshot> last_;
  std::optional<core::State> first_;
  bool failed_ = false;
};

// The playback device `query` names, by ID or name, as libobs lists them.
Result<std::string> FindCable(std::string_view query) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const audio::Endpoints endpoints = audio::ListEndpoints();
  if (SUCCEEDED(com)) CoUninitialize();
  auto found = audio::FindDevice(endpoints.outputs, query, "playback device");
  if (!found) return Error{found.error()};
  return found->id;
}

double CpuSeconds() {
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
  const auto ticks = [](const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  };
  return static_cast<double>(ticks(kernel) + ticks(user)) / 1e7;
}

int Run(const Options& options) {
  Print(std::format("{} core{}\n", kDisplayName, options.live ? "" : " (dry run: no audio device is opened)"));
  core::Settings settings;
  settings.obs_dir = options.obs_dir;
  settings.obs_config = options.import_args.config_dir;
  settings.mic = options.import_args.pick;
  if (!options.cable.empty()) {
    auto cable = FindCable(options.cable);
    if (!cable) {
      Check(false, "cable", cable.error());
      Print("FAIL\n");
      return kExitFail;
    }
    settings.cable = *cable;
  }
  if (options.live) {
    Report(Outcome::kNote, "live", "opens the mic and sends it to the cable whenever the core runs the chain");
  }

  long leaks = 0;
  core::ObsBackendOptions backend_options;
  backend_options.log_prefix = L"core ";
  backend_options.verbose = options.verbose;
  backend_options.pick_by_number = true;  // --pick takes a number too.
  backend_options.on_shutdown = [&leaks](long live, const runtime::ObsLog& log) {
    leaks = live;
    Check(live == 0, "shutdown", live == 0 ? "0 leaked allocations" : std::format("{} libobs allocations leaked", live));
    Report(Outcome::kNote, "libobs log", std::format("{} ({} warnings/errors)", ToUtf8(log.path()), log.problem_count()));
    for (const std::string& line : log.RecentProblems()) Print(std::format("{:26}{}\n", "", line));
  };
  std::unique_ptr<core::Backend> backend;
  if (options.live) {
    backend = std::make_unique<core::ObsBackend>(std::move(backend_options));
  } else {
    backend = std::make_unique<DryBackend>(std::move(backend_options), options.stall_seconds);
  }

  core::CoreOptions core_options;
  core_options.settings = settings;
  if (options.ignore_obs) core_options.obs_running = [] { return false; };
  PrintingObserver observer;
  const auto start = Clock::now();
  const double cpu_start = CpuSeconds();
  auto started = core::Core::Start(std::move(backend), observer, std::move(core_options));
  if (!started) {
    Check(false, "core", started.error());
    Print("FAIL\n");
    return kExitFail;
  }
  std::unique_ptr<core::Core> core = std::move(*started);

  DWORD mode = 0;
  const bool keys = GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
  const auto deadline = start + std::chrono::seconds(options.seconds);
  bool quit = false;
  while (!quit && !g_interrupted && (options.seconds == 0 || Clock::now() < deadline)) {
    std::this_thread::sleep_for(50ms);
    while (keys && !quit && _kbhit()) {
      switch (std::towlower(static_cast<wint_t>(_getwch()))) {
        case L'p':
          core->Pause();
          break;
        case L'r':
          core->Resume();
          break;
        case L'i':
          core->Reimport();
          break;
        case L'q':
          quit = true;
          break;
        default:
          break;
      }
    }
  }
  core.reset();  // Stops the chain and libobs.
  const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
  const double cpu = CpuSeconds() - cpu_start;
  PROCESS_MEMORY_COUNTERS memory = {sizeof(memory)};
  GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
  Report(Outcome::kNote, "cost",
         std::format("{:.2f} s of CPU in {:.1f} s ({:.2f}% of one core), peak working set {} MB", cpu, elapsed,
                     100 * cpu / elapsed, memory.PeakWorkingSetSize / (1024 * 1024)));
  if (const auto dirs = GetAppDirs()) Report(Outcome::kNote, "logs", ToUtf8(dirs->Logs()));

  if (observer.first_state() == core::State::kObsMissing && !options.obs_dir &&
      runtime::ObsInstallCandidates().empty()) {
    Print("SKIP (OBS isn't installed)\n");
    return kExitSkip;
  }
  if (observer.failed()) Check(false, "core", "the core failed (see above)");
  Print(AnyFailed() ? "FAIL\n" : "PASS\n");
  return AnyFailed() ? kExitFail : kExitPass;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // Plain LoadLibrary calls skip PATH and the working directory; see
  // ObsRuntime::Load.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
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
