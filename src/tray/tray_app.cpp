// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/tray_app.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <system_error>
#include <utility>
#include <vector>

#include "app_info.h"
#include "runtime/obs_install.h"
#include "tray/open_obs.h"
#include "tray/resource.h"
#include "tray/settings_file.h"
#include "util/win_strings.h"
#include "version.h"

namespace knobs::tray {
namespace {

constexpr UINT kIconId = 1;
constexpr UINT kTrayMessage = WM_APP + 1;      // From the icon (NOTIFYICON_VERSION_4).
constexpr UINT kSnapshotMessage = WM_APP + 2;  // OnSnapshot left one in pending_.
constexpr UINT kActivateMessage = WM_APP + 3;  // Another copy was started (ActivateTray).
constexpr UINT kFirstRunMessage = WM_APP + 4;  // Open the unfinished first run.
constexpr UINT_PTR kNoticeTimer = 1;           // Notifier::NextDeadline.

using Clock = Notifier::Clock;

constexpr Badge kBadges[] = {Badge::kNone, Badge::kPaused, Badge::kAttention};

// Creates the native menu for `items`.
HMENU CreateNativeMenu(const std::vector<MenuItem>& items) {
  const HMENU menu = CreatePopupMenu();
  UINT position = 0;
  for (const MenuItem& item : items) {
    MENUITEMINFOW info = {sizeof(info)};
    std::wstring text = FromUtf8(item.text);
    if (item.kind == MenuItem::Kind::kSeparator) {
      info.fMask = MIIM_FTYPE;
      info.fType = MFT_SEPARATOR;
    } else {
      info.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_FTYPE;
      info.fType = item.radio ? MFT_RADIOCHECK : MFT_STRING;
      info.fState = (item.enabled ? MFS_ENABLED : MFS_DISABLED) | (item.checked ? MFS_CHECKED : 0u) |
                    (item.bold ? MFS_DEFAULT : 0u);
      info.wID = item.id;
      info.dwTypeData = text.data();
      if (item.kind == MenuItem::Kind::kSubmenu) {
        info.fMask |= MIIM_SUBMENU;
        info.hSubMenu = CreateNativeMenu(item.items);
      }
    }
    InsertMenuItemW(menu, position++, TRUE, &info);
  }
  return menu;
}

HICON LoadAppIcon(HINSTANCE instance, int size) {
  HICON icon = nullptr;
  if (FAILED(LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_KNOBS), size, &icon))) return nullptr;
  return icon;
}

// Copies `text` into a NOTIFYICONDATAW field, shortened with "…" to fit.
template <size_t N>
void CopyField(wchar_t (&field)[N], std::string_view text) {
  std::wstring wide = FromUtf8(text);
  if (wide.size() >= N) {
    wide.resize(N - 2);
    wide += L'…';
  }
  wide.copy(field, wide.size());
  field[wide.size()] = L'\0';
}

// A task dialog with a Close button, owned by nobody so that it gets a
// taskbar button and can't hide behind other windows unseen.
void ShowDialog(HINSTANCE instance, PCWSTR icon, HICON custom_icon, const std::wstring& instruction,
                const std::wstring& content, const std::wstring& footer = {}) {
  const std::wstring title(kDisplayNameW);
  TASKDIALOGCONFIG config = {sizeof(config)};
  config.hInstance = instance;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
  config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
  config.pszWindowTitle = title.c_str();
  if (custom_icon) {
    config.dwFlags |= TDF_USE_HICON_MAIN;
    config.hMainIcon = custom_icon;
  } else {
    config.pszMainIcon = icon;
  }
  if (!instruction.empty()) config.pszMainInstruction = instruction.c_str();
  if (!content.empty()) config.pszContent = content.c_str();
  if (!footer.empty()) config.pszFooter = footer.c_str();
  TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
}

}  // namespace

