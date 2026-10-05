// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/backend.h"

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
  std::optional<std::string> chain_error;
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
  std::string_view description;
  void (*setup)(FakeWorld& world, FakeScript& script);
};

// Running: Mic/Aux (3-Band EQ, Expander, Compressor, Limiter) into CABLE In
// 16ch, with speakers, headphones and two cables connected.
FakeWorld DefaultWorld();
const std::vector<FakeScenario>& FakeScenarios();

class FakeBackend : public core::Backend {
 public:
  FakeBackend(FakeWorld world, std::function<void(std::string_view line)> log)
      : world_(std::move(world)), log_(std::move(log)) {}

  core::ObsCheck CheckObs(const core::Settings& settings) override;
  Result<import::ActiveObsConfig> ReadObsConfig(const core::Settings& settings,
                                                const runtime::ObsInstall& install) override;
  Status StartLibobs(const runtime::ObsInstall& install, const import::ProfileAudio& audio) override;
  Result<core::MicImport> ImportMic(const import::ActiveObsConfig& config, std::string_view pick) override;
  audio::Endpoints ListDevices() override { return world_.devices; }
  Status StartChain(const core::ChainPlan& plan) override;
  void StopChain() override {}
  // Always flowing: 10 ms packets, a second's worth each time it's asked.
  uint64_t ChainPackets() override { return packets_ += 100; }
  void Log(std::string_view line) override { log_(line); }

 private:
  FakeWorld world_;
  std::function<void(std::string_view)> log_;
  int checks_ = 0;
  uint64_t packets_ = 0;
};

}  // namespace knobs::tools
