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
#include "audio/device_watch.h"
#include "import/mic_import.h"
#include "import/obs_config.h"
#include "runtime/obs_install.h"
#include "util/result.h"

namespace knobs::core {

// knobs's own choices. They override what it imports, and survive
// re-imports.
struct Settings {
  // The OBS install. Default: found as runtime::FindObsInstall finds it.
  std::optional<std::filesystem::path> obs_dir;
  // OBS's settings folder, the one holding obs-studio\. Default: where OBS
  // keeps it (import::FindObsConfigRoot).
  std::optional<std::filesystem::path> obs_config;
  // Which mic, as import::PickMic takes it. Empty picks the only one.
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

enum class ObsFound { kYes, kMissing, kUnsupported };

struct ObsCheck {
  ObsFound found = ObsFound::kMissing;
  runtime::ObsInstall install;  // Unless missing.
  std::string message;          // Why not, unless found.
};

struct MicImport {
  // Every mic in the scene collection.
  std::vector<import::MicCandidate> mics;
  // The picked one, ready to load. Empty when the pick (Settings::mic)
  // matches none; pick_error says why.
  std::optional<size_t> picked;
  std::optional<import::ImportedMic> mic;
  std::string pick_error;
};

// What the chain runs: a mic's source object, loaded by libobs's own loader
// and monitored to a cable.
struct ChainPlan {
  std::string source_json;  // import::ImportedMic::source_json
  std::string chain_key;    // import::ImportedMic::chain_key
  bool load_callbacks = false;
  audio::AudioDevice cable;

  // The same chain to the same cable. The rest of source_json, and the
  // cable's name, don't change what reaches the cable.
  bool SameAs(const ChainPlan& other) const {
    return chain_key == other.chain_key && load_callbacks == other.load_callbacks && cable.id == other.cable.id;
  }
};

// Everything the Controller needs from the world: OBS's install and
// settings, libobs, and the audio devices. Unit tests fake it. Every call,
// destruction included, comes from one thread, and libobs runs on it.
class Backend {
 public:
  virtual ~Backend() = default;

  // Finds the OBS install and checks its version.
  virtual ObsCheck CheckObs(const Settings& settings) = 0;
  // The active OBS profile and scene collection.
  virtual Result<import::ActiveObsConfig> ReadObsConfig(const Settings& settings,
                                                        const runtime::ObsInstall& install) = 0;
  // Starts libobs from `install`'s runtime copy, at the profile's sample rate
  // and channels. Once, before anything below.
  virtual Status StartLibobs(const runtime::ObsInstall& install, const import::ProfileAudio& audio) = 0;
  // Reads the scene collection, picks the mic and runs the pre-flight checks.
  virtual Result<MicImport> ImportMic(const import::ActiveObsConfig& config, std::string_view pick) = 0;
  // Doesn't need libobs.
  virtual audio::Endpoints ListDevices() = 0;
  // Loads the chain and monitors it to its cable. Only one chain runs at a
  // time: the Controller stops one before starting the next.
  virtual Status StartChain(const ChainPlan& plan) = 0;
  virtual void StopChain() = 0;
  // Audio packets the running chain has filtered since it started. Silence
  // counts: win-wasapi passes silent packets on as zeros.
  virtual uint64_t ChainPackets() = 0;
  // A line for knobs's log, once there is one.
  virtual void Log(std::string_view line) = 0;
};

}  // namespace knobs::core
