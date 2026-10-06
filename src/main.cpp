// SPDX-License-Identifier: GPL-2.0-or-later
//
// knobs.exe: the tray app. One copy runs per Windows session. It shows its
// state in the tray and runs the always-on core (src/core) on the user's OBS.
// Until the first run is finished, it opens that too.
//
//   --startup   Started by Windows at sign-in (the Run entry). If a copy is
//               already running, quit without a word. Doesn't open the first
//               run.
//   --restart   Started by a copy that's quitting to restart: wait for it.
//   --restarted-for <reason>
//               Why: obs-updated or audio-changed. The new copy says so in a
//               notification.

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>

#include "app_info.h"
#include "core/core.h"
#include "core/obs_backend.h"
#include "tray/autostart.h"
#include "tray/notices.h"
#include "tray/single_instance.h"
#include "tray/tray_app.h"
#include "util/app_dirs.h"
#include "util/win_strings.h"

namespace {

using namespace knobs;
using namespace std::chrono_literals;

// How long a restarted copy waits for the old one to shut libobs down.
constexpr std::chrono::milliseconds kRestartWait = 30s;

std::filesystem::path ExePath() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) return {};
    if (length < path.size()) {
      path.resize(length);
      return path;
    }
    path.resize(path.size() * 2);
  }
}

Status StartCopy(const std::filesystem::path& exe, std::wstring_view arguments) {
  std::wstring command = std::format(L"\"{}\" {}", exe.native(), arguments);
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process = {};
  // In the exe's folder: libobs has moved this process's working directory
  // into its runtime copy.
  const std::wstring folder = exe.parent_path().native();
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startup,
                      &process)) {
    return Error{std::format("Couldn't start {} again: {}", kDisplayName, DescribeWinError(GetLastError()))};
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return Ok{};
}

int Run(HINSTANCE instance, bool startup, bool restart, core::RestartNeed restarted_for) {
  const std::wstring window_class = std::format(L"{}.tray", kDisplayNameW);
  auto lock = tray::SingleInstance::Acquire(window_class, restart ? kRestartWait : 0ms);
  if (!lock) {
    tray::ShowStartupError(lock.error());
    return 1;
  }
  if (!*lock) {
    // Show where the running copy is, unless Windows started this one.
    if (!startup) tray::ActivateTray(window_class);
    return 0;
  }
  const auto dirs = GetAppDirs();
  if (!dirs) {
    tray::ShowStartupError(dirs.error());
    return 1;
  }
  const std::filesystem::path exe = ExePath();
  const tray::RunEntry run{std::wstring(kDisplayNameW), std::format(L"\"{}\" --startup", exe.native())};

  tray::TrayOptions options;
  options.instance = instance;
  options.window_class = window_class;
  options.settings_file = dirs->roaming / L"settings.ini";
  // An unfinished first run comes back, but not at sign-in.
  options.open_first_run = !startup;
  options.log_folder = dirs->Logs();
  options.starts_with_windows = [run] { return tray::StartsWithWindows(run); };
  options.set_start_with_windows = [run](bool on) { return tray::SetStartWithWindows(run, on); };
  options.restart = [exe](core::RestartNeed why) {
    if (why == core::RestartNeed::kNone) return StartCopy(exe, L"--restart");
    return StartCopy(exe, std::format(L"--restart --restarted-for {}", FromUtf8(tray::RestartNeedName(why))));
  };
  options.restarted_for = restarted_for;
  options.start_core = [](core::Observer& observer, const core::Settings& settings) {
    core::ObsBackendOptions backend;
    backend.log_prefix = std::format(L"{} ", kDisplayNameW);
    backend.prune_runtime = true;
    core::CoreOptions core_options;
    core_options.settings = settings;
    return core::Core::Start(std::make_unique<core::ObsBackend>(std::move(backend)), observer,
                             std::move(core_options));
  };
  auto app = tray::TrayApp::Create(std::move(options));
  if (!app) {
    tray::ShowStartupError(app.error());
    return 1;
  }
  return (*app)->Run();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  // Plain LoadLibrary calls skip PATH and the working directory; see
  // ObsRuntime::Load.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  bool startup = false;
  bool restart = false;
  core::RestartNeed restarted_for = core::RestartNeed::kNone;
  int argc = 0;
  if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
    for (int i = 1; i < argc; ++i) {
      const std::wstring_view arg = argv[i];
      startup |= arg == L"--startup";
      restart |= arg == L"--restart";
      if (arg == L"--restarted-for" && i + 1 < argc) {
        restarted_for = tray::RestartNeedNamed(ToUtf8(argv[++i])).value_or(core::RestartNeed::kNone);
      }
    }
    LocalFree(argv);
  }
  // The tray's thread is a single-threaded apartment: the shell's dialogs
  // and ShellExecute need one.
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  if (FAILED(com)) {
    tray::ShowStartupError(std::format("Couldn't initialize COM (0x{:08X}).", static_cast<uint32_t>(com)));
    return 1;
  }
  const int code = Run(instance, startup, restart, restarted_for);
  CoUninitialize();
  return code;
}
