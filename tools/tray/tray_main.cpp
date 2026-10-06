// SPDX-License-Identifier: GPL-2.0-or-later
//
// knobs-tray: runs the tray app (src/tray) against a fake core: the real
// core and its decisions, on a made-up OBS install, scene collection and
// audio devices (fake_backend.h). Every state of the menu, every page of the
// first run and every notification can be checked without OBS or an audio
// device, and --screenshot saves a picture of any of them. --obs-config
// imports a real OBS settings folder instead, such as
// tests/fixtures/obs-config, with the installed OBS's libobs.

#include <windows.h>
#include <conio.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "app_info.h"
#include "common/console.h"
#include "common/obs_import.h"
#include "core/core.h"
#include "tray/badge.h"
#include "tray/fake_backend.h"
#include "tray/first_run.h"
#include "tray/notices.h"
#include "tray/resource.h"
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
// What the '.' key waits.
constexpr UINT kPauseMs = 1000;
// A notification slides in, and stays for 5 s by default.
constexpr auto kNotificationShows = 1500ms;
// How long a notification screenshot waits for one.
constexpr UINT kNotificationWaitMs = 20'000;
// The icon sheet (--icons): each size of the tray icon, magnified, and as it
// is, on a light and a dark taskbar's color.
constexpr int kIconSizes[] = {16, 20, 24, 32};
constexpr int kIconZoom = 6;
constexpr uint32_t kLightTaskbar = 0xffeeeeee;
constexpr uint32_t kDarkTaskbar = 0xff1c1c1c;

std::string Usage() {
  std::string states;
  for (const FakeScenario& scenario : FakeScenarios()) {
    states += std::format("  {:18} {}\n", scenario.name, scenario.description);
  }
  std::string pages;
  for (int page = 0; page <= static_cast<int>(tray::FirstRunPage::kDone); ++page) {
    pages += std::format("{}{}", pages.empty() ? "" : ", ", tray::PageName(static_cast<tray::FirstRunPage>(page)));
  }
  return std::format(R"(Usage: knobs-tray [options]

Runs the tray app against a fake core: the real core on a made-up OBS install,
scene collection and audio devices, so every state of the menu and every page of
the first run can be checked without OBS or an audio device. Nothing of OBS's is
read, no audio device is opened, and nothing is saved unless --settings is
given. "Start with Windows" is kept in memory. Menu commands work as in {0},
against the fake.

Options:
  --state <name>         The state to start in (below). Default: running.
  --theme <name>         system (Windows' mode), light or dark, for the menu and
                         the badges' colors. Default: system.
  --settings <file>      Read the settings from this file and save changes to it.
                         Without it, the first run counts as finished unless
                         --first-run is given.
  --start-with-windows   Start with "Start with Windows" checked.
  --restarted-for <why>  Start as the copy {0} starts to follow OBS, which says
                         why in a notification: obs-updated or audio-changed.
  --first-run <page>     Open the first run: "resume" opens it as {0} does, a
                         page's name opens that page. Pages: {1}.
  --press <keys>         Press these keys (below) one at a time once it has
                         settled, waiting for each to settle, then quit. With
                         --first-run, in the first run.
  --screenshot <png>     Open the menu, or the first run, save a picture of it,
                         and quit. After --press, a dialog the keys opened.
  --open <name>          With --screenshot, open mic, cable or other (Cable's
                         "Other devices") for the picture. about takes that
                         dialog instead of the menu, and notification the
                         notification the keys bring up, from the screen.
  --icons <png>          Save a sheet of the tray icon with each badge, at
                         16, 20, 24 and 32 px on a light and a dark taskbar,
                         and quit.
{2}
                         With --obs-config, the installed OBS's libobs imports
                         that folder, as {0} does. The audio devices are made
                         up to match tests/fixtures/obs-config's.

Keys while it runs: o opens or closes OBS, c plugs the virtual cables in or out,
m the mics, s stalls the chain or starts it again, f adds filters to OBS's mics
without any, e adds Noise Suppression to the first mic's chain or takes it out,
v adds a VST filter to it or takes it out, u updates OBS (to 32.2.3, then to
33.0.0), i re-imports from OBS, a clicks the notification, . waits a second,
and q quits. Changes in OBS are seen when it closes or at a re-import. In the
first run: y and n click the doors ("I set up my mic in OBS", "I'm new to OBS"),
x Next, b Back, d Done, p Open OBS (which only says so: OBS would open the
mic), 1 to 9 pick a choice, and k unchecks the check box.

States:
{3})",
                     kDisplayName, pages, kImportUsage, states);
}