Result<std::unique_ptr<TrayApp>> TrayApp::Create(TrayOptions options) {
  std::unique_ptr<TrayApp> app(new TrayApp(std::move(options)));
  const TrayOptions& opts = app->options_;
  std::string settings_error;
  if (!opts.settings_file.empty()) {
    if (auto loaded = LoadSettings(opts.settings_file)) {
      app->settings_ = std::move(loaded->core);
      app->first_run_ = loaded->first_run;
    } else {
      settings_error = loaded.error();
    }
  }

  WNDCLASSEXW window_class = {sizeof(window_class)};
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = opts.instance;
  window_class.lpszClassName = opts.window_class.c_str();
  if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return Error{std::format("Couldn't register the tray's window class: {}", DescribeWinError(GetLastError()))};
  }
  // A top-level window, not a message-only one: only those hear
  // TaskbarCreated when Explorer restarts.
  if (!CreateWindowExW(0, opts.window_class.c_str(), std::wstring(kDisplayNameW).c_str(), WS_OVERLAPPED, 0, 0, 0, 0,
                       nullptr, nullptr, opts.instance, app.get())) {
    return Error{std::format("Couldn't create the tray's window: {}", DescribeWinError(GetLastError()))};
  }
  app->taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
  if (app->taskbar_created_) ChangeWindowMessageFilterEx(app->window_, app->taskbar_created_, MSGFLT_ALLOW, nullptr);
  app->LoadIcons();
  ApplyMenuTheme(opts.theme);
  app->AddIcon();

  auto core = opts.start_core(*app, app->settings_);
  if (!core) return Error{core.error()};
  app->core_ = std::move(*core);
  if (!settings_error.empty()) {
    app->ShowError(std::format("Couldn't read the settings, so {} uses its defaults until they're changed.\n\n{}",
                               kDisplayName, settings_error));
  }
  // Once Run pumps messages: the dialog's loop needs Run's to end with it.
  if (opts.open_first_run && !app->first_run_.done) PostMessageW(app->window_, kFirstRunMessage, 0, 0);
  return app;
}

TrayApp::TrayApp(TrayOptions options)
    : options_(std::move(options)),
      settings_(options_.settings),
      first_run_(options_.first_run),
      notifier_({.restarted_for = options_.restarted_for}) {}

TrayApp::~TrayApp() {
  Quit();
  DestroyIcons();
}

int TrayApp::Run() {
  running_ = true;
  MSG message = {};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}

void TrayApp::Quit() {
  if (quitting_) return;
  quitting_ = true;
  core_.reset();  // Stops the chain and libobs, which releases the mic and the cable.
  if (icon_added_) {
    NOTIFYICONDATAW data = {sizeof(data)};
    data.hWnd = window_;
    data.uID = kIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    icon_added_ = false;
  }
  if (window_) DestroyWindow(window_);
}

void TrayApp::OnSnapshot(const core::Snapshot& snapshot) {
  {
    std::lock_guard lock(pending_mutex_);
    pending_ = snapshot;
  }
  PostMessageW(window_, kSnapshotMessage, 0, 0);
}

LRESULT CALLBACK TrayApp::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    auto* app = static_cast<TrayApp*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    app->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
  }
  auto* app = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!app) return DefWindowProcW(window, message, wparam, lparam);
  if (message == WM_NCDESTROY) {
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    app->window_ = nullptr;
    return DefWindowProcW(window, message, wparam, lparam);
  }
  return app->HandleMessage(message, wparam, lparam);
}

LRESULT TrayApp::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
  if (taskbar_created_ && message == taskbar_created_) {
    // Explorer restarted, and the icon went with it, or the taskbar's DPI
    // changed, which may leave it there at the old size.
    NOTIFYICONDATAW data = {sizeof(data)};
    data.hWnd = window_;
    data.uID = kIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    icon_added_ = false;
    LoadIcons();
    AddIcon();
    return 0;
  }
  switch (message) {
    case kTrayMessage:
      switch (LOWORD(lparam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_CONTEXTMENU:
          if (const HWND dialog = OpenDialog()) {
            SetForegroundWindow(dialog);
          } else {
            ShowMenu({GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam)});
          }
          break;
        case NIN_BALLOONUSERCLICK:
          OpenNotice();
          break;
        default:
          break;
      }
      return 0;
    case kSnapshotMessage: {
      std::optional<core::Snapshot> snapshot;
      {
        std::lock_guard lock(pending_mutex_);
        snapshot.swap(pending_);
      }
      if (snapshot) {
        const core::Snapshot before = std::exchange(snapshot_, std::move(*snapshot));
        UpdateTip();
        if (RestartByItself()) return 0;
        if (first_run_dialog_) first_run_dialog_->Changed(before);
        Notify(notifier_.Changed(snapshot_, Quiet(), Clock::now()));
        if (options_.on_snapshot) options_.on_snapshot(snapshot_);
      }
      return 0;
    }
    case WM_TIMER:
      if (wparam != kNoticeTimer) break;
      KillTimer(window_, kNoticeTimer);
      Notify(notifier_.Tick(Quiet(), Clock::now()));
      return 0;
    case kActivateMessage:
      if (const HWND dialog = OpenDialog()) {
        SetForegroundWindow(dialog);
      } else if (!first_run_.done) {
        // Started again before setup was finished: that's what's wanted.
        ShowFirstRun(false);
      } else {
        ShowMenu(IconPoint());
      }
      return 0;
    case kFirstRunMessage:
      if (!first_run_.done) ShowFirstRun(false);
      return 0;
    case WM_SETTINGCHANGE:
      if (wparam == SPI_SETHIGHCONTRAST ||
          (lparam && std::wstring_view(reinterpret_cast<const wchar_t*>(lparam)) == L"ImmersiveColorSet")) {
        ApplyMenuTheme(options_.theme);
      }
      break;
    case WM_QUERYENDSESSION:
      return TRUE;
    case WM_ENDSESSION:
      // Windows is shutting down or signing out, and ends the process after
      // this returns: let go of the mic and the cable first.
      if (wparam) Quit();
      return 0;
    case WM_CLOSE:
      Quit();
      return 0;
    case WM_DESTROY:
      // Only Run's loop is waiting for it. Before that, a WM_QUIT would close
      // the next dialog instead.
      if (running_) PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window_, message, wparam, lparam);
}

