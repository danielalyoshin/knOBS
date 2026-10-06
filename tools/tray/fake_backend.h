// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/backend.h"
#include "core/obs_backend.h"

// A made-up OBS, mic and audio devices for the always-on core, so knobs-tray
// can run the real core (core::Core and its Controller) into every state
// without OBS or an audio device.
namespace knobs::tools {

struct FakeWorld {
  core::ObsCheck obs;
  // CheckObs's answer from its second call on: an OBS updated while knobs
  // runs.
  std::optional<core::ObsCheck> obs_later;
  std::optional<std::string> config_error;
  import::ActiveObsConfig config;
  std::vector<import::ImportedMic> collection;
  audio::Endpoints devices;
  // Virtual cables to plug in when none is connected (ToggleCables).
  std::vector<audio::AudioDevice> spare_cables;
  // The devices' mics and default_mic, while they're unplugged (ToggleMics).
  std::optional<audio::Endpoints> spare_mics;
  std::optional<std::string> chain_error;
  // The running chain gets no audio, as when win-wasapi stops capturing.
  bool stalled = false;
};

// The world, changed by knobs-tray's keys while the core's thread reads it.
class SharedWorld {
 public:
  explicit SharedWorld(FakeWorld world) : world_(std::move(world)) {}
  FakeWorld Get() const {
    std::lock_guard lock(mutex_);
    return world_;
  }
  void Change(const std::function<void(FakeWorld& world)>& change) {
    std::lock_guard lock(mutex_);
    change(world_);
  }

 private:
  mutable std::mutex mutex_;
  FakeWorld world_;
};

// What knobs-tray does once the core has settled, to reach a state.
struct FakeScript {
  bool no_core = false;  // Stay starting.
  bool obs_running = false;
  bool pause = false;
  bool reimport = false;
};

struct FakeScenario {
  std::string_view name;
  std::string description;
  void (*setup)(FakeWorld& world, FakeScript& script);
};

// Running: Mic/Aux (3-Band EQ, Expander, Compressor, Limiter) into CABLE In
// 16ch, with speakers, headphones and two cables connected.
FakeWorld DefaultWorld();
// The devices of tests/fixtures/obs-config, connected: its two mics and the
// cable its profile monitors to.
FakeWorld FixtureWorld();
const std::vector<FakeScenario>& FakeScenarios();

// Unplugs every virtual cable, keeping them as spares, or plugs the spares
// in when none is connected.
void ToggleCables(FakeWorld& world);
// Gives every mic without filters Noise Suppression, Noise Gate, Compressor
// and Limiter, as someone following the second door's steps might in OBS.
// The core sees them at its next import, such as when OBS closes.
void AddFilters(FakeWorld& world);
// Unplugs the recording devices, keeping them as spares, or plugs them back
// in.
void ToggleMics(FakeWorld& world);
// Adds Noise Suppression to the front of the first mic's chain in OBS, or
// takes it out again. Seen at the next import.
void EditChain(FakeWorld& world);
// Adds a VST filter to the first mic in OBS, which knobs leaves out with a
// warning, or takes it out again. Seen at the next import.
void ToggleVstFilter(FakeWorld& world);
// Updates OBS: from 32.2.2 to 32.2.3, then to 33.0.0, which knobs doesn't
// support. Seen at the next import. Returns the new version.
std::string UpdateObs(FakeWorld& world);

class FakeBackend : public core::Backend {
 public:
  FakeBackend(std::shared_ptr<SharedWorld> world, std::function<void(std::string_view line)> log)
      : world_(std::move(world)), log_(std::move(log)) {}

  core::ObsCheck CheckObs(const core::Settings& settings) override;
  Result<import::ActiveObsConfig> ReadObsConfig(const core::Settings& settings,
                                                const runtime::ObsInstall& install) override;
  Status StartLibobs(const runtime::ObsInstall& install, const import::ProfileAudio& audio) override;
  bool LibobsCanRetry() override { return true; }
  Result<core::MicImport> ImportMic(const import::ActiveObsConfig& config, std::string_view pick) override;
  audio::Endpoints ListDevices() override { return world_->Get().devices; }
  Status StartChain(const core::ChainPlan& plan) override;
  void StopChain() override {}
  // Flowing unless stalled: 10 ms packets, a second's worth each time it's
  // asked. Counted from the chain's start, as the real backend counts.
  uint64_t ChainPackets() override { return world_->Get().stalled ? packets_ : packets_ += 100; }
  void Log(std::string_view line) override { log_(line); }

 private:
  std::shared_ptr<SharedWorld> world_;
  std::function<void(std::string_view)> log_;
  int checks_ = 0;
  uint64_t packets_ = 0;
};

// The installed OBS and an OBS settings folder, such as the made-up one in
// tests/fixtures/obs-config, imported by libobs as knobs imports them, with
// the fake world's audio devices. The chain isn't loaded, so no audio device
// is opened.
class FixtureBackend : public core::ObsBackend {
 public:
  FixtureBackend(core::ObsBackendOptions options, std::shared_ptr<SharedWorld> world,
                 std::function<void(std::string_view line)> log)
      : ObsBackend(std::move(options)), world_(std::move(world)), log_(std::move(log)) {}

  audio::Endpoints ListDevices() override { return world_->Get().devices; }
  Status StartChain(const core::ChainPlan& plan) override;
  void StopChain() override {}
  uint64_t ChainPackets() override { return world_->Get().stalled ? packets_ : packets_ += 100; }
  void Log(std::string_view line) override {
    log_(line);
    ObsBackend::Log(line);
  }

 private:
  std::shared_ptr<SharedWorld> world_;
  std::function<void(std::string_view)> log_;
  uint64_t packets_ = 0;
};

}  // namespace knobs::tools