struct Options {
  const FakeScenario* scenario = &FakeScenarios().front();
  tray::MenuTheme theme = tray::MenuTheme::kSystem;
  std::optional<fs::path> settings;
  bool start_with_windows = false;
  ImportArgs import_args;
  // The first run, and the page to open it at; nullopt resumes.
  bool first_run = false;
  std::optional<tray::FirstRunPage> first_run_page;
  std::wstring presses;
  std::optional<fs::path> screenshot;
  std::wstring keys;    // Menu mnemonics to press before the picture.
  unsigned dialog = 0;  // Or the menu command whose dialog to take instead.
  bool notification = false;  // Or the notification the keys bring up.
  core::RestartNeed restarted_for = core::RestartNeed::kNone;
  std::optional<fs::path> icons;
};

std::optional<Options> ParseArgs(int argc, wchar_t** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view arg = argv[i];
    if (arg == L"--start-with-windows") {
      options.start_with_windows = true;
      continue;
    }
    const wchar_t* next = i + 1 < argc ? argv[i + 1] : nullptr;
    if (bool bad = false; ParseImportArg(arg, next, options.import_args, bad)) {
      if (bad) return std::nullopt;
      ++i;
      continue;
    }
    if (!next) return std::nullopt;
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
    } else if (arg == L"--first-run") {
      options.first_run = true;
      if (value != L"resume") {
        options.first_run_page = tray::PageNamed(ToUtf8(value));
        if (!options.first_run_page) return std::nullopt;
      }
    } else if (arg == L"--press") {
      options.presses = value;
    } else if (arg == L"--restarted-for") {
      const auto why = tray::RestartNeedNamed(ToUtf8(value));
      if (!why) return std::nullopt;
      options.restarted_for = *why;
    } else if (arg == L"--icons") {
      options.icons = fs::absolute(value);
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
      } else if (value == L"about") {
        options.dialog = tray::kIdAbout;
      } else if (value == L"notification") {
        options.notification = true;
      } else {
        return std::nullopt;
      }
    } else {
      return std::nullopt;
    }
  }
  if ((!options.keys.empty() || options.dialog || options.notification) && (!options.screenshot || options.first_run)) {
    return std::nullopt;
  }
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

std::string DescribeFirstRun(const tray::FirstRunProgress& progress) {
  if (progress.done) return "done";
  switch (progress.door) {
    case tray::Door::kNone:
      return "at the welcome page";
    case tray::Door::kNewToObs:
      return "on the steps for people new to OBS";
    case tray::Door::kObsUser:
      break;
  }
  return std::format("answered up to the {} page", tray::PageName(progress.reached));
}

// What the timers need. One run per process.
struct Session {
  Options options;
  FakeScript script;
  std::shared_ptr<SharedWorld> world;
  tray::TrayApp* app = nullptr;
  core::Core* core = nullptr;
  std::atomic<bool> obs_running = false;
  bool start_with_windows = false;
  Clock::time_point start = Clock::now();
  UINT_PTR settle_timer = 0;
  bool first_run_opened = false;
  size_t presses_done = 0;
  bool screenshot_taken = false;
  bool ended = false;
  // The notification last shown, and when.
  std::optional<Clock::time_point> notice_shown;
  bool waiting_for_notice = false;
  UINT_PTR notice_timer = 0;
  // The screenshot.
  HWND backdrop = nullptr;
  size_t keys_sent = 0;
  bool failed = false;
};

Session* g_session = nullptr;

