// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "audio/device_watch.h"
#include "core/backend.h"
#include "core/state.h"

namespace knobs::core {

// The core's decisions. It has no threads of its own: it's told what
// happened and when, and drives a Backend. It reads the time itself only
// when a chain has started (Timing::clock). Not thread-safe; the Core runs
// it on one thread.
//
// After every event it works out the state from what it knows, in this
// order: problems with OBS, its settings or the import; a chain that failed
// to start; paused by the user; paused while OBS runs, unless
// Settings::pause_for_obs is off; the cable missing; the mic missing. The
// chain runs only while the state is kRunning. Every other state releases
// it, which frees the mic and the cable, and the next kRunning loads it
// again with libobs's loader (plan.md, M3 findings). A re-import while the
// chain runs reloads it only if the chain changed (ChainPlan::SameAs). While
// the chain runs, a watchdog checks that it gets audio, and the monitor is
// restarted now and then while the output is silent, so the delay to the
// cable can't creep up (Timing).
class Controller {
 public:
  using Clock = std::chrono::steady_clock;
  using Publish = std::function<void(const Snapshot&)>;

  struct Timing {
    // Device notifications come in bursts, e.g. while an audio interface
    // powers up. The devices are listed again once notifications have
    // stopped for this long...
    Clock::duration device_settle = std::chrono::milliseconds(500);
    // ...or this long after the first one, whichever comes first.
    Clock::duration device_settle_max = std::chrono::seconds(2);
    // How often the watchdog checks that the running chain gets audio.
    Clock::duration watchdog_interval = std::chrono::seconds(1);
    // A chain with no audio for this long has stalled: win-wasapi stopped
    // capturing, and without video nothing restarts it (M3 findings).
    Clock::duration stall_timeout = std::chrono::seconds(3);
    // How long to wait before rebuilding a stalled chain, after the first
    // stall, the second, and so on. The last one repeats.
    std::vector<Clock::duration> retry_delays = {std::chrono::seconds(2), std::chrono::seconds(5),
                                                 std::chrono::seconds(15), std::chrono::seconds(30),
                                                 std::chrono::seconds(60)};
    // A chain that has had audio for this long starts the waits over.
    Clock::duration healthy_after = std::chrono::seconds(30);
    // Restarting the monitor (plan.md, Long-run latency). libobs's monitor
    // doesn't correct its delay for a mic: a hiccup raises it, and a mic
    // whose clock runs faster than the cable's raises it steadily, until the
    // monitor's 1 s buffer overflows. A restart opens a fresh stream, at the
    // least delay. Not sooner than this after the chain started or last
    // restarted: the iD4's clock and VB-Cable's are 31.5 ppm apart, and a mic
    // that much faster than its cable adds 19 ms in 10 min, two engine
    // periods. Where a restart lands varies by about one...
    Clock::duration monitor_restart_every = std::chrono::minutes(10);
    // ...and only once the chain's output has been silent for this long, so
    // the gap a restart leaves falls in silence.
    Clock::duration monitor_restart_silence = std::chrono::seconds(3);
    // A mic without a gate is never silent. Then silent means as quiet as it
    // has been in this long: its noise floor.
    Clock::duration noise_floor_window = std::chrono::minutes(5);
    // Read once a chain has started, since the watchdog times it from then.
    // That can be seconds after the event that started it: the first start
    // makes the runtime copy. Tests on a fake clock pass theirs.
    std::function<Clock::time_point()> clock = Clock::now;
  };

  Controller(Backend& backend, Settings settings, Publish publish, Timing timing = {});

