// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/fake_backend.h"

#include <format>
#include <utility>

#include "app_info.h"
#include "runtime/obs_version.h"
#include "tray/menu.h"

namespace knobs::tools {
namespace {

constexpr char kInterfaceId[] = "{0.0.1.00000000}.{6a0e2a51-audient-id4}";
constexpr char kSpeakersId[] = "{0.0.0.00000000}.{speakers}";
constexpr char kHeadphonesId[] = "{0.0.0.00000000}.{audient-id4-out}";
constexpr char kHeadphonesName[] = "Headphones (Audient iD4)";
constexpr char kCableInputId[] = "{0.0.0.00000000}.{vb-cable-input}";
constexpr char kCableInputName[] = "CABLE Input (VB-Audio Virtual Cable)";
constexpr char kCable16Id[] = "{0.0.0.00000000}.{vb-cable-16ch}";
constexpr char kCable16Name[] = "CABLE In 16ch (VB-Audio Virtual Cable)";

// tests/fixtures/obs-config's devices.
constexpr char kFixtureMicId[] = "{0.0.1.00000000}.{00000000-0000-0000-0000-0000000000a1}";
constexpr char kFixturePodcastMicId[] = "{0.0.1.00000000}.{00000000-0000-0000-0000-0000000000a2}";
constexpr char kFixtureCableId[] = "{0.0.0.00000000}.{00000000-0000-0000-0000-00000000cab1}";

const std::vector<std::string> kStepsFilters = {"Noise Suppression", "Noise Gate", "Compressor", "Limiter"};
constexpr char kEditedFilter[] = "Noise Suppression";

void SetFilters(import::ImportedMic& mic, std::vector<std::string> filters) {
  mic.filters = std::move(filters);
  mic.mic.filters = mic.filters;
  mic.chain_key = mic.mic.name;
  for (const std::string& filter : mic.filters) mic.chain_key += "|" + filter;
}

import::ImportedMic MakeMic(std::string name, std::string location, std::vector<std::string> filters) {
  import::ImportedMic mic;
  mic.mic.name = std::move(name);
  mic.mic.location = std::move(location);
  mic.mic.origin = import::MicOrigin::kGlobalDevice;
  mic.mic.key = mic.mic.location;
  mic.mic.device_id = kInterfaceId;
  mic.mic.monitored = true;
  mic.source_json = "{}";
  SetFilters(mic, std::move(filters));
  return mic;
}

void RemoveOutput(FakeWorld& world, std::string_view id) {
  std::erase_if(world.devices.outputs, [id](const audio::AudioDevice& device) { return device.id == id; });
}

// The OBS profile monitors to its default device, usually speakers.
void MonitorToDefault(FakeWorld& world) {
  world.config.audio.monitoring_device_id = "default";
  world.config.audio.monitoring_device_name.clear();
}

import::ImportNote VstWarning() {
  return {true, std::format("Filter \"ReaComp\" is a VST plugin (reacomp-standalone.dll). {} can't run VST "
                            "plugins yet, so it leaves the filter out.",
                            kDisplayName)};
}

// No virtual cable is installed. VB-Cable's input is the spare.
void NoCable(FakeWorld& world) {
  world.devices.outputs = {{"Speakers (Realtek(R) Audio)", kSpeakersId}, {kHeadphonesName, kHeadphonesId}};
  world.spare_cables = {{kCableInputName, kCableInputId}};
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
                           {kHeadphonesName, kHeadphonesId},
                           {kCableInputName, kCableInputId},
                           {kCable16Name, kCable16Id}};
  return world;
}

FakeWorld FixtureWorld() {
  FakeWorld world = DefaultWorld();
  world.devices.mics = {{"Microphone (Audient iD4)", kFixtureMicId}, {"Microphone (Shure MV7)", kFixturePodcastMicId}};
  world.devices.default_mic = kFixtureMicId;
  world.devices.outputs = {{"Speakers (Realtek(R) Audio)", kSpeakersId},
                           {kHeadphonesName, kHeadphonesId},
                           {kCableInputName, kFixtureCableId},
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
         world.config_error =
             "OBS has no settings in C:\\Users\\you\\AppData\\Roaming\\obs-studio yet. Open OBS once and close it.";
       }},
      {"setup-no-mic", "the scene collection has no mic",
       [](FakeWorld& world, FakeScript&) { world.collection.clear(); }},
      {"setup-pick-mic", "the scene collection has three mics, and none is picked",
       [](FakeWorld& world, FakeScript&) {
         world.collection.push_back(MakeMic("Mic/Aux 2", "AuxAudioDevice2", {"Noise Suppression"}));
         world.collection.push_back(MakeMic("Podcast Mic", "sources[4]", {"Noise Gate", "Compressor"}));
       }},
      {"setup-cable", "the OBS profile monitors to the default device",
       [](FakeWorld& world, FakeScript&) { MonitorToDefault(world); }},
      {"no-filters", "Mic/Aux has no filters",
       [](FakeWorld& world, FakeScript&) { SetFilters(world.collection.front(), {}); }},
      {"no-cable", "no virtual cable is installed, and OBS monitors to the default device",
       [](FakeWorld& world, FakeScript&) {
         MonitorToDefault(world);
         NoCable(world);
       }},
      {"not-a-cable", "the OBS profile monitors to headphones",
       [](FakeWorld& world, FakeScript&) {
         world.config.audio.monitoring_device_id = kHeadphonesId;
         world.config.audio.monitoring_device_name = kHeadphonesName;
       }},
      {"warnings", "Mic/Aux has a VST filter and push-to-talk",
       [](FakeWorld& world, FakeScript&) {
         world.collection.front().notes = {
             VstWarning(),
             {false, "Filter \"Expander\" is off in OBS, and stays off."},
             {false,
              std::format("The mic is on push-to-talk in OBS. libobs's monitor ignores mute, push-to-talk and "
                          "push-to-mute (OBS 32.2 and later), so the cable gets the mic either way, from OBS and "
                          "from {}.",
                          kDisplayName),
              true}};
       }},
      {"new-user", "OBS opened once: Mic/Aux on the default device with no filters, no cable",
       [](FakeWorld& world, FakeScript&) {
         world.collection.front().mic.device_id = "default";
         SetFilters(world.collection.front(), {});
         MonitorToDefault(world);
         NoCable(world);
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
      {"restart", "OBS was updated to 32.2.3 while knobs ran, so it restarts",
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

void ToggleCables(FakeWorld& world) {
  std::vector<audio::AudioDevice> cables;
  std::erase_if(world.devices.outputs, [&cables](const audio::AudioDevice& device) {
    if (!tray::IsVirtualCable(device.name)) return false;
    cables.push_back(device);
    return true;
  });
  if (cables.empty()) {
    world.devices.outputs.insert(world.devices.outputs.end(), world.spare_cables.begin(), world.spare_cables.end());
  } else {
    world.spare_cables = std::move(cables);
  }
}

void AddFilters(FakeWorld& world) {
  for (import::ImportedMic& mic : world.collection) {
    if (mic.filters.empty()) SetFilters(mic, kStepsFilters);
  }
}

void ToggleMics(FakeWorld& world) {
  if (world.spare_mics) {
    world.devices.mics = std::move(world.spare_mics->mics);
    world.devices.default_mic = std::move(world.spare_mics->default_mic);
    world.spare_mics.reset();
    return;
  }
  world.spare_mics = world.devices;
  world.devices.mics.clear();
  world.devices.default_mic.clear();
}

void EditChain(FakeWorld& world) {
  if (world.collection.empty()) return;
  import::ImportedMic& mic = world.collection.front();
  std::vector<std::string> filters = mic.filters;
  if (!filters.empty() && filters.front() == kEditedFilter) {
    filters.erase(filters.begin());
  } else {
    filters.insert(filters.begin(), kEditedFilter);
  }
  SetFilters(mic, std::move(filters));
}

void ToggleVstFilter(FakeWorld& world) {
  if (world.collection.empty()) return;
  std::vector<import::ImportNote>& notes = world.collection.front().notes;
  // Left out of the chain, so the chain's key stays the same.
  const import::ImportNote warning = VstWarning();
  if (std::erase(notes, warning) == 0) notes.insert(notes.begin(), warning);
}

std::string UpdateObs(FakeWorld& world) {
  world.obs_later.reset();
  if (world.obs.install.version < runtime::ObsVersion{32, 2, 3}) {
    world.obs.install.version = {32, 2, 3};
  } else {
    world.obs = {core::ObsFound::kUnsupported, {world.obs.install.root, {33, 0, 0}},
                 runtime::UnsupportedObsMessage("33.0.0")};
  }
  return world.obs.install.version.ToString();
}

core::ObsCheck FakeBackend::CheckObs(const core::Settings&) {
  ++checks_;
  const FakeWorld world = world_->Get();
  if (checks_ > 1 && world.obs_later) return *world.obs_later;
  return world.obs;
}

Result<import::ActiveObsConfig> FakeBackend::ReadObsConfig(const core::Settings&, const runtime::ObsInstall&) {
  const FakeWorld world = world_->Get();
  if (world.config_error) return Error{*world.config_error};
  return world.config;
}

Status FakeBackend::StartLibobs(const runtime::ObsInstall& install, const import::ProfileAudio& audio) {
  Log(std::format("libobs {} would start at {} Hz {}.", install.version.ToString(), audio.sample_rate,
                  audio.channel_setup));
  return Ok{};
}

Result<core::MicImport> FakeBackend::ImportMic(const import::ActiveObsConfig&, std::string_view pick) {
  const FakeWorld world = world_->Get();
  core::MicImport result;
  for (const import::ImportedMic& mic : world.collection) result.mics.push_back(mic.mic);
  auto picked = import::PickMic(result.mics, pick);
  if (!picked) {
    result.pick_error = picked.error();
    return result;
  }
  result.picked = *picked;
  result.mic = world.collection[*picked];
  return result;
}

Status FakeBackend::StartChain(const core::ChainPlan& plan) {
  if (const FakeWorld world = world_->Get(); world.chain_error) return Error{*world.chain_error};
  Log(std::format("The chain would run into \"{}\".", plan.cable.name));
  packets_ = 0;
  return Ok{};
}

Status FixtureBackend::StartChain(const core::ChainPlan& plan) {
  if (const FakeWorld world = world_->Get(); world.chain_error) return Error{*world.chain_error};
  Log(std::format("The chain would run into \"{}\".", plan.cable.name));
  packets_ = 0;
  return Ok{};
}

}  // namespace knobs::tools