void TakeScreenshot();
void TakeDialogScreenshot();
void OpenFirstRun();
void TakeNotificationScreenshot();

void WaitToSettle(UINT ms = kSettleMs);

// The keys, from the console or --press.
void Press(wchar_t key) {
  Session& session = *g_session;
  switch (std::towlower(static_cast<wint_t>(key))) {
    case L'o': {
      const bool running = !session.obs_running;
      session.obs_running = running;
      Print(running ? "OBS opens\n" : "OBS closes\n");
      break;
    }
    case L'c': {
      bool plugged = false;
      session.world->Change([&plugged](FakeWorld& world) {
        ToggleCables(world);
        plugged = std::any_of(world.devices.outputs.begin(), world.devices.outputs.end(),
                              [](const audio::AudioDevice& device) { return tray::IsVirtualCable(device.name); });
      });
      Print(plugged ? "virtual cables plugged in\n" : "virtual cables unplugged\n");
      if (session.core) session.core->DevicesChanged();
      break;
    }
    case L'f':
      session.world->Change(AddFilters);
      Print("OBS's mics without filters get some (seen at the next import)\n");
      break;
    case L'm': {
      bool plugged = false;
      session.world->Change([&plugged](FakeWorld& world) {
        ToggleMics(world);
        plugged = !world.spare_mics;
      });
      Print(plugged ? "mics plugged in\n" : "mics unplugged\n");
      if (session.core) session.core->DevicesChanged();
      break;
    }
    case L's': {
      bool stalled = false;
      session.world->Change([&stalled](FakeWorld& world) { stalled = world.stalled = !world.stalled; });
      Print(stalled ? "the chain gets no audio\n" : "the chain gets audio again\n");
      break;
    }
    case L'e':
      session.world->Change(EditChain);
      Print("the first mic's chain changes in OBS (seen at the next import)\n");
      break;
    case L'v':
      session.world->Change(ToggleVstFilter);
      Print("the first mic gets a VST filter in OBS, or loses it (seen at the next import)\n");
      break;
    case L'u': {
      std::string version;
      session.world->Change([&version](FakeWorld& world) { version = UpdateObs(world); });
      Print(std::format("OBS is updated to {} (seen at the next import)\n", version));
      break;
    }
    case L'i':
      Print("menu: Re-import from OBS\n");
      session.app->Execute(tray::kIdReimport);
      break;
    case L'a':
      Print("the notification is clicked\n");
      session.app->OpenNotice();
      break;
    case L'.':
      break;
    case L'q':
      session.app->Quit();
      break;
    default:
      // The first run's buttons, clicked through the task dialog's own
      // messages.
      if (const HWND dialog = OpenDialog()) {
        const auto click = [dialog](int id) { SendMessageW(dialog, TDM_CLICK_BUTTON, id, 0); };
        if (key == L'y') {
          click(tray::kButtonObsUser);
        } else if (key == L'n') {
          click(tray::kButtonNewToObs);
        } else if (key == L'x') {
          click(tray::kButtonNext);
        } else if (key == L'b') {
          click(tray::kButtonBack);
        } else if (key == L'd') {
          click(tray::kButtonDone);
        } else if (key == L'p') {
          click(tray::kButtonOpenObs);
        } else if (key == L'k') {
          SendMessageW(dialog, TDM_CLICK_VERIFICATION, FALSE, FALSE);
        } else if (key >= L'1' && key <= L'9') {
          SendMessageW(dialog, TDM_CLICK_RADIO_BUTTON, tray::kChoiceFirst + (key - L'1'), 0);
        }
      }
      break;
  }
}

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
  if (session.options.first_run && !session.first_run_opened) {
    session.first_run_opened = true;
    OpenFirstRun();
    return;
  }
  if (session.presses_done < session.options.presses.size()) {
    // In the first run's loop, if it's open.
    const wchar_t key = session.options.presses[session.presses_done++];
    Print(std::format("press {}\n", ToUtf8(std::wstring(1, key))));
    // A key may change nothing the core reports. The wait starts before the
    // key too, which may open a dialog and run its loop until it closes.
    const UINT wait = key == L'.' ? kPauseMs : kSettleMs;
    WaitToSettle(wait);
    Press(key);
    WaitToSettle(wait);
    return;
  }
  const HWND dialog = OpenDialog();
  if (session.options.notification) {
    if (!session.waiting_for_notice) {
      session.waiting_for_notice = true;
      TakeNotificationScreenshot();
    }
    return;
  }
  if (session.options.screenshot && !session.screenshot_taken) {
    if (dialog || session.first_run_opened) {
      session.screenshot_taken = true;
      const Status saved = dialog ? SaveWindowPng(dialog, *session.options.screenshot)
                                  : Status(Error{"The first run's window isn't open."});
      Check(saved.ok(), "screenshot", saved ? ToUtf8(*session.options.screenshot) : saved.error());
      session.failed |= !saved;
    } else {
      session.screenshot_taken = true;
      if (session.options.dialog) {
        TakeDialogScreenshot();
      } else {
        TakeScreenshot();
      }
      return;
    }
  }
  // A scripted run ends with the keys: close what they opened, then quit.
  if (!session.options.screenshot && session.options.presses.empty()) return;
  if (dialog) {
    PostMessageW(dialog, WM_CLOSE, 0, 0);
  } else if (!session.first_run_opened && !session.ended) {
    session.ended = true;
    session.app->Quit();
  }
}

