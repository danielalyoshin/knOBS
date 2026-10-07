// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <windows.h>
#include <commctrl.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/state.h"
#include "tray/first_run.h"
#include "util/result.h"

// The first run's window (docs/design.md, Tray and first run): one task dialog
// that shows FirstRun's pages and moves between them with TDM_NAVIGATE_PAGE, as
// the user clicks and as the core's state changes.
namespace knobs::tray {

// What the first run needs from the tray. Called on the tray's thread.
class FirstRunHost {
 public:
  virtual ~FirstRunHost() = default;
  // The core's latest state.
  virtual const core::Snapshot& snapshot() const = 0;
  virtual void ApplySettings(const core::Settings& settings) = 0;
  virtual void Reimport() = 0;
  // Starts a new copy and quits. Returns false if it couldn't, once it has
  // said why: the tray runs on.
  virtual bool Restart() = 0;
  // Starts OBS from its install (OpenObs).
  virtual Status StartObs(const std::filesystem::path& install_root) = 0;
  // Done was clicked, with Start with Windows as `start_with_windows`.
  virtual void FinishFirstRun(bool start_with_windows) = 0;
  virtual void SaveFirstRun(const FirstRunProgress& progress) = 0;
};

class FirstRunDialog {
 public:
  FirstRunDialog(FirstRunHost& host, HINSTANCE instance, FirstRun first_run);
  ~FirstRunDialog();
  FirstRunDialog(const FirstRunDialog&) = delete;
  FirstRunDialog& operator=(const FirstRunDialog&) = delete;

  // Shows the dialog, and returns once it's closed. Snapshots keep arriving
  // meanwhile: the tray's messages are pumped by the dialog's.
  void Show();
  // The host's snapshot has moved on from `before`.
  void Changed(const core::Snapshot& before);
  // Null until it's shown, and after.
  HWND window() const { return window_; }

 private:
  // A page's TASKDIALOGCONFIG and the strings it points to.
  struct Config {
    TASKDIALOGCONFIG config = {};
    std::wstring title, instruction, content, footer, check;
    std::vector<std::wstring> button_texts, choice_texts;
    std::vector<TASKDIALOG_BUTTON> buttons, choices;
  };

  static HRESULT CALLBACK Callback(HWND window, UINT notification, WPARAM wparam, LPARAM lparam, LONG_PTR data);
  HRESULT OnButton(int id);
  // Shows the page for the host's snapshot, if it's changed.
  void Render();
  Config& Fill(const PageView& view);
  // After a page appears: what the config can't say.
  void ApplyStates();
  void Apply(const core::Settings& settings);
  void Save();
  void ShowError(const std::string& text);

  FirstRunHost& host_;
  HINSTANCE instance_;
  FirstRun first_run_;
  FirstRunProgress saved_;
  HWND window_ = nullptr;
  HICON icon_large_ = nullptr;
  HICON icon_small_ = nullptr;
  // Two, so a page's strings outlive the move to the next.
  std::array<Config, 2> configs_;
  size_t next_config_ = 0;
  PageView shown_;
  int choice_ = 0;
  bool checked_ = false;
  // Settings applied, until the core's snapshots carry them.
  std::optional<core::Settings> pending_;
  // In a button's handler, which may run a dialog of its own: changes wait
  // until it returns.
  bool in_button_ = false;
  bool render_due_ = false;
  std::optional<core::Snapshot> changed_from_;
};

}  // namespace knobs::tray
