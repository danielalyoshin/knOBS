// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "audio/audio_devices.h"
#include "import/mic_import.h"

// What knobs is doing, as the tray shows it (plan.md, Tray and first run).
namespace knobs::core {

enum class State {
  // Finding OBS, starting libobs and importing. The first state.
  kStarting,
  // The mic runs through its chain into the cable.
  kRunning,
  // Paused from the tray. Stays paused, whatever OBS does, until resumed.
  kPausedByUser,
  // Paused while OBS runs, so the cable never gets the mic twice. Resumes
  // when OBS exits, after a re-import.
  kPausedForObs,
  // Needs something only the user can settle; Snapshot::setup says what.
  kNeedsSetup,
  // The mic's recording device isn't connected, or it stopped sending audio.
  // Resumes by itself when the mic is back.
  kMicMissing,
  // The cable isn't connected. Resumes by itself when it's back.
  kCableMissing,
  // No OBS install was found.
  kObsMissing,
  // The OBS install is a version knobs doesn't support.
  kObsUnsupported,
  // OBS was updated, or its profile's sample rate or channels changed.
  // knobs has to restart to follow: libobs can't be reloaded in a running
  // process, and OBS restarts for a format change too.
  kRestartNeeded,
  // Something went wrong that knobs can't fix by itself; Snapshot::detail
  // says what. Tried again on a re-import and when OBS exits; a chain that
  // failed to start also on a device change or a resume.
  kFailed,
};

// "running", "paused while OBS is open", ...
std::string_view StateName(State state);

enum class SetupNeed {
  kNone,
  // OBS's settings couldn't be read: OBS hasn't been run and set up yet, or
  // keeps its settings somewhere knobs doesn't know (Settings::obs_config).
  kObsSettings,
  // The scene collection has no mic.
  kNoMic,
  // The scene collection has several mics, and none is picked
  // (Settings::mic), or the pick matches none.
  kPickMic,
  // The OBS profile monitors to the default playback device, usually
  // speakers, and no cable is picked (Settings::cable).
  kCable,
};

// The chain as the user knows it from OBS.
struct ChainSummary {
  std::string mic;                   // As OBS names it, e.g. "Mic/Aux".
  std::vector<std::string> filters;  // The filters that are on, in order.
  std::string cable;                 // Empty until a cable is known.

  friend bool operator==(const ChainSummary&, const ChainSummary&) = default;
};

// "Mic/Aux › 3-Band EQ › Compressor › CABLE In 16ch", or in the short form
// the tray menu and tooltip use, "Mic/Aux › 2 filters › CABLE In 16ch".
std::string FormatChain(const ChainSummary& chain, bool short_form);

struct Snapshot {
  State state = State::kStarting;
  SetupNeed setup = SetupNeed::kNone;  // With kNeedsSetup.
  // What happened, in a sentence or two, for states that need explaining.
  std::string detail;
  // The imported chain, once an import has got as far as the mic.
  std::optional<ChainSummary> chain;
  // The scene collection's mics, to pick from, and which one is picked.
  std::vector<import::MicCandidate> mics;
  std::optional<size_t> picked_mic;
  // Pre-flight notes on the picked mic.
  std::vector<import::ImportNote> notes;
  // Goes up each time an import finds that the mic or its chain changed in a
  // way that reaches the cable (import::ImportedMic::chain_key). 0 until the
  // first import, then 1.
  uint32_t chain_revision = 0;
  bool obs_running = false;
  // Paused from the tray, even while a state above kPausedByUser shows.
  bool paused_by_user = false;
  // The OBS profile's monitoring device, once a profile has been read. Its
  // ID may be "default".
  audio::AudioDevice obs_cable;
  // The playback devices that are connected, to pick a cable from.
  std::vector<audio::AudioDevice> outputs;

  friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

// One line for logs and the dev tools: "running: Mic/Aux › … › CABLE In 16ch".
std::string DescribeSnapshot(const Snapshot& snapshot);

// Hears the core's state. Called on the core's thread: first with
// kStarting, then whenever anything in the snapshot changes. Copy what's
// needed and return quickly. Calling the core's commands from here is fine.
class Observer {
 public:
  virtual ~Observer() = default;
  virtual void OnSnapshot(const Snapshot& snapshot) = 0;
};

}  // namespace knobs::core
