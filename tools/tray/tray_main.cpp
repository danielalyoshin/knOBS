// SPDX-License-Identifier: GPL-2.0-or-later
//
// knobs-tray: runs the tray app (src/tray) against a fake core: the real
// core and its decisions, on a made-up OBS install, scene collection and
// audio devices (fake_backend.h). Every state of the menu can be checked
// without OBS or an audio device, and --screenshot saves a picture of the
// menu.

#include <windows.h>
#include <conio.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "app_info.h"
#include "common/console.h"
#include "core/core.h"
#include "tray/fake_backend.h"
#include "tray/screenshot.h"
#include "tray/tray_app.h"
#include "util/app_dirs.h"
#include "util/win_strings.h"

namespace {

using namespace knobs;
using namespace knobs::tools;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

// The core has settled once it has said nothing for this long.
constexpr UINT kSettleMs = 600;
// Between steps of a screenshot: long enough for a menu to fade in.
constexpr UINT kStepMs = 400;
// Around the menus in a screenshot, for their shadows.
constexpr int kMarginPx = 24;

std::string Usage() {
  std::string states;
  for (const FakeScenario& scenario : FakeScenarios()) {
    states += std::format("  {:18} {}\n", scenario.name, scenario.description);
  }
  return std::format(R"(Usage: knobs-tray [options]

Runs the tray app against a fake core: the real core on a made-up OBS install,
scene collection and audio devices, so every state of the menu can be checked
without OBS or an audio device. Nothing of OBS's is read, no audio device is
opened, and nothing is saved unless --settings is given. "Start with Windows"
is kept in memory. Menu commands work as in {0}, against the fake.

Options:
  --state <name>         The state to start in (below). Default: running.
  --theme <name>         system (Windows' mode), light or dark. Default: system.
  --settings <file>      Read the settings from this file and save changes to it.
  --start-with-windows   Start with "Start with Windows" checked.
  --screenshot <png>     Open the menu, save a picture of it, and quit.
  --open <name>          With --screenshot, open mic, cable, other (Cable's
                         "Other devices"), or choose (the choices "Choose a
                         mic…" or "Choose a cable…" opens) for the picture.
                         about or setup takes that dialog instead of the menu.

Keys while it runs: o opens or closes OBS, q quits.

States:
{1})",
                     kDisplayName, states);
}

struct Options {
  const FakeScenario* scenario = &FakeScenarios().front();
  tray::MenuTheme theme = tray::MenuTheme::kSystem;
  std::optional<fs::path> settings;
  bool start_with_windows = false;
  std::optional<fs::path> screenshot;
  std::wstring keys;    // Menu mnemonics to press before the picture.
  unsigned dialog = 0;  // Or the menu command whose dialog to take instead.
};

std::optional<Options> ParseArgs(int argc, wchar_t** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view arg = argv[i];
    if (arg == L"--start-with-windows") {
      options.start_with_windows = true;
      continue;
    }
    if (i + 1 >= argc) return std::nullopt;
    const std::wstring_view value = argv[++i];
    if (arg == L"--state") {
      options.scenario = nullptr;
      for (const FakeScenario& scenario : FakeScenarios()) {
        if (FromUtf8(scenario.name) == value) options.scenario = &scenario;
      }
      if (!options.scenario) return std::nullopt;
    } else if (arg == L"--theme") {
      if (value == L"system") {
        options.theme = tray::MenuTheme::kSystem;
      } else if (value == L"light") {
        options.theme = tray::MenuTheme::kLight;
      } else if (value == L"dark") {
        options.theme = tray::MenuTheme::kDark;
      } else {
        return std::nullopt;
      }
    } else if (arg == L"--settings") {
      options.settings = fs::absolute(value);
    } else if (arg == L"--screenshot") {
      options.screenshot = fs::absolute(value);
    } else if (arg == L"--open") {
      // The mnemonics: &Mic, &Cable, then &Other devices.
      if (value == L"mic") {
        options.keys = L"m";
      } else if (value == L"cable") {
        options.keys = L"c";
      } else if (value == L"other") {
        options.keys = L"co";
      } else if (value == L"choose") {
        options.keys = L"h";  // C&hoose a mic… or C&hoose a cable…
      } else if (value == L"about") {
        options.dialog = tray::kIdAbout;
      } else if (value == L"setup") {
        options.dialog = tray::kIdSetup;
      } else {
        return std::nullopt;
      }
    } else {
      return std::nullopt;
    }
  }
  if ((!options.keys.empty() || options.dialog) && !options.screenshot) return std::nullopt;
  return options;
}

std::string DescribeSettings(const core::Settings& settings) {
  return std::format("OBS install {}, OBS settings {}, mic {}, cable {}, pause while OBS is open {}",
                     settings.obs_dir ? ToUtf8(*settings.obs_dir) : "(found)",
                     settings.obs_config ? ToUtf8(*settings.obs_config) : "(found)",
                     settings.mic.empty() ? "(same as OBS)" : "\"" + settings.mic + "\"",
                     settings.cable.empty() ? "(same as OBS)" : "\"" + settings.cable_name + "\"",
                     settings.pause_for_obs ? "on" : "off");
}

// What the timers need. One run per process.
struct Session {
  Options options;
  FakeScript script;
  tray::TrayApp* app = nullptr;
  std::atomic<bool> obs_running = false;
  bool start_with_windows = false;
  Clock::time_point start = Clock::now();
  UINT_PTR settle_timer = 0;
  // The screenshot.
  HWND backdrop = nullptr;
  size_t keys_sent = 0;
  bool failed = false;
};

Session* g_session = nullptr;

void TakeScreenshot();
void TakeDialogScreenshot();

void CALLBACK OnSettled(HWND, UINT, UINT_PTR timer, DWORD) {
  KillTimer(nullptr, timer);
  g_session->settle_timer = 0;
  Session& session = *g_session;
  // Steps that take the core to the state, as the user would.
  if (session.script.pause) {
    session.script.pause = false;
    Print("menu: Pause\n");
    session.app->Execute(tray::kIdPause);
    return;  // The snapshot restarts the wait.
  }
  if (session.script.reimport) {
    session.script.reimport = false;
    Print("menu: Re-import from OBS\n");
    session.app->Execute(tray::kIdReimport);
    return;
  }
  if (session.options.dialog) {
    TakeDialogScreenshot();
  } else if (session.options.screenshot) {
    TakeScreenshot();
  }
}

void WaitToSettle() {
  if (g_session->settle_timer) KillTimer(nullptr, g_session->settle_timer);
  g_session->settle_timer = SetTimer(nullptr, 0, kSettleMs, OnSettled);
}

void CALLBACK OnKeys(HWND, UINT, UINT_PTR, DWORD) {
  while (_kbhit()) {
    switch (std::towlower(static_cast<wint_t>(_getwch()))) {
      case L'o': {
        const bool running = !g_session->obs_running;
        g_session->obs_running = running;
        Print(running ? "OBS opens\n" : "OBS closes\n");
        break;
      }
      case L'q':
        g_session->app->Quit();
        return;
      default:
        break;
    }
  }
}

// One step of the screenshot each tick, while the menu is open: press the
// next key, or take the picture and close the menu.
void CALLBACK OnScreenshotStep(HWND window, UINT, UINT_PTR timer, DWORD) {
  Session& session = *g_session;
  const std::vector<HWND> menus = OpenMenus();
  if (menus.empty()) return;
  if (session.keys_sent < session.options.keys.size()) {
    PostMessageW(menus.back(), WM_CHAR, session.options.keys[session.keys_sent++], 0);
    return;
  }
  KillTimer(window, timer);
  RECT rect = *BoundsOf(menus);
  InflateRect(&rect, kMarginPx, kMarginPx);
  RECT backdrop = {};
  GetWindowRect(session.backdrop, &backdrop);
  IntersectRect(&rect, &rect, &backdrop);
  const Status saved = SaveScreenPng(rect, *session.options.screenshot);
  Check(saved.ok(), "screenshot",
        saved ? std::format("{} ({}x{})", ToUtf8(*session.options.screenshot), rect.right - rect.left,
                            rect.bottom - rect.top)
              : saved.error());
  session.failed |= !saved;
  EndMenu();
}

// Opens the menu over a plain backdrop, so the picture shows nothing else
// that's on the screen, away from the mouse pointer, which would highlight
// an item.
void TakeScreenshot() {
  Session& session = *g_session;
  const bool dark = session.options.theme == tray::MenuTheme::kDark ||
                    (session.options.theme == tray::MenuTheme::kSystem && tray::WindowsModeIsDark());
  const HBRUSH brush = CreateSolidBrush(dark ? RGB(0x20, 0x20, 0x20) : RGB(0xf3, 0xf3, 0xf3));
  WNDCLASSEXW window_class = {sizeof(window_class)};
  window_class.lpfnWndProc = DefWindowProcW;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.hbrBackground = brush;
  window_class.lpszClassName = L"knobs-tray backdrop";
  RegisterClassExW(&window_class);

  POINT cursor = {};
  GetCursorPos(&cursor);
  MONITORINFO monitor = {sizeof(monitor)};
  GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
  const RECT work = monitor.rcWork;
  const int width = std::min<int>(1200, (work.right - work.left) / 2 - 40);
  const int height = std::min<int>(1000, work.bottom - work.top - 80);
  const bool cursor_left = cursor.x < (work.left + work.right) / 2;
  const int left = cursor_left ? work.right - width - 40 : work.left + 40;
  session.backdrop = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, window_class.lpszClassName,
                                     L"", WS_POPUP, left, work.top + 40, width, height, nullptr, nullptr,
                                     window_class.hInstance, nullptr);
  ShowWindow(session.backdrop, SW_SHOWNOACTIVATE);
  UpdateWindow(session.backdrop);