void TrayApp::LoadIcons() {
  DestroyIcons();
  const HICON small = LoadAppIcon(options_.instance, LIM_SMALL);
  const HICON large = LoadAppIcon(options_.instance, LIM_LARGE);
  for (const Badge badge : kBadges) {
    icons_[static_cast<size_t>(badge)] = BadgedIcon(small, badge);
    large_icons_[static_cast<size_t>(badge)] = BadgedIcon(large, badge);
  }
  if (small) DestroyIcon(small);
  if (large) DestroyIcon(large);
}

void TrayApp::DestroyIcons() {
  for (std::array<HICON, 3>* icons : {&icons_, &large_icons_}) {
    for (HICON& icon : *icons) {
      if (icon) DestroyIcon(icon);
      icon = nullptr;
    }
  }
}

void TrayApp::AddIcon() {
  NOTIFYICONDATAW data = {sizeof(data)};
  data.hWnd = window_;
  data.uID = kIconId;
  data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  data.uCallbackMessage = kTrayMessage;
  data.hIcon = icons_[static_cast<size_t>(badge_)];
  // Fails while Explorer is starting, at sign-in; TaskbarCreated comes once
  // it's up.
  if (!Shell_NotifyIconW(NIM_ADD, &data)) return;
  data.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &data);
  icon_added_ = true;
  UpdateTip();
}

void TrayApp::UpdateTip() {
  if (!icon_added_) return;
  NOTIFYICONDATAW data = {sizeof(data)};
  data.hWnd = window_;
  data.uID = kIconId;
  data.uFlags = NIF_TIP | NIF_SHOWTIP;
  std::wstring tip = std::format(L"{}\n{}", kDisplayNameW, FromUtf8(StatusLine(snapshot_)));
  if (tip.size() >= std::size(data.szTip)) {
    tip.resize(std::size(data.szTip) - 2);
    tip += L'…';
  }
  tip.copy(data.szTip, tip.size());
  Shell_NotifyIconW(NIM_MODIFY, &data);
}

void TrayApp::UpdateBadge() {
  const Badge badge = notifier_.badge();
  if (badge == badge_) return;
  badge_ = badge;
  if (icon_added_) {
    NOTIFYICONDATAW data = {sizeof(data)};
    data.hWnd = window_;
    data.uID = kIconId;
    data.uFlags = NIF_ICON;
    data.hIcon = icons_[static_cast<size_t>(badge_)];
    Shell_NotifyIconW(NIM_MODIFY, &data);
  }
  if (options_.on_badge) options_.on_badge(badge_);
}

