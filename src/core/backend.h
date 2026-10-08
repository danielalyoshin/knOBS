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
#include "core/state.h"
#include "import/mic_import.h"
#include "import/obs_config.h"
#include "runtime/obs_install.h"
#include "util/result.h"

namespace knobs::core {

enum class ObsFound { kYes, kMissing, kUnsupported };

struct ObsCheck {
  ObsFound found = ObsFound::kMissing;
  runtime::ObsInstall install;  // Unless missing.
  std::string message;          // Why not, unless found.
  // The settings folder to read for that install, unless missing: the one
  // picked (Settings::obs_config), else where that OBS keeps them
  // (import::ObsConfigRootFor).
  Result<import::ObsConfigRoot> config = Error{};
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

  // The same chain to the same cable (audio::SameId). The rest of
  // source_json, and the cable's name, don't change what reaches the cable.
  bool SameAs(const ChainPlan& other) const {
    return chain_key == other.chain_key && load_callbacks == other.load_callbacks &&
           audio::SameId(cable.id, other.cable.id);
  }
};

// How loud a running chain's output has been since the last look, from the
// peak of each packet it filtered (10 ms each from win-wasapi), as the monitor
// plays it: after the source's volume. 1 is full scale.
struct ChainLevel {
  // What the packets stayed at or below, leaving out the loudest tenth of
  // them, so a click or a short breath doesn't count.
  float typical = 0;
  // The loudest of the last few packets: what's playing now.
  float latest = 0;
};

// Everything the Controller needs from the world: OBS's install and
// settings, libobs, and the audio devices. Unit tests fake it. Every call,
// destruction included, comes from one thread, and libobs runs on it.
class Backend {
 public:
  virtual ~Backend() = default;

  // Finds the OBS install and its settings folder, and checks its version.
  virtual ObsCheck CheckObs(const Settings& settings) = 0;
  // The active OBS profile and scene collection in `root` (ObsCheck::config).
  virtual Result<import::ActiveObsConfig> ReadObsConfig(const import::ObsConfigRoot& root) = 0;
  // Starts libobs from `install`'s runtime copy, at the profile's sample rate
  // and channels. Once, before anything below. After a failure, again only
  // if LibobsCanRetry says so.
  virtual Status StartLibobs(const runtime::ObsInstall& install, const import::ProfileAudio& audio) = 0;
  // After StartLibobs failed: whether it can run again in this process. Not
  // once libobs had opened a module: libobs never unloads module DLLs, and
  // they keep obs.dll and its state loaded (runtime::ObsRuntime).
  virtual bool LibobsCanRetry() = 0;
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
  // How loud the running chain's output has been since the last call, or
  // since it started.
  virtual ChainLevel TakeChainLevel() = 0;
  // Opens the running chain's monitor stream afresh, dropping the audio it had
  // queued, so the delay to the cable starts again from the least. The chain
  // keeps running; the packets that come meanwhile are lost (docs/design.md,
  // Keeping the delay down).
  virtual void RestartMonitor() = 0;
  // A line for knobs's log, once there is one.
  virtual void Log(std::string_view line) = 0;
};

}  // namespace knobs::core
