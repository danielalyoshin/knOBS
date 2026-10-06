// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "audio/audio_devices.h"
#include "import/mic_import.h"
#include "runtime/obs_install.h"

// What knobs is doing, as the tray shows it (plan.md, Tray and first run).
namespace knobs::core {

// knobs's own choices. They override what it imports, and survive
// re-imports.
struct Settings {
  // The OBS install. Default: found as runtime::FindObsInstall finds it.
  std::optional<std::filesystem::path> obs_dir;
  // OBS's settings folder, the one holding obs-studio\. Default: where OBS
  // keeps it (import::FindObsConfigRoot).
  std::optional<std::filesystem::path> obs_config;
  // Which mic, by name (import::PickMicByName). Empty picks the only one.
  std::string mic;
  // The playback device to send the mic to, by endpoint ID. Empty: the OBS
  // profile's monitoring device.
  std::string cable;
  // Its name, for saying which device is missing while it isn't connected.
  std::string cable_name;
  // Pause while OBS runs (State::kPausedForObs). Off, the chain keeps running
  // while OBS is open; OBS exiting still re-imports.
  bool pause_for_obs = true;

  friend bool operator==(const Settings&, const Settings&) = default;
};

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
  // process, and OBS restarts for a format change too. Also, with
  // RestartNeed::kNone, when trying again after libobs failed to start with
  // a module loaded: only a new process can.
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

enum class RestartNeed {
  kNone,
  // The OBS install is a new version, which needs a new runtime copy.
  kObsUpdated,
  // The OBS profile's sample rate or channels changed.
  kAudioChanged,
};

// The chain as the user knows it from OBS.
struct ChainSummary {
  std::string mic;                   // As OBS names it, e.g. "Mic/Aux".
  std::vector<std::string> filters;  // The filters that run, in order (import::ImportedMic::filters).
  std::string cable;                 // Empty until a cable is known.

  friend bool operator==(const ChainSummary&, const ChainSummary&) = default;
};

// "Mic/Aux › 3-Band EQ › Compressor › CABLE In 16ch", or in the short form
// the tray menu and tooltip use, "Mic/Aux › 2 filters › CABLE In 16ch".
std::string FormatChain(const ChainSummary& chain, bool short_form);

// OBS open in another Windows session (Snapshot::other_obs).
struct OtherObs {
  uint32_t session = 0;  // The session's ID.
  // The name of the account signed in to it, or "" where it can't be read.
  std::string account;
  // That account is this one, signed in a second time, as Remote Desktop
  // allows on a server.
  bool yours = false;

  friend bool operator==(const OtherObs&, const OtherObs&) = default;
};

// Where else OBS is open (Snapshot::other_obs): "Alex's account", "another
// Windows account", "2 other Windows accounts", or "your other session".
// An account signed in twice counts once.
std::string DescribeOtherObs(const std::vector<OtherObs>& others);

struct Snapshot {
  State state = State::kStarting;
  SetupNeed setup = SetupNeed::kNone;      // With kNeedsSetup.
  RestartNeed restart = RestartNeed::kNone;  // With kRestartNeeded.
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
  // OBS running in other Windows sessions, by session. It doesn't pause
  // knobs, but it can send audio to the same cable, which apps here would
  // hear mixed with this mic.
  std::vector<OtherObs> other_obs;
  // Paused from the tray, even while a state above kPausedByUser shows.
  bool paused_by_user = false;
  // The OBS profile's monitoring device, once a profile has been read. Its
  // ID may be "default".
  audio::AudioDevice obs_cable;
  // The playback devices that are connected, to pick a cable from.
  std::vector<audio::AudioDevice> outputs;
  // The recording devices that are connected, and the default
  // communications device's ID, which a mic set to "default" records from
  // (M1 findings).
  std::vector<audio::AudioDevice> inputs;
  std::string default_input;
  // The OBS install, once found, even if its version isn't supported.
  runtime::ObsInstall obs;
  // Whether OBS keeps its settings in the folder knobs reads
  // (import::ObsConfigRoot::obs_writes_here), so that opening it once fills
  // that folder: OBS's own, picked or not, and not one picked elsewhere.
  bool obs_writes_config = false;
  // The settings this snapshot was worked out with. They lag behind new
  // ones for as long as the core takes to apply them.
  Settings settings;

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