  // Publishes kStarting, then imports and starts.
  void Start(bool obs_running, Clock::time_point now);
  // OBS started or exited. When it exits, its settings are imported again.
  void SetObsRunning(bool running, Clock::time_point now);
  // OBS running in other Windows sessions (Snapshot::other_obs).
  void SetOtherObs(std::vector<OtherObs> others, Clock::time_point now);
  // A device notification. Acted on once they settle (Timing).
  void DevicesChanged(Clock::time_point now);
  void Pause(Clock::time_point now);
  void Resume(Clock::time_point now);
  void Reimport(Clock::time_point now);
  // New settings, then a re-import if the install, OBS's settings folder or
  // the mic changed.
  void Apply(const Settings& settings, Clock::time_point now);
  // Does whatever is due: settled device changes, the watchdog, a retry.
  void Tick(Clock::time_point now);
  // When Tick next has something to do, if ever.
  std::optional<Clock::time_point> NextDeadline() const;
  // Releases the chain for good. Nothing is published after this.
  void Stop();

  const Snapshot& snapshot() const { return snapshot_; }

 private:
  struct Problem {
    State state;
    std::string detail;
    SetupNeed setup = SetupNeed::kNone;
    RestartNeed restart = RestartNeed::kNone;
  };
  // What libobs was started with.
  struct Libobs {
    runtime::ObsVersion version;
    uint32_t sample_rate = 0;
    speaker_layout speakers = SPEAKERS_UNKNOWN;
    std::string channel_setup;
  };

  // Finds OBS, reads its settings, starts libobs if needed, and imports.
  void Refresh();
  // Works out the cable and the chain to run, from an import that found the
  // mic and the settings.
  void PlanChain();
  // Works out the state, starts or stops the chain to match, publishes.
  void Reconcile(Clock::time_point now);
  Snapshot Decide(Clock::time_point now) const;
  void StopChain();
  // Forgets failures, so the next Reconcile tries again.
  void ClearFailures();
  // Hears the running chain's peak since the last watchdog check, and
  // restarts the monitor once that's due and the output has been silent
  // long enough.
  void FollowLevel(float peak, Clock::time_point now);
  bool MicIsDefault() const;
  bool MicPresent() const;
  bool CablePresent() const;
  std::string CableName() const;

  Backend& backend_;
  Settings settings_;
  Publish publish_;
  Timing timing_;
  Snapshot snapshot_;
  bool stopped_ = false;

  // From the last Refresh.
  std::optional<Problem> problem_;
  runtime::ObsInstall obs_install_;
  bool obs_writes_config_ = false;  // Snapshot::obs_writes_config
  std::optional<Libobs> libobs_;
  // libobs failed to start, and can't start again in this process
  // (Backend::LibobsCanRetry).
  bool libobs_spent_ = false;
  std::optional<MicImport> import_;
  std::optional<ChainPlan> plan_;
  std::string profile_cable_id_;
  std::string profile_cable_name_;
  std::string chain_key_;
  uint32_t chain_revision_ = 0;

  audio::Endpoints devices_;
  std::optional<Clock::time_point> devices_first_;  // First unhandled notification.
  std::optional<Clock::time_point> devices_due_;

  bool paused_by_user_ = false;
  bool obs_running_ = false;
  std::vector<OtherObs> other_obs_;

  // The running chain.
  std::optional<ChainPlan> loaded_;
  std::string loaded_default_mic_;  // The device a "default" mic opened.
  std::optional<std::string> chain_error_;
  Clock::time_point running_since_;
  // The watchdog.
  Clock::time_point next_check_;
  uint64_t last_packets_ = 0;
  Clock::time_point last_audio_;
  size_t stalls_ = 0;  // In a row, without a healthy run between.
  std::optional<Clock::time_point> retry_at_;
  std::string stall_detail_;
  // Restarting the monitor.
  Clock::time_point monitor_since_;  // When the chain started or the monitor last restarted.
  bool monitor_restarted_ = false;   // Since the chain started.
  Clock::time_point last_sound_;     // The last check that heard more than silence.
  std::deque<float> peaks_;          // Each check's peak over Timing::noise_floor_window, newest last.
};

}  // namespace knobs::core