void WaitToSettle(UINT ms) {
  if (g_session->settle_timer) KillTimer(nullptr, g_session->settle_timer);
  g_session->settle_timer = SetTimer(nullptr, 0, ms, OnSettled);
}

void CALLBACK OnKeys(HWND, UINT, UINT_PTR, DWORD) {
  while (_kbhit()) Press(static_cast<wchar_t>(_getwch()));
}

// Opens the first run, and returns once it's closed. The keys to press and
// the picture to take happen in its loop, once it has settled.
void OpenFirstRun() {
  Session& session = *g_session;
  WaitToSettle();
  session.app->ShowFirstRun(false, session.options.first_run_page);
  Print(std::format("first run closed: {}\n", DescribeFirstRun(session.app->first_run())));
  if (session.options.screenshot || !session.options.presses.empty()) session.app->Quit();
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

void CALLBACK OnNotificationStep(HWND, UINT, UINT_PTR timer, DWORD) {
  KillTimer(nullptr, timer);
  Session& session = *g_session;
  session.notice_timer = 0;
  if (session.ended) return;
  session.ended = true;
  if (!session.notice_shown) {
    Check(false, "screenshot", "No notification came up.");
    session.failed = true;
  } else {
    const RECT area = NotificationArea();
    const Status saved = SaveScreenPng(area, *session.options.screenshot);
    Check(saved.ok(), "screenshot",
          saved ? std::format("{} ({}x{}, the bottom-right corner of the screen)", ToUtf8(*session.options.screenshot),
                              area.right - area.left, area.bottom - area.top)
                : saved.error());
    session.failed |= !saved;
  }
  session.app->Quit();
}

// Waits for a notification, and takes the corner of the screen it shows in
// once it has slid in. Windows draws it, so Do Not Disturb, or notifications
// turned off for the app, leave nothing to take.
void TakeNotificationScreenshot() {
  Session& session = *g_session;
  if (session.notice_timer) KillTimer(nullptr, session.notice_timer);
  UINT wait = kNotificationWaitMs;
  if (session.notice_shown) {
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(*session.notice_shown + kNotificationShows -
                                                                    Clock::now());
    wait = static_cast<UINT>(std::max<long long>(left.count(), USER_TIMER_MINIMUM));
  }
  session.notice_timer = SetTimer(nullptr, 0, wait, OnNotificationStep);
}

// Each size of the tray icon with each badge, magnified and as it is, on a
// light and a dark taskbar.
int SaveIconSheet(const fs::path& file) {
  const int cell = kIconSizes[std::size(kIconSizes) - 1] * (kIconZoom + 1) + 3 * kMarginPx;
  const int columns = static_cast<int>(std::size(kIconSizes));
  constexpr tray::Badge kBadges[] = {tray::Badge::kNone, tray::Badge::kPaused, tray::Badge::kAttention};
  const int width = columns * cell;
  const int height = 2 * static_cast<int>(std::size(kBadges)) * cell;
  std::vector<uint32_t> sheet(static_cast<size_t>(width) * height);
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  for (int dark = 0; dark < 2; ++dark) {
    const uint32_t background = dark ? kDarkTaskbar : kLightTaskbar;
    for (size_t row = 0; row < std::size(kBadges); ++row) {
      const int top = (dark * static_cast<int>(std::size(kBadges)) + static_cast<int>(row)) * cell;
      for (int y = top; y < top + cell; ++y) {
        std::fill_n(sheet.begin() + static_cast<ptrdiff_t>(y) * width, width, background);
      }
      for (int column = 0; column < columns; ++column) {
        const int size = kIconSizes[column];
        HICON icon = nullptr;
        if (FAILED(LoadIconWithScaleDown(instance, MAKEINTRESOURCEW(IDI_KNOBS), size, size, &icon))) continue;
        auto pixels = tray::ReadIcon(icon);
        DestroyIcon(icon);
        if (!pixels) continue;
        const tray::IconPixels badged = tray::AddBadge(std::move(*pixels), kBadges[row], dark != 0);
        // Over the background, magnified, then as it is beside it.
        const auto put = [&](int x, int y, uint32_t pixel) {
          const float alpha = (pixel >> 24) / 255.0f;
          uint32_t out = 0xff000000;
          for (int shift = 0; shift < 24; shift += 8) {
            const float over = ((pixel >> shift) & 0xff) * alpha + ((background >> shift) & 0xff) * (1 - alpha);
            out |= static_cast<uint32_t>(std::lround(over)) << shift;
          }
          sheet[static_cast<size_t>(y) * width + x] = out;
        };
        const int left = column * cell + kMarginPx;
        for (int y = 0; y < size * kIconZoom; ++y) {
          for (int x = 0; x < size * kIconZoom; ++x) {
            put(left + x, top + kMarginPx + y,
                badged.bgra[static_cast<size_t>(y / kIconZoom) * size + x / kIconZoom]);
          }
        }
        for (int y = 0; y < size; ++y) {
          for (int x = 0; x < size; ++x) {
            put(left + size * kIconZoom + kMarginPx + x, top + kMarginPx + y,
                badged.bgra[static_cast<size_t>(y) * size + x]);
          }
        }
      }
    }
  }
  const Status saved = SavePixelsPng(width, height, sheet, file);
  Check(saved.ok(), "icons",
        saved ? std::format("{} (rows: no badge, paused, attention; light taskbar, then dark)", ToUtf8(file))
              : saved.error());
  Print(saved ? "PASS\n" : "FAIL\n");
  return saved ? kExitPass : kExitFail;
}

const char* BadgeName(tray::Badge badge) {
  switch (badge) {
    case tray::Badge::kNone:
      return "no badge";
    case tray::Badge::kPaused:
      return "paused badge";
    case tray::Badge::kAttention:
      return "attention badge";
  }
  return "";
}

int Run(const Options& options) {
  Session session;
  g_session = &session;
  session.options = options;
  session.start_with_windows = options.start_with_windows;
  const bool fixture = options.import_args.config_dir.has_value();
  FakeWorld world = fixture ? FixtureWorld() : DefaultWorld();
  options.scenario->setup(world, session.script);
  // The copy restarted for an update finds the new version.
  if (options.restarted_for == core::RestartNeed::kObsUpdated) UpdateObs(world);
  session.world = std::make_shared<SharedWorld>(std::move(world));
  session.obs_running = session.script.obs_running;
  Print(std::format("{} tray, {}: {} ({})\n", kDisplayName,
                    fixture ? std::format("OBS settings in {}", ToUtf8(*options.import_args.config_dir))
                            : std::string("fake core"),
                    options.scenario->name, options.scenario->description));

  tray::TrayOptions tray_options;
  tray_options.instance = GetModuleHandleW(nullptr);
  tray_options.window_class = std::format(L"{}.tray-fake", kDisplayNameW);
  if (options.settings) tray_options.settings_file = *options.settings;
  // Notifications wait for the first run to be finished.
  tray_options.first_run.done = !options.first_run;
  tray_options.restarted_for = options.restarted_for;
  tray_options.settings.obs_config = options.import_args.config_dir;
  tray_options.settings.mic = options.import_args.pick;
  if (const auto dirs = GetAppDirs()) tray_options.log_folder = dirs->Logs();
  tray_options.theme = options.theme;
  tray_options.starts_with_windows = [] { return g_session->start_with_windows; };
  tray_options.set_start_with_windows = [](bool on) {
    g_session->start_with_windows = on;
    Print(std::format("Start with Windows {}\n", on ? "on" : "off"));
    return Status(Ok{});
  };
  tray_options.restart = [](core::RestartNeed why) {
    Print(std::format("{0} would restart now{1}{2}\n", kDisplayName,
                      why == core::RestartNeed::kNone ? "" : ", and say why: knobs-tray --restarted-for ",
                      tray::RestartNeedName(why)));
    return Status(Ok{});
  };
  // The real OBS would open the mic.
  tray_options.open_obs = [](const fs::path& install_root) {
    Print(std::format("OBS would open from {}\n", ToUtf8(install_root)));
    return Status(Ok{});
  };
  tray_options.start_core = [fixture](core::Observer& observer,
                                      const core::Settings& settings) -> Result<std::unique_ptr<core::Core>> {
    Print(std::format("settings: {}\n", DescribeSettings(settings)));
    if (g_session->script.no_core) return std::unique_ptr<core::Core>();
    core::CoreOptions core_options;
    core_options.settings = settings;
    core_options.obs_running = [] { return g_session->obs_running.load(); };
    core_options.obs_poll = 250ms;
    core_options.watch_devices = false;
    const auto log = [](std::string_view line) { Print(std::format("{:13}log: {}\n", "", line)); };
    std::unique_ptr<core::Backend> backend;
    if (fixture) {
      backend = std::make_unique<FixtureBackend>(core::ObsBackendOptions{.log_prefix = L"tray "}, g_session->world,
                                                 log);
    } else {
      backend = std::make_unique<FakeBackend>(g_session->world, log);
    }
    auto started = core::Core::Start(std::move(backend), observer, std::move(core_options));
    if (started) g_session->core = started->get();
    return started;
  };
  tray_options.on_snapshot = [](const core::Snapshot& snapshot) {
    const double seconds = std::chrono::duration<double>(Clock::now() - g_session->start).count();
    Print(std::format("[{:8.2f} s] {}\n{:13}status: {}\n", seconds, core::DescribeSnapshot(snapshot), "",
                      tray::StatusLine(snapshot)));
    WaitToSettle();
  };
  tray_options.on_notice = [](const std::optional<tray::Notice>& notice) {
    const double seconds = std::chrono::duration<double>(Clock::now() - g_session->start).count();
    if (!notice) {
      Print(std::format("[{:8.2f} s] notification taken down\n", seconds));
      return;
    }
    Print(std::format("[{:8.2f} s] notification: {}\n{:13}{}\n{:13}a click opens {}{}\n", seconds, notice->title,
                      "", notice->text, "",
                      notice->page ? std::format("the first run's {} page", tray::PageName(*notice->page))
                                   : std::string("the menu"),
                      notice->sound ? "" : " (no sound)"));
    g_session->notice_shown = Clock::now();
    if (g_session->waiting_for_notice) TakeNotificationScreenshot();
  };
  tray_options.on_badge = [](tray::Badge badge) {
    const double seconds = std::chrono::duration<double>(Clock::now() - g_session->start).count();
    Print(std::format("[{:8.2f} s] icon: {}\n", seconds, BadgeName(badge)));
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
  const int code = options->icons ? SaveIconSheet(*options->icons) : Run(*options);
  CoUninitialize();
  return code;
}