  SetTimer(session.backdrop, 1, kStepMs, OnScreenshotStep);
  // Menus open to the left of the point when Windows is set for right-handed
  // pen use (SM_MENUDROPALIGNMENT).
  const int x = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? left + width - 48 : left + 48;
  session.app->ShowMenu({x, work.top + 88});
  DestroyWindow(session.backdrop);
  DeleteObject(brush);
  session.app->Quit();
}

void CALLBACK OnDialogStep(HWND, UINT, UINT_PTR timer, DWORD) {
  const HWND dialog = OpenDialog();
  if (!dialog) return;
  KillTimer(nullptr, timer);
  const Status saved = SaveWindowPng(dialog, *g_session->options.screenshot);
  Check(saved.ok(), "screenshot", saved ? ToUtf8(*g_session->options.screenshot) : saved.error());
  g_session->failed |= !saved;
  PostMessageW(dialog, WM_CLOSE, 0, 0);
}

// Task dialogs stay light in either theme (plan.md, Tray and first run).
void TakeDialogScreenshot() {
  SetTimer(nullptr, 0, kStepMs, OnDialogStep);
  g_session->app->Execute(g_session->options.dialog);
  g_session->app->Quit();
}

int Run(const Options& options) {
  Session session;
  g_session = &session;
  session.options = options;
  session.start_with_windows = options.start_with_windows;
  FakeWorld world = DefaultWorld();
  options.scenario->setup(world, session.script);
  session.obs_running = session.script.obs_running;
  Print(std::format("{} tray, fake core: {} ({})\n", kDisplayName, options.scenario->name,
                    options.scenario->description));

  tray::TrayOptions tray_options;
  tray_options.instance = GetModuleHandleW(nullptr);
  tray_options.window_class = std::format(L"{}.tray-fake", kDisplayNameW);
  if (options.settings) tray_options.settings_file = *options.settings;
  if (const auto dirs = GetAppDirs()) tray_options.log_folder = dirs->Logs();
  tray_options.theme = options.theme;
  tray_options.starts_with_windows = [] { return g_session->start_with_windows; };
  tray_options.set_start_with_windows = [](bool on) {
    g_session->start_with_windows = on;
    Print(std::format("Start with Windows {}\n", on ? "on" : "off"));
    return Status(Ok{});
  };
  tray_options.restart = [] {
    Print(std::format("{} would restart now\n", kDisplayName));
    return Status(Ok{});
  };
  tray_options.start_core = [world](core::Observer& observer,
                                    const core::Settings& settings) -> Result<std::unique_ptr<core::Core>> {
    Print(std::format("settings: {}\n", DescribeSettings(settings)));
    if (g_session->script.no_core) return std::unique_ptr<core::Core>();
    core::CoreOptions core_options;
    core_options.settings = settings;
    core_options.obs_running = [] { return g_session->obs_running.load(); };
    core_options.obs_poll = 250ms;
    core_options.watch_devices = false;
    auto backend = std::make_unique<FakeBackend>(world, [](std::string_view line) {
      Print(std::format("{:13}log: {}\n", "", line));
    });
    return core::Core::Start(std::move(backend), observer, std::move(core_options));
  };
  tray_options.on_snapshot = [](const core::Snapshot& snapshot) {
    const double seconds = std::chrono::duration<double>(Clock::now() - g_session->start).count();
    Print(std::format("[{:8.2f} s] {}\n{:13}status: {}\n", seconds, core::DescribeSnapshot(snapshot), "",
                      tray::StatusLine(snapshot)));
    WaitToSettle();
  };

  auto app = tray::TrayApp::Create(std::move(tray_options));
  if (!app) {
    Check(false, "tray", app.error());
    Print("FAIL\n");
    return kExitFail;
  }
  session.app = app->get();
  WaitToSettle();
  DWORD mode = 0;
  if (!options.screenshot && GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode)) SetTimer(nullptr, 0, 100, OnKeys);
  (*app)->Run();
  app->reset();
  g_session = nullptr;
  Print(session.failed ? "FAIL\n" : "PASS\n");
  return session.failed ? kExitFail : kExitPass;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
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
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  if (FAILED(com)) {
    Print(std::format("Couldn't initialize COM (0x{:08X}).\n", static_cast<uint32_t>(com)));
    return kExitFail;
  }
  const int code = Run(*options);
  CoUninitialize();
  return code;
}