void TrayApp::Notify(const Notifier::Update& update) {
  if (update.show) {
    ShowBalloon(*update.show);
  } else if (update.hide) {
    HideBalloon();
  }
  if ((update.show || update.hide) && options_.on_notice) options_.on_notice(update.show);
  UpdateBadge();
  KillTimer(window_, kNoticeTimer);
  if (const auto next = notifier_.NextDeadline()) {
    const auto wait = std::chrono::ceil<std::chrono::milliseconds>(*next - Clock::now()).count();
    SetTimer(window_, kNoticeTimer, static_cast<UINT>(std::clamp<long long>(wait, USER_TIMER_MINIMUM, 60'000)),
             nullptr);
  }
}

void TrayApp::ShowBalloon(const Notice& notice) {
  if (!icon_added_) return;
  NOTIFYICONDATAW data = {sizeof(data)};
  data.hWnd = window_;
  data.uID = kIconId;
  data.uFlags = NIF_INFO;
  CopyField(data.szInfoTitle, notice.title);
  CopyField(data.szInfo, notice.text);
  // The knob, with the badge for a problem, rather than one of Windows'
  // icons.
  data.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | (notice.sound ? 0u : NIIF_NOSOUND);
  data.hBalloonIcon = large_icons_[static_cast<size_t>(notice.problem ? Badge::kAttention : Badge::kNone)];
  Shell_NotifyIconW(NIM_MODIFY, &data);
}

void TrayApp::HideBalloon() {
  if (!icon_added_) return;
  // An empty text takes it down.
  NOTIFYICONDATAW data = {sizeof(data)};
  data.hWnd = window_;
  data.uID = kIconId;
  data.uFlags = NIF_INFO;
  Shell_NotifyIconW(NIM_MODIFY, &data);
}

bool TrayApp::Quiet() const { return !first_run_.done || first_run_dialog_ != nullptr; }

bool TrayApp::RestartByItself() {
  if (snapshot_.state != core::State::kRestartNeeded) {
    restart_tried_ = false;
    return false;
  }
  if (restart_tried_ || in_menu_ || quitting_ || OpenDialog()) return false;
  // Once: if it fails, the menu and the first run offer it.
  restart_tried_ = true;
  Restart();
  return quitting_;
}

POINT TrayApp::IconPoint() const {
  NOTIFYICONIDENTIFIER icon = {sizeof(icon)};
  icon.hWnd = window_;
  icon.uID = kIconId;
  RECT rect = {};
  if (SUCCEEDED(Shell_NotifyIconGetRect(&icon, &rect))) {
    return {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
  }
  POINT cursor = {};
  GetCursorPos(&cursor);
  return cursor;
}

void TrayApp::ShowMenu(POINT point) {
  if (in_menu_ || quitting_) return;
  in_menu_ = true;
  const bool starts = options_.starts_with_windows && options_.starts_with_windows();
  const Menu menu = BuildMenu(snapshot_, settings_, starts);
  const HMENU native = CreateNativeMenu(menu.items);
  // Without this, the menu wouldn't close when the user clicks elsewhere.
  SetForegroundWindow(window_);
  UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY;
  flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
  const UINT id = TrackPopupMenuEx(native, flags, point.x, point.y, window_, nullptr);
  PostMessageW(window_, WM_NULL, 0, 0);
  DestroyMenu(native);
  in_menu_ = false;
  if (id != 0) Execute(id, menu);
  RestartByItself();
}

void TrayApp::Execute(unsigned id) {
  const bool starts = options_.starts_with_windows && options_.starts_with_windows();
  Execute(id, BuildMenu(snapshot_, settings_, starts));
}

void TrayApp::OpenNotice() {
  const std::optional<Notice> notice = notifier_.shown();
  notifier_.Clicked();
  if (notice && notice->page) {
    ShowFirstRun(false, notice->page);
  } else if (const HWND dialog = OpenDialog()) {
    SetForegroundWindow(dialog);
  } else {
    ShowMenu(IconPoint());
  }
}

void TrayApp::Execute(unsigned id, const Menu& menu) {
  if (const auto picked = SettingsForPick(menu, id, settings_)) {
    ApplySettings(*picked);
    return;
  }
  switch (id) {
    case kIdPause:
      if (core_) core_->Pause();
      break;
    case kIdResume:
      if (core_) core_->Resume();
      break;
    case kIdReimport:
    case kIdTryAgain:
      Reimport();
      break;
    case kIdPauseForObs: {
      core::Settings settings = settings_;
      settings.pause_for_obs = !settings.pause_for_obs;
      ApplySettings(settings);
      break;
    }
    case kIdStartWithWindows:
      ToggleStartWithWindows();
      break;
    case kIdSetup:
      ShowFirstRun(true);
      break;
    case kIdFinishSetup:
    case kIdChooseMic:
    case kIdChooseCable:
      // Opens at the page for it: the state calls for it.
      ShowFirstRun(false);
      break;
    case kIdFindObs:
      FindObs();
      break;
    case kIdOpenLogs:
      OpenLogFolder();
      break;
    case kIdAbout:
      ShowAbout();
      break;
    case kIdRestart:
      Restart();
      break;
    case kIdQuit:
      Quit();
      break;
    default:
      break;
  }
}

void TrayApp::ShowFirstRun(bool from_start, std::optional<FirstRunPage> page) {
  if (quitting_) return;
  if (first_run_dialog_) {
    if (const HWND window = first_run_dialog_->window()) SetForegroundWindow(window);
    return;
  }
  FirstRunProgress progress = first_run_;
  if (progress.done) {
    // Every page again, or only what the state calls for, then the last.
    progress = from_start ? FirstRunProgress{.done = true}
                          : FirstRunProgress{.done = true, .door = Door::kObsUser, .reached = FirstRunPage::kDone};
  }
  const bool starts = options_.starts_with_windows && options_.starts_with_windows();
  FirstRunDialog dialog(*this, options_.instance, FirstRun(progress, page, starts));
  first_run_dialog_ = &dialog;
  dialog.Show();
  first_run_dialog_ = nullptr;
  RestartByItself();
}

void TrayApp::ApplySettings(const core::Settings& settings) {
  settings_ = settings;
  if (core_) core_->Apply(settings_);
  Save();
}

void TrayApp::Reimport() {
  if (core_) core_->Reimport();
}

Status TrayApp::StartObs(const std::filesystem::path& install_root) {
  return options_.open_obs ? options_.open_obs(install_root) : OpenObs(install_root);
}

void TrayApp::FinishFirstRun(bool start_with_windows) {
  const bool starts = options_.starts_with_windows && options_.starts_with_windows();
  if (options_.set_start_with_windows && start_with_windows != starts) {
    if (const Status set = options_.set_start_with_windows(start_with_windows); !set) ShowError(set.error());
  }
  first_run_.done = true;
  Save();
}

void TrayApp::SaveFirstRun(const FirstRunProgress& progress) {
  first_run_ = progress;
  Save();
}

void TrayApp::Save() {
  if (options_.settings_file.empty()) return;
  if (const Status saved = SaveSettings(options_.settings_file, {settings_, first_run_}); !saved) {
    ShowError(std::format("Couldn't save the change, so it lasts only until {} quits.\n\n{}", kDisplayName,
                          saved.error()));
  }
}

void TrayApp::ToggleStartWithWindows() {
  if (!options_.set_start_with_windows) return;
  const bool on = !(options_.starts_with_windows && options_.starts_with_windows());
  if (const Status set = options_.set_start_with_windows(on); !set) ShowError(set.error());
}

void TrayApp::FindObs() {
  const auto folder = runtime::PickObsInstallFolder(nullptr);
  if (!folder) return;
  core::Settings settings = settings_;
  settings.obs_dir = *folder;
  ApplySettings(settings);
}

void TrayApp::OpenLogFolder() {
  std::error_code ec;
  std::filesystem::create_directories(options_.log_folder, ec);
  const auto opened = reinterpret_cast<INT_PTR>(
      ShellExecuteW(nullptr, L"open", options_.log_folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (opened <= 32) ShowError(std::format("Couldn't open the log folder, {}.", ToUtf8(options_.log_folder)));
}

void TrayApp::Restart() {
  if (!options_.restart) return;
  if (const Status started = options_.restart(snapshot_.restart); !started) {
    ShowError(started.error());
    return;
  }
  Quit();
}

void TrayApp::ShowAbout() {
  const HICON icon = LoadAppIcon(options_.instance, LIM_LARGE);
  ShowDialog(options_.instance, nullptr, icon, std::format(L"{} {}", kDisplayNameW, L"" KNOBS_VERSION),
             std::format(L"Your OBS mic chain, without OBS. {0} runs the filters from your own OBS Studio "
                         L"install and sends your mic to a virtual cable.\n\n{0} isn't affiliated with or endorsed "
                         L"by the OBS Project.",
                         kDisplayNameW),
             L"Free software under the GNU General Public License, version 2 or later.");
  if (icon) DestroyIcon(icon);
}

void TrayApp::ShowError(std::string_view text) {
  ShowDialog(options_.instance, TD_WARNING_ICON, nullptr, L"", FromUtf8(text));
}

HWND TrayApp::OpenDialog() const {
  struct Search {
    HWND tray;
    HWND found = nullptr;
  } search{window_};
  EnumThreadWindows(
      GetCurrentThreadId(),
      [](HWND window, LPARAM param) -> BOOL {
        auto* search = reinterpret_cast<Search*>(param);
        if (window == search->tray || !IsWindowVisible(window)) return TRUE;
        search->found = window;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&search));
  return search.found;
}

bool ActivateTray(const std::wstring& window_class) {
  const HWND window = FindWindowW(window_class.c_str(), nullptr);
  if (!window) return false;
  DWORD process = 0;
  GetWindowThreadProcessId(window, &process);
  // Lets it bring its menu to the front.
  AllowSetForegroundWindow(process);
  return PostMessageW(window, kActivateMessage, 0, 0) != FALSE;
}

void ShowStartupError(std::string_view text) {
  ShowDialog(GetModuleHandleW(nullptr), TD_ERROR_ICON, nullptr, L"", FromUtf8(text));
}

}  // namespace knobs::tray
