// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/state.h"

// The first run as data (plan.md, Tray and first run): which page to show for
// a snapshot of the core, what the page says and offers, and what each button
// does. first_run_dialog.cpp shows the pages in one task dialog. Text is
// UTF-8. Content and footers may hold links, <a href="…">as task dialogs take
// them</a>.
namespace knobs::tray {

// In the order the first door goes through them.
enum class FirstRunPage {
  kWelcome,  // The two doors.
  kSteps,    // The second door: setting up a mic in OBS.
  // The first door. These five come up whenever the core's state calls for
  // them, and block the rest.
  kLooking,         // Starting: finding OBS and reading its settings.
  kFindObs,         // No OBS install found.
  kObsUnsupported,  // The install's version isn't supported.
  kObsSettings,     // OBS's settings can't be read.
  kProblem,         // Failed, or a restart is needed.
  // Then, each only when it's needed, or until it's answered.
  kMic,       // Which mic, when OBS has several. Needed while none is picked.
  kFilters,   // The mic has no filters. Needed while OBS has no mic.
  kCable,     // Which cable, when OBS doesn't monitor to one. Needed while
              // none is chosen or the chosen one isn't connected.
  kWarnings,  // Pre-flight warnings, and notes that change what to expect.
  kDone,      // The chain, where to find knobs, and Start with Windows.
};

// "welcome", "cable", ...: for the settings file and the dev tools.
std::string_view PageName(FirstRunPage page);
std::optional<FirstRunPage> PageNamed(std::string_view name);

enum class Door {
  kNone,      // Not chosen yet: the welcome page.
  kObsUser,   // "I set up my mic in OBS".
  kNewToObs,  // "I'm new to OBS".
};

// How far a first run got, saved so that an unfinished one resumes there.
struct FirstRunProgress {
  // Finished once. knobs no longer opens it by itself.
  bool done = false;
  Door door = Door::kNone;
  // The first door's pages from kMic up to this one are answered, and show
  // again only if they're needed.
  FirstRunPage reached = FirstRunPage::kMic;

  friend bool operator==(const FirstRunProgress&, const FirstRunProgress&) = default;
};

// Button and choice IDs. Choices (radio buttons) are numbered from
// kChoiceFirst, in the order the page lists them.
enum FirstRunId : int {
  kButtonObsUser = 100,  // The doors, as command links.
  kButtonNewToObs,
  kButtonBack,
  kButtonNext,
  kButtonDone,
  kButtonOpenObs,
  kButtonChooseObs,          // OBS's install folder.
  kButtonChooseObsSettings,  // OBS's settings folder.
  kButtonTryAgain,
  kButtonRestart,
  kButtonShowSteps,      // To the second door.
  kButtonSkipFilters,    // Go on with a mic that has no filters.
  kButtonKeepDevice,     // Go on with a playback device that isn't a cable.
  kChoiceFirst = 200,
};

enum class PageIcon { kKnob, kInformation, kWarning, kError };

struct PageButton {
  int id = 0;
  std::string text;  // A command link's is "Text\nNote".
  bool enabled = true;

  friend bool operator==(const PageButton&, const PageButton&) = default;
};

struct PageView {
  FirstRunPage page = FirstRunPage::kWelcome;
  PageIcon icon = PageIcon::kKnob;
  std::string instruction;  // The heading.
  std::string content;
  std::string footer;
  // The buttons are command links (the doors), not push buttons.
  bool command_links = false;
  std::vector<PageButton> buttons;
  int default_button = 0;
  std::vector<PageButton> choices;
  int default_choice = 0;
  // A check box under the content, if there's text for one.
  std::string check;
  bool checked = false;
  // The page waits for something and moves on by itself.
  bool waiting = false;

  friend bool operator==(const PageView&, const PageView&) = default;
};

// What a click or a change asks of the dialog, besides showing the page
// that's next.
struct FirstRunAction {
  enum class Kind {
    kNone,
    kApply,            // Apply `settings`, and wait for the core to take them.
    kReimport,         // Try again.
    kOpenObs,          // Start obs64.exe.
    kChooseObs,        // Ask for OBS's install folder (Settings::obs_dir).
    kChooseObsSettings,  // Ask for OBS's settings folder (Settings::obs_config).
    kRestart,          // Restart knobs.
    kFinish,           // Done: close, and set Start with Windows.
  };
  Kind kind = Kind::kNone;
  core::Settings settings;          // kApply.
  bool start_with_windows = false;  // kFinish.
};

// One first run: the progress, and the pages visited, for Back. Snapshots
// passed in carry the settings they were worked out with
// (core::Snapshot::settings); changes are made to those.
class FirstRun {
 public:
  // Starts from `progress`, at `page` if given (the dev tools open any page
  // with it). A page that doesn't apply to the state shows the page that
  // does instead.
  explicit FirstRun(FirstRunProgress progress, std::optional<FirstRunPage> page = std::nullopt);

  const FirstRunProgress& progress() const { return progress_; }

  FirstRunPage Page(const core::Snapshot& snapshot) const;
  // The page for `snapshot`, with `choice` selected if the page offers it,
  // and the check box as `checked` if it has one; 0 and nullopt mean the
  // page's defaults.
  PageView View(const core::Snapshot& snapshot, int choice = 0, std::optional<bool> checked = std::nullopt) const;

  // A button was clicked on the page View showed for `snapshot`.
  FirstRunAction Click(int button, int choice, bool checked, const core::Snapshot& snapshot);
  // The core moved on from `before` to `after`: OBS closing on the second
  // door, or a cable appearing on the cable page, moves the first run on.
  FirstRunAction Changed(const core::Snapshot& before, const core::Snapshot& after);

 private:
  void Leave(FirstRunPage page);
  void Back(const core::Snapshot& snapshot);

  FirstRunProgress progress_;
  // A page shown out of turn: one gone Back to, or asked for.
  std::optional<FirstRunPage> focus_;
  std::vector<FirstRunPage> history_;
};

// Whether any of the collection's mics has a filter that's on.
bool AnyFilters(const core::Snapshot& snapshot);

// The playback devices the cable page offers: virtual cables, then the
// device knobs sends to if it's connected and not a cable.
std::vector<audio::AudioDevice> CableChoices(const core::Snapshot& snapshot);

}  // namespace knobs::tray
