// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <windows.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "core/core.h"
#include "core/state.h"
#include "tray/first_run.h"
#include "tray/first_run_dialog.h"
#include "tray/menu.h"
#include "tray/menu_theme.h"
#include "util/result.h"

// The tray icon and its menu (plan.md, Tray and first run). It runs the core
// and shows its state: the icon's tooltip and the menu's first line are the
// status line, and the menu offers the fix for whatever needs the user.
namespace knobs::tray {

struct TrayOptions {
  HINSTANCE instance = nullptr;  // Holds the icon (IDI_KNOBS).
  // Names the hidden window's class, so another copy can find it
  // (ActivateTray).
  std::wstring window_class;
  // Starts the core, which reports to `observer`. A null core leaves the
  // tray starting for good (knobs-tray does that to show the state).
  std::function<Result<std::unique_ptr<core::Core>>(core::Observer& observer, const core::Settings& settings)>
      start_core;
  // Where the settings are kept. Empty: `settings` is used and nothing is
  // saved.
  std::filesystem::path settings_file;
  core::Settings settings;
  // Opens the first run once the tray runs, unless it was finished before.
  bool open_first_run = false;
  // Start with Windows.
  std::function<bool()> starts_with_windows;
  std::function<Status(bool on)> set_start_with_windows;
  // Starts a new copy, which waits for this one to quit.
  std::function<Status()> restart;
  // Starts OBS from its install, for the first run. Default: OpenObs.
  std::function<Status(const std::filesystem::path& install_root)> open_obs;
  std::filesystem::path log_folder;
  MenuTheme theme = MenuTheme::kSystem;
  // Hears each snapshot on the tray's thread, after the tray has.
  std::function<void(const core::Snapshot& snapshot)> on_snapshot;
};

class TrayApp final : public core::Observer, private FirstRunHost {
 public:
  // Creates the hidden window and the icon, and starts the core. Call from
  // a thread that is a COM single-threaded apartment and pumps messages
  // (Run).
  static Result<std::unique_ptr<TrayApp>> Create(TrayOptions options);
  ~TrayApp() override;
  TrayApp(const TrayApp&) = delete;
  TrayApp& operator=(const TrayApp&) = delete;

  // Pumps messages until Quit. Returns the exit code.
  int Run();
  // Stops the core, removes the icon and ends Run.
  void Quit();

  // Opens the menu at `point`, in screen coordinates, as a click on the icon
  // does, and carries out what's picked. Returns once the menu has closed.
  void ShowMenu(POINT point);
  // Carries out a menu command as if it had been picked.
  void Execute(unsigned id);
  // Opens the first run, or brings it forward, and returns once it's
  // closed. Unfinished, it resumes where it stopped; finished, it starts
  // over if `from_start`, and otherwise shows only what the state needs.
  // `page` opens that page instead (the dev tools show any page with it).
  void ShowFirstRun(bool from_start, std::optional<FirstRunPage> page = std::nullopt);

  HWND window() const { return window_; }
  const core::Snapshot& snapshot() const override { return snapshot_; }
  const core::Settings& settings() const { return settings_; }
  const FirstRunProgress& first_run() const { return first_run_; }

  // core::Observer, on the core's thread.
  void OnSnapshot(const core::Snapshot& snapshot) override;

 private:
  explicit TrayApp(TrayOptions options);
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

  void AddIcon();
  void UpdateTip();
  POINT IconPoint() const;
  void Execute(unsigned id, const Menu& menu);
  void Save();
  void ToggleStartWithWindows();
  void FindObs();
  void OpenLogFolder();
  void ShowAbout();
  void ShowError(std::string_view text);

  // FirstRunHost.
  void ApplySettings(const core::Settings& settings) override;
  void Reimport() override;
  void Restart() override;
  Status StartObs(const std::filesystem::path& install_root) override;
  void FinishFirstRun(bool start_with_windows) override;
  void SaveFirstRun(const FirstRunProgress& progress) override;

  // A window of ours that's open (a dialog), to bring forward rather than
  // open the menu over it.
  HWND OpenDialog() const;

  TrayOptions options_;
  HWND window_ = nullptr;
  HICON icon_ = nullptr;
  UINT taskbar_created_ = 0;
  bool icon_added_ = false;
  bool running_ = false;  // In Run.
  bool in_menu_ = false;
  bool quitting_ = false;
  core::Settings settings_;
  FirstRunProgress first_run_;
  core::Snapshot snapshot_;
  std::unique_ptr<core::Core> core_;
  FirstRunDialog* first_run_dialog_ = nullptr;  // While it's open.

  std::mutex pending_mutex_;  // Guards pending_.
  std::optional<core::Snapshot> pending_;
};

// Tells a running copy, through its window, that another copy was started:
// it opens its menu, so the user sees where knobs is. Returns false if no
// copy's window was found.
bool ActivateTray(const std::wstring& window_class);

// A dialog for an error before there's a tray to show it.
void ShowStartupError(std::string_view text);

}  // namespace knobs::tray
