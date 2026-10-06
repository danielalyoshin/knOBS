// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/state.h"
#include "tray/badge.h"
#include "tray/first_run.h"

// The tray's notifications as data (plan.md, Tray and first run): which
// changes of the core's state get one, what it says, and what a click on it
// opens. Each says what happened, then what knobs does or what to do. There
// are none for a normal start, or when OBS opens or closes without changing
// the mic. The icon's badge comes from here too, since Do Not Disturb can
// hide the notifications. tray_app.cpp shows them as Shell_NotifyIcon
// balloons. Text is UTF-8.
namespace knobs::tray {

struct Notice {
  enum class Kind {
    kMicMissing,      // For Notifier::Options::device_grace.
    kCableMissing,    // Likewise.
    kObsUnsupported,  // OBS was updated to a version knobs doesn't support.
    kObsMissing,
    kNeedsSetup,      // Something changed in OBS that only the user can settle.
    kFailed,
    kChainChanged,    // The mic or its chain changed in OBS.
    kFilterWarnings,  // New pre-flight warnings: filters knobs can't run.
    kRestarted,       // The copy before this one restarted to follow OBS.
    kOtherObs,        // OBS opened in another Windows session.
  };

  Kind kind = Kind::kChainChanged;
  std::string title;  // What happened.
  std::string text;   // What knobs does, or what to do.
  // The cable gets no mic. Shown with the attention badge.
  bool problem = false;
  // Played with a sound: a problem, or a filter that won't sound as in OBS.
  bool sound = false;
  // What a click opens: the first run at this page, or else the tray menu.
  std::optional<FirstRunPage> page;

  friend bool operator==(const Notice&, const Notice&) = default;
};

// "obs-updated", "audio-changed": how a restart says why, on the new copy's
// command line.
std::string_view RestartNeedName(core::RestartNeed need);
std::optional<core::RestartNeed> RestartNeedNamed(std::string_view name);

// Follows the core's snapshots, and time, and says when to show or take down
// a notification. The rules:
// - Problems, the states where the cable gets no mic, get one as they begin:
//   a mic or cable missing once it has been for Options::device_grace, the
//   others at once. A restart that's needed gets none: the tray restarts by
//   itself. While paused, nothing is missing.
// - A problem's notification is taken down when it ends.
// - The mic or its chain changing in OBS, and new pre-flight warnings, get
//   one after the first import, but a change knobs's own settings made gets
//   none.
// - After a restart to follow OBS, the new copy says why.
// - OBS opening in another Windows session gets one, unless one of the
//   above is shown at the same time, and it's taken down when that OBS
//   closes.
// - Nothing shows while `quiet`: the first run is unfinished or open, and
//   shows the state itself. What it would have said isn't said later.
class Notifier {
 public:
  using Clock = std::chrono::steady_clock;

  struct Options {
    // A mic or cable missing this long gets a notification, and the
    // attention badge. Shorter, and an audio interface's power cycle, or
    // Windows' audio service restarting, would get one.
    Clock::duration device_grace = std::chrono::seconds(5);
    // A mic that stalled is back once its chain has run this long. The
    // core's watchdog takes up to 4 s to see that a rebuilt chain stalled
    // again (core::Controller::Timing), so a mic that keeps stalling stays
    // missing. A mic whose device came back is back at once.
    Clock::duration mic_back_after = std::chrono::seconds(5);
    // Why the copy before this one restarted it, if it did.
    core::RestartNeed restarted_for = core::RestartNeed::kNone;
  };

  struct Update {
    std::optional<Notice> show;
    // Take down the notification showing: it no longer holds.
    bool hide = false;
  };

  Notifier() : Notifier(Options{}) {}
  explicit Notifier(Options options);

  // A new snapshot.
  Update Changed(const core::Snapshot& snapshot, bool quiet, Clock::time_point now);
  // Does what's due by `now` (NextDeadline).
  Update Tick(bool quiet, Clock::time_point now);
  std::optional<Clock::time_point> NextDeadline() const;

  Badge badge() const;
  // The notification last shown, until it's taken down or clicked: what a
  // click on it opens.
  const std::optional<Notice>& shown() const { return shown_; }
  // It was clicked, and Windows took it down.
  void Clicked() { shown_.reset(); }

 private:
  // A mic or cable missing, from the first snapshot that said so.
  struct Missing {
    Clock::time_point since;
    // Past the grace: notified, unless quiet then.
    bool due = false;
    // A mic only: its device is connected, and the chain stalled.
    bool stalled = false;
    // A stalled mic only: running again since, but not for long enough to
    // be back.
    std::optional<Clock::time_point> running_since;
  };

  // `stalled` is whether a missing mic's device is connected.
  void Follow(std::optional<Missing>& missing, bool now_missing, bool stalled, bool running,
              Clock::time_point now) const;
  // Shows what `missing` is due to say, once.
  void Due(std::optional<Missing>& missing, Notice::Kind kind, bool quiet, Clock::time_point now, Update& update);
  void Show(Notice notice, bool quiet, Update& update);
  // Takes down the notification showing if it's a `kind`.
  void Hide(Notice::Kind kind, Update& update);

  Options options_;
  core::Snapshot snapshot_;
  bool started_ = false;
  std::optional<Missing> mic_;
  std::optional<Missing> cable_;
  // The problem other than a missing device that was last notified, as its
  // state and setup need.
  std::optional<std::pair<core::State, core::SetupNeed>> problem_;
  // What the last import that found the mic found.
  uint32_t chain_revision_ = 0;
  // Its warnings, by what each is about (WarningKey).
  std::vector<std::string> warnings_;
  std::string mic_name_;
  std::vector<core::OtherObs> other_obs_;
  runtime::ObsVersion obs_version_;
  std::optional<Notice> shown_;
};

}  // namespace knobs::tray
