// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/fake_backend.h"

#include <format>
#include <utility>

#include "app_info.h"
#include "runtime/obs_version.h"

namespace knobs::tools {
namespace {

constexpr char kInterfaceId[] = "{0.0.1.00000000}.{6a0e2a51-audient-id4}";
constexpr char kSpeakersId[] = "{0.0.0.00000000}.{speakers}";
constexpr char kHeadphonesId[] = "{0.0.0.00000000}.{audient-id4-out}";
constexpr char kCableInputId[] = "{0.0.0.00000000}.{vb-cable-input}";
constexpr char kCable16Id[] = "{0.0.0.00000000}.{vb-cable-16ch}";
constexpr char kCable16Name[] = "CABLE In 16ch (VB-Audio Virtual Cable)";

import::ImportedMic MakeMic(std::string name, std::string location, std::vector<std::string> filters) {
  import::ImportedMic mic;
  mic.mic.name = std::move(name);
  mic.mic.location = std::move(location);
  mic.mic.origin = import::MicOrigin::kGlobalDevice;
  mic.mic.key = mic.mic.location;
  mic.mic.device_id = kInterfaceId;
  mic.mic.monitored = true;
  mic.filters = std::move(filters);
  mic.chain_key = mic.mic.name;
  for (const std::string& filter : mic.filters) mic.chain_key += "|" + filter;
  mic.source_json = "{}";
  return mic;
}

void RemoveOutput(FakeWorld& world, std::string_view id) {
  std::erase_if(world.devices.outputs, [id](const audio::AudioDevice& device) { return device.id == id; });
}

}  // namespace

FakeWorld DefaultWorld() {
  FakeWorld world;
  world.obs.found = core::ObsFound::kYes;
  world.obs.install = {"C:\\Program Files\\obs-studio", {32, 2, 2}};
  world.config.profile = "Untitled";
  world.config.collection = "Untitled";
  world.config.audio.monitoring_device_id = kCable16Id;
  world.config.audio.monitoring_device_name = kCable16Name;
  world.collection = {MakeMic("Mic/Aux", "AuxAudioDevice1", {"3-Band EQ", "Expander", "Compressor", "Limiter"})};
  world.devices.mics = {{"Microphone (Audient iD4)", kInterfaceId}};
  world.devices.default_mic = kInterfaceId;
  world.devices.outputs = {{"Speakers (Realtek(R) Audio)", kSpeakersId},
                           {"Headphones (Audient iD4)", kHeadphonesId},
                           {"CABLE Input (VB-Audio Virtual Cable)", kCableInputId},
                           {kCable16Name, kCable16Id}};
  return world;
}

const std::vector<FakeScenario>& FakeScenarios() {
  static const std::vector<FakeScenario> scenarios = {
      {"running", "Mic/Aux with 4 filters into CABLE In 16ch", [](FakeWorld&, FakeScript&) {}},
      {"paused", "running, then paused from the menu", [](FakeWorld&, FakeScript& script) { script.pause = true; }},
      {"obs-open", "OBS is open, so knobs pauses", [](FakeWorld&, FakeScript& script) { script.obs_running = true; }},
      {"mic-missing", "the mic's recording device isn't connected",
       [](FakeWorld& world, FakeScript&) {
         world.devices.mics.clear();
         world.devices.default_mic.clear();
       }},
      {"cable-missing", "CABLE In 16ch isn't connected",
       [](FakeWorld& world, FakeScript&) { RemoveOutput(world, kCable16Id); }},
      {"setup-obs", "OBS's settings can't be read",
       [](FakeWorld& world, FakeScript&) {
         world.config_error = "OBS's settings weren't found in C:\\Users\\you\\AppData\\Roaming\\obs-studio. Run OBS "
                              "once, or choose its settings folder.";
       }},
      {"setup-no-mic", "the scene collection has no mic",
       [](FakeWorld& world, FakeScript&) { world.collection.clear(); }},
      {"setup-pick-mic", "the scene collection has three mics, and none is picked",
       [](FakeWorld& world, FakeScript&) {
         world.collection.push_back(MakeMic("Mic/Aux 2", "AuxAudioDevice2", {"Noise Suppression"}));
         world.collection.push_back(MakeMic("Podcast Mic", "sources[4]", {"Noise Gate", "Compressor"}));
       }},
      {"setup-cable", "the OBS profile monitors to the default device",
       [](FakeWorld& world, FakeScript&) {
         world.config.audio.monitoring_device_id = "default";
         world.config.audio.monitoring_device_name.clear();
       }},
      {"obs-missing", "OBS isn't installed",
       [](FakeWorld& world, FakeScript&) {
         world.obs = {core::ObsFound::kMissing, {},
                      std::format("OBS Studio isn't installed. {} needs {}.", kDisplayName,
                                  runtime::DescribeSupportedObsVersions())};
       }},
      {"obs-unsupported", "OBS 33.0.0 is installed",
       [](FakeWorld& world, FakeScript&) {
         world.obs = {core::ObsFound::kUnsupported, {"C:\\Program Files\\obs-studio", {33, 0, 0}},
                      runtime::UnsupportedObsMessage("33.0.0")};
       }},
      {"restart", "OBS was updated to 32.2.3 while knobs ran",
       [](FakeWorld& world, FakeScript& script) {
         world.obs_later = world.obs;
         world.obs_later->install.version = {32, 2, 3};
         script.reimport = true;
       }},
      {"failed", "the chain fails to start",
       [](FakeWorld& world, FakeScript&) {
         world.chain_error = "libobs couldn't load the mic's source.";
       }},
      {"starting", "the core hasn't reported yet", [](FakeWorld&, FakeScript& script) { script.no_core = true; }},
  };
  return scenarios;
}

core::ObsCheck FakeBackend::CheckObs(const core::Settings&) {
  ++checks_;
  if (checks_ > 1 && world_.obs_later) return *world_.obs_later;
  return world_.obs;
}

Result<import::ActiveObsConfig> FakeBackend::ReadObsConfig(const core::Settings&, const runtime::ObsInstall&) {
  if (world_.config_error) return Error{*world_.config_error};
  return world_.config;
}

Status FakeBackend::StartLibobs(const runtime::ObsInstall& install, const import::ProfileAudio& audio) {
  Log(std::format("libobs {} would start at {} Hz {}.", install.version.ToString(), audio.sample_rate,
                  audio.channel_setup));
  return Ok{};
}

Result<core::MicImport> FakeBackend::ImportMic(const import::ActiveObsConfig&, std::string_view pick) {
  core::MicImport result;
  for (const import::ImportedMic& mic : world_.collection) result.mics.push_back(mic.mic);
  auto picked = import::PickMic(result.mics, pick);
  if (!picked) {
    result.pick_error = picked.error();
    return result;
  }
  result.picked = *picked;
  result.mic = world_.collection[*picked];
  return result;
}

Status FakeBackend::StartChain(const core::ChainPlan& plan) {
  if (world_.chain_error) return Error{*world_.chain_error};
  Log(std::format("The chain would run into \"{}\".", plan.cable.name));
  return Ok{};
}

}  // namespace knobs::tools
