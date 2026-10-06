// SPDX-License-Identifier: GPL-2.0-or-later
//
// Unit tests for the always-on core: its decisions (core::Controller) and its
// thread (core::Core), against a fake backend. No OBS, no libobs and no audio
// devices.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/backend.h"
#include "core/controller.h"
#include "core/core.h"
#include "core/obs_backend.h"
#include "core/obs_process.h"
#include "core/state.h"
#include "runtime/obs_layout.h"
#include "test_harness.h"
#include "util/win_strings.h"

namespace {

using namespace knobs;
using namespace knobs::core;
using namespace std::chrono_literals;
using Clock = Controller::Clock;
namespace fs = std::filesystem;

constexpr char kMicId[] = "{0.0.1.00000000}.{mic}";
constexpr char kHeadsetId[] = "{0.0.1.00000000}.{headset}";
constexpr char kCableId[] = "{0.0.0.00000000}.{cable}";
constexpr char kCableName[] = "CABLE In 16ch (VB-Audio Virtual Cable)";
constexpr char kOtherCableId[] = "{0.0.0.00000000}.{cable-a}";

import::ImportedMic MakeMic(std::string name, std::string device_id, std::string key,
                            std::vector<std::string> filters = {"Noise Suppression", "Compressor"}) {
  import::ImportedMic mic;
  mic.mic.name = std::move(name);
  mic.mic.location = "AuxAudioDevice1";
  mic.mic.origin = import::MicOrigin::kGlobalDevice;
  mic.mic.device_id = std::move(device_id);
  mic.mic.monitored = true;
  mic.source_json = "{\"key\": \"" + key + "\"}";
  mic.chain_key = std::move(key);
  mic.filters = std::move(filters);
  return mic;
}

import::ActiveObsConfig DefaultConfig() {
  import::ActiveObsConfig config;
  config.profile = "Untitled";
  config.collection = "Untitled";
  config.audio.monitoring_device_id = kCableId;
  config.audio.monitoring_device_name = kCableName;
  return config;
}

audio::Endpoints DefaultDevices() {
  audio::Endpoints devices;
  devices.mics = {{"Microphone (Audient iD4)", kMicId}};
  devices.outputs = {{"Speakers (Realtek)", "{0.0.0.00000000}.{speakers}"}, {kCableName, kCableId}};
  devices.default_mic = kMicId;
  return devices;
}

// Answers from its public fields, which tests change between events, and
// counts what was asked of it.
struct FakeBackend : Backend {
  ObsCheck obs{ObsFound::kYes, {"C:\\obs-studio", {32, 2, 2}}, "",
               import::ObsConfigRoot{"C:\\Users\\you\\AppData\\Roaming", false, true}};
  std::optional<std::string> config_error;
  import::ActiveObsConfig config = DefaultConfig();
  std::optional<std::string> libobs_error;
  // Whether libobs can start again after libobs_error. It can't once the
  // failure left a module loaded.
  bool libobs_can_retry = true;
  std::optional<std::string> import_error;
  // The scene collection's mics. The only one is picked without a pick;
  // otherwise the pick names one.
  std::vector<import::ImportedMic> collection = {MakeMic("Mic/Aux", kMicId, "chain-1")};
  audio::Endpoints devices = DefaultDevices();
  std::optional<std::string> chain_error;
  // Whether the running chain gets audio.
  bool flowing = true;

  int libobs_starts = 0;
  import::ProfileAudio libobs_audio;
  int imports = 0;
  int device_lists = 0;
  int chain_starts = 0;
  int chain_stops = 0;
  bool chain_running = false;
  std::optional<ChainPlan> last_plan;
  uint64_t packets = 0;

  std::function<void()> on_call;
  std::function<void()> on_start_libobs;
  std::function<void(bool chain_running)> on_destroy;

  ~FakeBackend() override {
    if (on_destroy) on_destroy(chain_running);
  }

  ObsCheck CheckObs(const Settings&) override {
    Called();
    return obs;
  }
  Result<import::ActiveObsConfig> ReadObsConfig(const import::ObsConfigRoot&) override {
    Called();
    if (config_error) return Error{*config_error};
    return config;
  }
  Status StartLibobs(const runtime::ObsInstall&, const import::ProfileAudio& audio) override {
    Called();
    if (on_start_libobs) on_start_libobs();
    ++libobs_starts;
    libobs_audio = audio;
    if (libobs_error) return Error{*libobs_error};
    return Ok{};
  }
  bool LibobsCanRetry() override {
    Called();
    return libobs_can_retry;
  }
  Result<MicImport> ImportMic(const import::ActiveObsConfig&, std::string_view pick) override {
    Called();
    ++imports;
    if (import_error) return Error{*import_error};
    MicImport result;
    for (const import::ImportedMic& mic : collection) result.mics.push_back(mic.mic);
    std::optional<size_t> picked;
    if (pick.empty() && collection.size() == 1) picked = 0;
    for (size_t i = 0; i < collection.size(); ++i) {
      if (!pick.empty() && collection[i].mic.name == pick) picked = i;
    }
    if (!picked) {
      result.pick_error = collection.empty() ? "The scene collection has no mic." : "Choose a mic.";
      return result;
    }
    result.picked = picked;
    result.mic = collection[*picked];
    return result;
  }
  audio::Endpoints ListDevices() override {
    Called();
    ++device_lists;
    return devices;
  }
  Status StartChain(const ChainPlan& plan) override {
    Called();
    CHECK(!chain_running);
    if (chain_error) return Error{*chain_error};
    ++chain_starts;
    chain_running = true;
    last_plan = plan;
    packets = 0;
    return Ok{};
  }
  void StopChain() override {
    Called();
    CHECK(chain_running);
    ++chain_stops;
    chain_running = false;
  }
  uint64_t ChainPackets() override {
    Called();
    CHECK(chain_running);
    if (flowing) packets += 100;
    return packets;
  }
  void Log(std::string_view) override { Called(); }

 private:
  void Called() {
    if (on_call) on_call();
  }
};

// A controller on a fake backend and a fake clock.
struct Fixture {
  FakeBackend backend;
  std::vector<Snapshot> published;
  Controller controller;
  Clock::time_point now{};

  explicit Fixture(Settings settings = {})
      : controller(backend, std::move(settings), [this](const Snapshot& snapshot) { published.push_back(snapshot); },
                   {.clock = [this] { return now; }}) {}

  const Snapshot& last() const { return published.back(); }
  State state() const { return published.back().state; }

  // Moves the clock on by `duration`, ticking at whatever falls due on the
  // way, as the core's thread does.
  void Advance(Clock::duration duration) {
    const Clock::time_point end = now + duration;
    for (int ticks = 0; ticks < 10'000; ++ticks) {
      const auto next = controller.NextDeadline();
      if (!next || *next > end) break;
      now = std::max(now, *next);
      controller.Tick(now);
    }
    now = end;
  }

  void DevicesChange(const audio::Endpoints& devices) {
    backend.devices = devices;
    controller.DevicesChanged(now);
    Advance(500ms);
  }
};

audio::Endpoints NoMics() {
  audio::Endpoints devices = DefaultDevices();
  devices.mics.clear();
  devices.default_mic.clear();
  return devices;
}

// --- State and format -------------------------------------------------------------

TEST(ChainFormatsLikeTheTray) {
  ChainSummary chain{"Mic/Aux", {"3-Band EQ", "Expander", "Compressor", "Limiter"}, "CABLE In 16ch"};
  CHECK(FormatChain(chain, false) == "Mic/Aux › 3-Band EQ › Expander › Compressor › Limiter › CABLE In 16ch");
  CHECK(FormatChain(chain, true) == "Mic/Aux › 4 filters › CABLE In 16ch");
  chain.filters = {"Gain"};
  CHECK(FormatChain(chain, true) == "Mic/Aux › 1 filter › CABLE In 16ch");
  chain.filters.clear();
  chain.cable.clear();
  CHECK(FormatChain(chain, true) == "Mic/Aux");

  Snapshot snapshot;
  snapshot.state = State::kPausedForObs;
  snapshot.chain = ChainSummary{"Mic/Aux", {"Gain"}, "CABLE Input"};
  CHECK(DescribeSnapshot(snapshot) == "paused while OBS is open: Mic/Aux › Gain › CABLE Input");
  snapshot.state = State::kMicMissing;
  snapshot.detail = "Gone.";
  CHECK(DescribeSnapshot(snapshot) == "mic missing: Gone.");
}

// --- Controller --------------------------------------------------------------------

TEST(CoreStartsTheImportedChain) {
  Fixture f;
  f.controller.Start(false, f.now);
  CHECK(f.published.size() == 2 && f.published.front().state == State::kStarting);
  CHECK(f.state() == State::kRunning);
  CHECK(f.backend.libobs_starts == 1 && f.backend.libobs_audio.sample_rate == 48000);
  CHECK(f.backend.chain_starts == 1 && f.backend.chain_running);
  CHECK(f.backend.last_plan && f.backend.last_plan->cable.id == kCableId &&
        f.backend.last_plan->cable.name == kCableName && f.backend.last_plan->chain_key == "chain-1");
  const Snapshot& snapshot = f.last();
  CHECK(snapshot.chain &&
        FormatChain(*snapshot.chain, false) == std::string("Mic/Aux › Noise Suppression › Compressor › ") + kCableName);
  CHECK(snapshot.chain_revision == 1 && snapshot.mics.size() == 1 && snapshot.picked_mic == 0u);
  CHECK(snapshot.detail.empty() && !snapshot.obs_running);
}

TEST(CoreReportsAMissingOrUnsupportedObs) {
  {
    Fixture f;
    f.backend.obs = {ObsFound::kMissing, {}, "OBS Studio isn't installed."};
    f.controller.Start(false, f.now);
    CHECK(f.state() == State::kObsMissing && f.last().detail == "OBS Studio isn't installed.");
    CHECK(f.backend.libobs_starts == 0 && f.backend.chain_starts == 0);
  }
  {
    Fixture f;
    f.backend.obs = {ObsFound::kUnsupported, {"C:\\obs-studio", {33, 0, 0}}, "OBS 33.0.0 isn't supported."};
    f.controller.Start(false, f.now);
    CHECK(f.state() == State::kObsUnsupported && f.backend.libobs_starts == 0);
  }
}

TEST(CoreNeedsSetupUntilTheUserChooses) {
  Fixture f;
  f.backend.config_error = "OBS has no settings yet.";
  f.controller.Start(false, f.now);
  CHECK(f.state() == State::kNeedsSetup && f.last().setup == SetupNeed::kObsSettings);
  // libobs runs at the profile's sample rate, so it waits for the profile.
  CHECK(f.backend.libobs_starts == 0);

  f.backend.config_error.reset();
  f.backend.collection.clear();
  f.controller.Reimport(f.now);
  CHECK(f.state() == State::kNeedsSetup && f.last().setup == SetupNeed::kNoMic && f.backend.libobs_starts == 1);

  f.backend.collection = {MakeMic("Mic/Aux", kMicId, "aux"), MakeMic("Podcast Mic", kMicId, "podcast")};
  f.controller.Reimport(f.now);
  CHECK(f.last().setup == SetupNeed::kPickMic && f.last().mics.size() == 2 && !f.last().picked_mic);
  CHECK(!f.last().chain && !f.backend.chain_running);

  f.controller.Apply({.mic = "Podcast Mic"}, f.now);
  CHECK(f.state() == State::kRunning && f.last().setup == SetupNeed::kNone && f.last().picked_mic == 1u &&
        f.last().chain->mic == "Podcast Mic");

  // A profile that monitors to the default device, usually speakers, waits
  // for a cable.
  f.backend.config.audio.monitoring_device_id = "default";
  f.backend.config.audio.monitoring_device_name.clear();
  f.controller.Reimport(f.now);
  CHECK(f.state() == State::kNeedsSetup && f.last().setup == SetupNeed::kCable && !f.backend.chain_running);
  CHECK(f.last().chain && f.last().chain->cable.empty());
  f.controller.Apply({.mic = "Podcast Mic", .cable = kCableId}, f.now);
  CHECK(f.state() == State::kRunning && f.backend.last_plan->cable.id == kCableId);
  CHECK(f.backend.libobs_starts == 1);
}

TEST(CoreSaysWhetherOpeningObsFillsItsSettingsFolder) {
  // OBS's own settings folder: opening OBS once fills it.
  Fixture f;
  f.backend.config_error = "OBS has no settings yet.";
  f.controller.Start(false, f.now);
  CHECK(f.last().setup == SetupNeed::kObsSettings && f.last().obs_writes_config);
  // A folder picked elsewhere, as CheckObs found it.
  f.backend.obs.config = import::ObsConfigRoot{"E:\\Portable\\config", true};
  f.controller.Reimport(f.now);
  CHECK(f.last().setup == SetupNeed::kObsSettings && !f.last().obs_writes_config);
  // No settings folder at all: OBS's settings can't be read either.
  f.backend.config_error.reset();
  f.backend.obs.config = Error{"Couldn't find the %AppData% folder."};
  f.controller.Reimport(f.now);
  CHECK(f.state() == State::kNeedsSetup && f.last().setup == SetupNeed::kObsSettings);
  CHECK(f.last().detail == "Couldn't find the %AppData% folder." && f.backend.libobs_starts == 0);
}

TEST(CorePausesWhileObsRunsAndReimportsWhenItExits) {
  Fixture f;
  f.controller.Start(true, f.now);
  CHECK(f.state() == State::kPausedForObs && f.last().obs_running);
  CHECK(f.backend.chain_starts == 0 && f.backend.imports == 1 && f.last().chain);

  f.controller.SetObsRunning(false, f.now);
  CHECK(f.state() == State::kRunning && !f.last().obs_running);
  CHECK(f.backend.imports == 2 && f.backend.chain_starts == 1);

  f.controller.SetObsRunning(true, f.now);
  CHECK(f.state() == State::kPausedForObs && !f.backend.chain_running && f.backend.imports == 2);

  // Nothing changed in OBS: the same chain comes back, and no revision.
  f.controller.SetObsRunning(false, f.now);
  CHECK(f.state() == State::kRunning && f.backend.imports == 3 && f.last().chain_revision == 1);
}

TEST(CoreCanKeepRunningWhileObsRuns) {
  Fixture f({.pause_for_obs = false});
  f.controller.Start(true, f.now);
  CHECK(f.state() == State::kRunning && f.last().obs_running && f.backend.chain_starts == 1);

  // OBS exiting still re-imports, and the same chain carries on.
  f.controller.SetObsRunning(false, f.now);
  CHECK(f.state() == State::kRunning && f.backend.imports == 2 && f.backend.chain_starts == 1);

  f.controller.SetObsRunning(true, f.now);
  CHECK(f.state() == State::kRunning && f.backend.chain_running);
  f.controller.Apply({}, f.now);
  CHECK(f.state() == State::kPausedForObs && !f.backend.chain_running);
}

TEST(CoreImportsAgainOnlyForSettingsThatChangeTheImport) {
  Fixture f;
  f.backend.devices.outputs.push_back({"CABLE-A Input (VB-Audio Cable A)", kOtherCableId});
  f.controller.Start(true, f.now);
  CHECK(f.state() == State::kPausedForObs && f.backend.imports == 1);

  // A change made in OBS while it's open waits for OBS to exit, even if a
  // setting changes meanwhile.
  f.backend.collection[0].chain_key = "chain-2";
  Settings settings{.pause_for_obs = false};
  f.controller.Apply(settings, f.now);
  CHECK(f.state() == State::kRunning && f.backend.imports == 1 && f.last().chain_revision == 1);
  settings.cable = kOtherCableId;
  f.controller.Apply(settings, f.now);
  CHECK(f.backend.imports == 1 && f.backend.chain_starts == 2 && f.backend.last_plan->cable.id == kOtherCableId);
  CHECK(f.backend.last_plan->chain_key == "chain-1" && f.last().chain_revision == 1);
  f.controller.SetObsRunning(false, f.now);
  CHECK(f.backend.imports == 2 && f.last().chain_revision == 2 && f.backend.last_plan->chain_key == "chain-2");

  // What's imported: the mic, the install and OBS's settings folder.
  settings.mic = "Mic/Aux";
  f.controller.Apply(settings, f.now);
  settings.obs_dir = "D:\\obs-studio";
  f.controller.Apply(settings, f.now);
  settings.obs_config = "D:\\obs-studio\\config";
  f.controller.Apply(settings, f.now);
  CHECK(f.backend.imports == 5 && f.state() == State::kRunning && f.backend.chain_starts == 3);

  // The profile's cable is now the default device, which is no cable.
  f.backend.config.audio.monitoring_device_id = "default";
  f.controller.Reimport(f.now);
  CHECK(f.backend.imports == 6 && f.state() == State::kRunning);
  settings.cable.clear();
  f.controller.Apply(settings, f.now);
  CHECK(f.backend.imports == 6 && f.state() == State::kNeedsSetup && f.last().setup == SetupNeed::kCable);
  CHECK(!f.backend.chain_running);
}

TEST(CoreSnapshotHasWhatTheTrayOffers) {
  Fixture f;
  f.controller.Start(false, f.now);
  CHECK(f.last().outputs == DefaultDevices().outputs);
  CHECK(f.last().obs_cable == (audio::AudioDevice{kCableName, kCableId}));
  CHECK(!f.last().paused_by_user);

  // A picked cable that isn't connected goes by the name it was picked
  // under.
  f.controller.Apply({.cable = kOtherCableId, .cable_name = "CABLE-A Input (VB-Audio Cable A)"}, f.now);
  CHECK(f.state() == State::kCableMissing && f.last().chain->cable == "CABLE-A Input (VB-Audio Cable A)");
  CHECK(f.last().detail == "\"CABLE-A Input (VB-Audio Cable A)\" isn't connected.");

  // Paused shows through states that come before it.
  f.controller.Apply({}, f.now);
  f.controller.Pause(f.now);
  CHECK(f.state() == State::kPausedByUser && f.last().paused_by_user);
  f.backend.config_error = "OBS has no settings yet.";
  f.controller.Reimport(f.now);
  CHECK(f.state() == State::kNeedsSetup && f.last().paused_by_user);
  // The profile is unknown again, and so is its cable.
  CHECK(f.last().obs_cable.id.empty());

  f.backend.devices.outputs.pop_back();
  f.DevicesChange(f.backend.devices);
  CHECK(f.last().outputs.size() == 1);
}

TEST(CoreFollowsChangesMadeInObs) {
  Fixture f;
  f.controller.Start(false, f.now);
  f.controller.SetObsRunning(true, f.now);
  f.backend.collection = {MakeMic("Mic/Aux", kMicId, "chain-2", {"Noise Suppression", "Compressor", "Limiter"})};
  f.controller.SetObsRunning(false, f.now);
  CHECK(f.state() == State::kRunning && f.last().chain_revision == 2);
  CHECK(f.backend.last_plan->chain_key == "chain-2" && f.last().chain->filters.size() == 3);
}

TEST(CoreReloadsARunningChainOnlyWhenItChanged) {
  Fixture f;
  f.backend.devices.outputs.push_back({"CABLE-A Input (VB-Audio Cable A)", kOtherCableId});
  f.controller.Start(false, f.now);
  f.controller.Reimport(f.now);
  CHECK(f.backend.chain_starts == 1 && f.backend.chain_stops == 0);

  // Muting the mic in OBS changes its JSON but not its key: the monitor
  // ignores mute.
  f.backend.collection[0].source_json = R"({"key": "chain-1", "muted": true})";
  f.controller.Reimport(f.now);
  CHECK(f.backend.chain_starts == 1 && f.last().chain_revision == 1);

  f.backend.collection[0].chain_key = "chain-2";
  f.controller.Reimport(f.now);
  CHECK(f.backend.chain_starts == 2 && f.backend.chain_stops == 1 && f.last().chain_revision == 2);
  CHECK(f.backend.last_plan->source_json == R"({"key": "chain-1", "muted": true})");

  // A new monitoring device in the profile reloads too, but isn't a new
  // chain.
  f.backend.config.audio.monitoring_device_id = kOtherCableId;
  f.controller.Reimport(f.now);
  CHECK(f.backend.chain_starts == 3 && f.backend.last_plan->cable.id == kOtherCableId);
  CHECK(f.last().chain_revision == 2 && f.last().chain->cable == "CABLE-A Input (VB-Audio Cable A)");
}

TEST(CoreKeepsTheChainThroughRenamesAndCaseChanges) {
  // OBS's profile has the cable's ID in a different case from Windows.
  Fixture f;
  f.backend.config.audio.monitoring_device_id = "{0.0.0.00000000}.{CABLE}";
  f.controller.Start(false, f.now);
  CHECK(f.state() == State::kRunning && f.backend.chain_starts == 1);

  // Picking that cable in the tray, under Windows' ID, is the same cable.
  f.controller.Apply({.cable = kCableId, .cable_name = kCableName}, f.now);
  CHECK(f.state() == State::kRunning && f.backend.chain_starts == 1 && f.backend.chain_stops == 0);

  // Renaming the mic or a filter in OBS leaves the key as it was: the names
  // shown change, and the chain carries on.
  f.backend.collection[0].mic.name = "Mic";
  f.backend.collection[0].filters = {"Noise Suppression", "Comp"};
  f.backend.collection[0].source_json = R"({"key": "chain-1", "name": "Mic"})";
  f.controller.Reimport(f.now);
  CHECK(f.state() == State::kRunning && f.backend.chain_starts == 1 && f.last().chain_revision == 1);
  CHECK(f.last().chain->mic == "Mic" && f.last().chain->filters.back() == "Comp" && f.last().mics[0].name == "Mic");
}

TEST(CoreAsksForARestartToFollowObs) {
  Fixture f;
  f.controller.Start(false, f.now);
  f.backend.config.audio.sample_rate = 44100;
  f.backend.config.audio.channel_setup = "Mono";
  f.backend.config.audio.speakers = SPEAKERS_MONO;
  f.controller.Reimport(f.now);
  CHECK(f.state() == State::kRestartNeeded && !f.backend.chain_running && f.backend.libobs_starts == 1);
  CHECK(f.last().restart == RestartNeed::kAudioChanged);
  CHECK(f.last().detail.find("from 48000 Hz Stereo to 44100 Hz Mono") != std::string::npos);

  // Changed back before the restart: carries on.
  f.backend.config = DefaultConfig();
  f.controller.Reimport(f.now);
  CHECK(f.state() == State::kRunning);

  f.backend.obs.install.version = {32, 2, 3};
  f.controller.Reimport(f.now);
  CHECK(f.state() == State::kRestartNeeded && f.last().detail.find("from 32.2.2 to 32.2.3") != std::string::npos);
  CHECK(f.last().restart == RestartNeed::kObsUpdated);

  f.backend.obs = {ObsFound::kUnsupported, {"C:\\obs-studio", {33, 0, 0}}, "OBS 33.0.0 isn't supported."};
  f.controller.SetObsRunning(true, f.now);
  f.controller.SetObsRunning(false, f.now);
  CHECK(f.state() == State::kObsUnsupported && !f.backend.chain_running && f.last().restart == RestartNeed::kNone);
}

TEST(CoreRebuildsTheChainWhenALostMicComesBack) {
  Fixture f;
  f.controller.Start(false, f.now);
  CHECK(f.state() == State::kRunning);

  f.backend.devices = NoMics();
  f.controller.DevicesChanged(f.now);
  f.Advance(400ms);
  CHECK(f.state() == State::kRunning && f.backend.device_lists == 1);  // Still settling.
  f.Advance(100ms);
  CHECK(f.state() == State::kMicMissing && !f.backend.chain_running && f.backend.device_lists == 2);
  CHECK(f.last().detail == "The recording device for \"Mic/Aux\" isn't connected.");

  // libobs's loader runs again: a source whose device went away never
  // recovers by itself without video.
  f.DevicesChange(DefaultDevices());
  CHECK(f.state() == State::kRunning && f.backend.chain_starts == 2);
}

TEST(CoreSettlesDeviceNotificationBursts) {
  Fixture f;
  f.controller.Start(false, f.now);
  for (int i = 0; i < 3; ++i) {
    f.controller.DevicesChanged(f.now);
    f.Advance(300ms);
  }
  CHECK(f.backend.device_lists == 1);  // Only Start's.
  f.Advance(200ms);
  CHECK(f.backend.device_lists == 2);
  // Notifications that never stop are still acted on within 2 s.
  const Clock::time_point first = f.now;
  while (f.backend.device_lists == 2 && f.now - first < 10s) {
    f.controller.DevicesChanged(f.now);
    f.Advance(300ms);
  }
  CHECK(f.backend.device_lists == 3 && f.now - first <= 2100ms);
}

TEST(CoreWaitsForTheCable) {
  Fixture f;
  f.backend.devices = NoMics();
  f.backend.devices.outputs.pop_back();
  f.controller.Start(false, f.now);
  // Before the mic: a missing cable needs the user more.
  CHECK(f.state() == State::kCableMissing && f.backend.chain_starts == 0);
  CHECK(f.last().detail == std::string("\"") + kCableName + "\" isn't connected.");
  f.DevicesChange(DefaultDevices());
  CHECK(f.state() == State::kRunning && f.backend.chain_starts == 1);
}

TEST(CoreRebuildsADefaultMicWhenTheDefaultChanges) {
  Fixture f;
  f.backend.collection = {MakeMic("Mic/Aux", "default", "chain-1")};
  f.backend.devices.mics.push_back({"Headset Microphone", kHeadsetId});
  f.controller.Start(false, f.now);
  CHECK(f.state() == State::kRunning && f.backend.chain_starts == 1);

  // win-wasapi stops a "default" mic for good when the default changes.
  audio::Endpoints devices = f.backend.devices;
  devices.default_mic = kHeadsetId;
  f.DevicesChange(devices);
  CHECK(f.state() == State::kRunning && f.backend.chain_starts == 2 && f.backend.chain_stops == 1);

  f.DevicesChange(NoMics());
  CHECK(f.state() == State::kMicMissing && f.last().detail == "There's no recording device.");
}

TEST(CoreRebuildsAChainThatStopsGettingAudio) {
  Fixture f;
  f.controller.Start(false, f.now);
  f.Advance(10s);
  CHECK(f.state() == State::kRunning && f.backend.chain_starts == 1);

  f.backend.flowing = false;
  f.Advance(2s);
  CHECK(f.state() == State::kRunning);
  f.Advance(1s);
  CHECK(f.state() == State::kMicMissing && !f.backend.chain_running);
  CHECK(f.last().detail.find("\"Mic/Aux\" stopped sending audio.") == 0 &&
        f.last().detail.find(" 2 s.") != std::string::npos);

  // Rebuilt after 2 s; still no audio, so the next wait is longer.
  f.Advance(2s);
  CHECK(f.state() == State::kRunning && f.backend.chain_starts == 2);
  f.Advance(3s);
  CHECK(f.state() == State::kMicMissing && f.last().detail.find(" 5 s.") != std::string::npos);

  // Audio again. After 30 s of it, the waits start over.
  f.backend.flowing = true;
  f.Advance(5s);
  CHECK(f.state() == State::kRunning && f.backend.chain_starts == 3);
  f.Advance(30s);
  f.backend.flowing = false;
  f.Advance(3s);
  CHECK(f.state() == State::kMicMissing && f.last().detail.find(" 2 s.") != std::string::npos);

  // A device notification tries again at once.
  f.backend.flowing = true;
  f.DevicesChange(DefaultDevices());
  CHECK(f.state() == State::kRunning && f.backend.chain_starts == 4);
}

TEST(CoreTimesTheWatchdogFromTheChainsStart) {
  Fixture f;
  // The first start makes the runtime copy, which can take seconds.
  f.backend.on_start_libobs = [&f] { f.now += 5s; };
  f.backend.flowing = false;  // The mic's first packets haven't come yet.
  f.controller.Start(false, f.now);
  // The core's thread ticks right after.
  f.controller.Tick(f.now);
  CHECK(f.state() == State::kRunning && f.backend.chain_running);
  f.backend.flowing = true;
  f.Advance(10s);
  CHECK(f.state() == State::kRunning && f.backend.chain_starts == 1);
}

TEST(CoreStaysPausedUntilResumed) {
  Fixture f;
  f.controller.Start(false, f.now);
  f.controller.Pause(f.now);
  CHECK(f.state() == State::kPausedByUser && !f.backend.chain_running);
  f.controller.SetObsRunning(true, f.now);
  CHECK(f.state() == State::kPausedByUser && f.last().obs_running);
  f.controller.SetObsRunning(false, f.now);
  CHECK(f.state() == State::kPausedByUser && f.backend.imports == 2 && f.backend.chain_starts == 1);

  // A missing mic doesn't matter while paused.
  f.DevicesChange(NoMics());
  CHECK(f.state() == State::kPausedByUser);
  f.controller.Resume(f.now);
  CHECK(f.state() == State::kMicMissing);
  f.DevicesChange(DefaultDevices());
  CHECK(f.state() == State::kRunning && f.backend.chain_starts == 2);

  // Resuming while OBS runs waits for OBS.
  f.controller.Pause(f.now);
  f.controller.SetObsRunning(true, f.now);
  f.controller.Resume(f.now);
  CHECK(f.state() == State::kPausedForObs && !f.backend.chain_running);
}

TEST(CoreTriesAFailedChainAgain) {
  Fixture f;
  f.backend.chain_error = "libobs couldn't monitor to \"CABLE In 16ch\".";
  f.controller.Start(false, f.now);
  CHECK(f.state() == State::kFailed && f.last().detail == *f.backend.chain_error && !f.backend.chain_running);
  f.Advance(10s);
  CHECK(f.state() == State::kFailed);  // No retrying in a loop.
  f.backend.chain_error.reset();
  f.DevicesChange(DefaultDevices());
  CHECK(f.state() == State::kRunning && f.backend.chain_starts == 1);
}

TEST(CoreReportsWhatStopsLibobsOrTheImport) {
  {
    Fixture f;
    f.backend.libobs_error = "libobs failed to start.";
    f.controller.Start(false, f.now);
    CHECK(f.state() == State::kFailed && f.last().detail == "libobs failed to start." && f.backend.imports == 0);
    f.backend.libobs_error.reset();
    f.controller.Reimport(f.now);
    CHECK(f.state() == State::kRunning && f.backend.libobs_starts == 2);
  }
  {
    // A module that loaded keeps obs.dll in the process: only a restart
    // tries again, which the tray does by itself.
    Fixture f;
    f.backend.libobs_error = "The OBS module obs-filters failed to initialize. The log has details.";
    f.backend.libobs_can_retry = false;
    f.controller.Start(false, f.now);
    CHECK(f.state() == State::kFailed && f.last().detail == *f.backend.libobs_error);
    f.backend.libobs_error.reset();
    f.controller.Reimport(f.now);
    CHECK(f.state() == State::kRestartNeeded && f.last().restart == RestartNeed::kNone);
    f.controller.SetObsRunning(true, f.now);
    f.controller.SetObsRunning(false, f.now);
    f.controller.Apply({.mic = "Mic/Aux"}, f.now);
    CHECK(f.state() == State::kRestartNeeded && f.backend.libobs_starts == 1 && !f.backend.chain_running);
  }
  {
    Fixture f;
    f.backend.import_error = "Couldn't read the scene collection.";
    f.controller.Start(false, f.now);
    CHECK(f.state() == State::kFailed && !f.last().chain && f.backend.chain_starts == 0);
  }
}

TEST(CorePublishesOnlyChanges) {
  Fixture f;
  f.controller.Start(false, f.now);
  const size_t count = f.published.size();
  f.controller.Reimport(f.now);
  f.controller.DevicesChanged(f.now);
  f.Advance(5s);
  f.controller.SetObsRunning(false, f.now);
  CHECK(f.published.size() == count && f.backend.imports == 2);

  f.controller.Stop();
  CHECK(!f.backend.chain_running);
  f.controller.Pause(f.now);
  f.controller.DevicesChanged(f.now);
  CHECK(f.published.size() == count && !f.controller.NextDeadline());
}

// --- ObsBackend --------------------------------------------------------------------

// A fresh folder under %TEMP%, removed when the test ends.
struct ScratchDir {
  std::filesystem::path path;
  explicit ScratchDir(std::wstring_view name)
      : path(std::filesystem::temp_directory_path() /
             std::format(L"knobs-tests-{}-{}", GetCurrentProcessId(), name)) {
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~ScratchDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

void WriteFile(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << contents;
}

TEST(ObsBackendKeepsToAPickedInstallFolder) {
  // A folder picked with Find OBS… that OBS has gone from stays picked, even
  // where OBS is installed as usual: its error shows, for the user to say.
  const ScratchDir dir(L"picked-obs");
  ObsBackend backend({});
  const ObsCheck picked = backend.CheckObs({.obs_dir = dir.path});
  CHECK(picked.found == ObsFound::kMissing);
  CHECK(picked.message.find("isn't a complete OBS Studio install") != std::string::npos);
}

TEST(ObsBackendKeepsToAPickedInstallOfAnUnsupportedObs) {
  // A portable OBS as CheckObs inspects it, with Windows' own version.dll as
  // its obs.dll: a version no OBS has, so not one knobs supports.
  const ScratchDir dir(L"picked-unsupported");
  const fs::path root = dir.path / L"OBS 31";
  wchar_t system[MAX_PATH];
  GetSystemDirectoryW(system, MAX_PATH);
  fs::create_directories(runtime::BinDir(root));
  fs::copy_file(fs::path(system) / L"version.dll", runtime::ObsDll(root));
  WriteFile(runtime::BinDir(root) / (fs::path(runtime::kGraphicsModule) += L".dll"), "");
  fs::create_directories(runtime::LibobsDataDir(root));
  for (const std::string_view module : runtime::kObsModules) {
    WriteFile(runtime::PluginDll(root, module), "");
    fs::create_directories(runtime::PluginDataDir(root, module));
  }
  WriteFile(root / L"portable_mode.txt", "");

  // It stays picked, even where OBS is installed as usual: the check is of
  // this folder's OBS, and of its settings folder.
  ObsBackend backend({});
  ObsCheck check = backend.CheckObs({.obs_dir = root});
  CHECK(check.found == ObsFound::kUnsupported && check.install.root == root);
  CHECK(check.message.find("isn't supported") != std::string::npos);
  CHECK(check.config.ok() && check.config->path == root / L"config" && check.config->obs_writes_here);
  // Its settings folder picked, however it's spelled, is OBS's own; another
  // isn't, so the errors don't suggest opening OBS for it.
  check = backend.CheckObs({.obs_dir = root, .obs_config = root / L"CONFIG" / L""});
  CHECK(check.config.ok() && check.config->path == root / L"config" && check.config->obs_writes_here);
  check = backend.CheckObs({.obs_dir = root, .obs_config = dir.path / L"elsewhere"});
  CHECK(check.config.ok() && check.config->path == dir.path / L"elsewhere" && !check.config->obs_writes_here);
}

// --- ObsWatch ----------------------------------------------------------------------

// Names for OBS of this run's own, so that the OBSes the tests make up aren't
// seen by a knobs or an OBS that's running, or by another run of the tests.
ObsNames TestObsNames() {
  return {.mutex = std::format(L"knobs-tests-{}-obs", GetCurrentProcessId()),
          .exe = std::format(L"knobs-tests-{}-obs.exe", GetCurrentProcessId())};
}

// Runs ping.exe under the name `names.exe`, with `security` for its process,
// until End() or the end of the test. It would last 30 s.
struct PingAsObs {
  explicit PingAsObs(const ObsNames& names, SECURITY_ATTRIBUTES* security = nullptr) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    const fs::path exe = dir / names.exe;
    fs::copy_file(fs::path(system) / L"ping.exe", exe, fs::copy_options::overwrite_existing, ec);
    std::wstring command = names.exe + L" -n 30 127.0.0.1";
    STARTUPINFOW startup = {sizeof(startup)};
    started = !ec && CreateProcessW(exe.c_str(), command.data(), security, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                    nullptr, &startup, &process);
  }
  ~PingAsObs() {
    End();
    if (started) {
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  void End() {
    if (!started) return;
    TerminateProcess(process.hProcess, 0);
    WaitForSingleObject(process.hProcess, 5000);
  }

  const fs::path dir = fs::temp_directory_path() / std::format(L"knobs-tests-{}-obs", GetCurrentProcessId());
  PROCESS_INFORMATION process = {};
  bool started = false;
};

TEST(ObsWatchLooksForObsByDefault) {
  CHECK(ObsNames{}.mutex == L"OBSStudioCore" && ObsNames{}.exe == L"obs64.exe");
}

TEST(ObsWatchSeesObsByItsMutex) {
  const ObsNames names = TestObsNames();
  ObsWatch watch(names);
  CHECK(!watch.Running());
  // What an installed OBS creates first thing.
  const HANDLE mutex = CreateMutexW(nullptr, FALSE, names.mutex.c_str());
  CHECK(mutex != nullptr);
  CHECK(watch.Running());
  CloseHandle(mutex);
  CHECK(!watch.Running());
}

TEST(ObsWatchSeesObsByItsProcessUntilItEnds) {
  // Any program with OBS's name counts, as a portable OBS does.
  const ObsNames names = TestObsNames();
  PingAsObs obs(names);
  CHECK(obs.started);
  if (!obs.started) return;
  ObsWatch watch(names);  // Scans on its first call.
  CHECK(watch.Running());
  obs.End();
  CHECK(!watch.Running());  // Its handle says so, without another scan.
}

TEST(ObsWatchCountsAProcessItCantOpenUntilItEnds) {
  // No one may open it: an empty DACL.
  SECURITY_DESCRIPTOR descriptor;
  ACL acl;
  InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION);
  InitializeAcl(&acl, sizeof(acl), ACL_REVISION);
  SetSecurityDescriptorDacl(&descriptor, TRUE, &acl, FALSE);
  SECURITY_ATTRIBUTES security = {sizeof(security), &descriptor, FALSE};
  const ObsNames names = TestObsNames();
  PingAsObs obs(names, &security);
  CHECK(obs.started);
  if (!obs.started) return;
  // Unless this process may open any process (SeDebugPrivilege).
  if (const HANDLE opened = OpenProcess(SYNCHRONIZE, FALSE, obs.process.dwProcessId)) {
    CloseHandle(opened);
    return;
  }
  ObsWatch watch(names);
  CHECK(watch.Running());
  obs.End();
  // Without a handle, the next scan tells.
  std::this_thread::sleep_for(2100ms);
  CHECK(!watch.Running());
}

TEST(ObsWatchKeepsToItsSession) {
  // A process in another session can't be opened from this one.
  // services.exe runs in session 0, which runs services: no one signs in to
  // it, so it isn't noted either.
  DWORD session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || session == 0) return;
  ObsNames names = TestObsNames();
  names.exe = L"services.exe";
  ObsWatch watch(names);
  CHECK(!watch.Running());
  CHECK(watch.Others().empty());
}

TEST(ObsWatchNotesObsInOtherSessions) {
  // Sessions this one can't make: Alex's, this account's signed in again,
  // and one whose account can't be read. And session 0.
  DWORD here = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &here) || here == 0) return;
  const uint32_t alex = here + 1;
  const uint32_t again = here + 2;
  const uint32_t unknown = here + 3;
  std::optional<std::vector<ObsProcess>> processes =
      std::vector<ObsProcess>{{100, unknown}, {104, alex}, {108, 0}, {112, again}, {116, alex}};
  ObsWatch::System system{
      .processes = [&processes](std::wstring_view) { return processes; },
      .account = [&](unsigned long session) -> std::string {
        if (session == here || session == again) return "PC\\Daniel";
        return session == alex ? "PC\\Alex" : "";
      },
  };
  const ObsNames names = TestObsNames();
  ObsWatch watch(names, system);
  CHECK(!watch.Running());
  CHECK(watch.Others() == (std::vector<OtherObs>{{.session = alex, .account = "Alex"},
                                                 {.session = again, .account = "Daniel", .yours = true},
                                                 {.session = unknown}}));
  // Gone.
  processes = std::vector<ObsProcess>{};
  const HANDLE mutex = CreateMutexW(nullptr, FALSE, names.mutex.c_str());  // Scans at once.
  CHECK(watch.Running());
  CHECK(watch.Others().empty());
  CloseHandle(mutex);
}

TEST(ObsWatchKeepsWhatItFoundWhenAScanFails) {
  DWORD here = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &here);
  // OBS in this session that can't be opened (no process has the ID), and
  // OBS in another.
  std::optional<std::vector<ObsProcess>> processes =
      std::vector<ObsProcess>{{0xfffffff0, here}, {100, here + 1}};
  ObsWatch watch(TestObsNames(), {.processes = [&processes](std::wstring_view) { return processes; }});
  CHECK(watch.Running() && watch.Others().size() == 1);
  // The process list can't be read: nothing has changed as far as anyone
  // knows.
  processes.reset();
  std::this_thread::sleep_for(2100ms);
  CHECK(watch.Running() && watch.Others().size() == 1);
}

TEST(OtherObsIsDescribedByAccount) {
  const OtherObs alex{.session = 2, .account = "Alex"};
  const OtherObs sam{.session = 3, .account = "Sam"};
  const OtherObs yours{.session = 4, .account = "Daniel", .yours = true};
  CHECK(DescribeOtherObs({alex}) == "Alex's account");
  CHECK(DescribeOtherObs({{.session = 2}}) == "another Windows account");
  CHECK(DescribeOtherObs({alex, sam}) == "2 other Windows accounts");
  CHECK(DescribeOtherObs({{.session = 2}, {.session = 3}}) == "2 other Windows accounts");
  // An account signed in twice counts once.
  CHECK(DescribeOtherObs({alex, {.session = 5, .account = "Alex"}}) == "Alex's account");
  CHECK(DescribeOtherObs({yours}) == "your other session");
  CHECK(DescribeOtherObs({yours, {.session = 5, .account = "Daniel", .yours = true}}) == "your other sessions");
  CHECK(DescribeOtherObs({alex, yours}) == "2 other Windows sessions");
}

// --- Core --------------------------------------------------------------------------

// Records what the core tells it, and waits for it.
struct WaitingObserver : Observer {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<Snapshot> seen;

  void OnSnapshot(const Snapshot& snapshot) override {
    {
      std::lock_guard lock(mutex);
      seen.push_back(snapshot);
    }
    changed.notify_all();
  }

  bool WaitFor(State state) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, 10s, [&] { return !seen.empty() && seen.back().state == state; });
  }
};

TEST(CoreRunsLibobsOnAThreadOfItsOwn) {
  struct Calls {
    std::mutex mutex;
    std::vector<DWORD> threads;
    DWORD destroyed_on = 0;
    bool chain_running_at_end = true;
  };
  auto calls = std::make_shared<Calls>();
  auto backend = std::make_unique<FakeBackend>();
  backend->on_call = [calls] {
    std::lock_guard lock(calls->mutex);
    calls->threads.push_back(GetCurrentThreadId());
  };
  backend->on_destroy = [calls](bool chain_running) {
    calls->destroyed_on = GetCurrentThreadId();
    calls->chain_running_at_end = chain_running;
  };

  std::atomic<bool> obs = false;
  WaitingObserver observer;
  {
    CoreOptions options;
    options.obs_running = [&obs] { return obs.load(); };
    options.obs_poll = 10ms;
    options.watch_devices = false;
    auto core = Core::Start(std::move(backend), observer, std::move(options));
    CHECK(core.ok());
    if (!core) return;
    CHECK(observer.WaitFor(State::kRunning));
    obs = true;
    CHECK(observer.WaitFor(State::kPausedForObs));
    obs = false;
    CHECK(observer.WaitFor(State::kRunning));
    (*core)->Pause();
    CHECK(observer.WaitFor(State::kPausedByUser));
    (*core)->Resume();
    CHECK(observer.WaitFor(State::kRunning));
  }
  CHECK(observer.seen.front().state == State::kStarting);
  // Every call and the backend's end on one thread, not this one, with the
  // chain stopped first.
  CHECK(!calls->threads.empty());
  const DWORD core_thread = calls->threads.empty() ? 0 : calls->threads.front();
  CHECK(core_thread != GetCurrentThreadId());
  CHECK(std::all_of(calls->threads.begin(), calls->threads.end(), [&](DWORD id) { return id == core_thread; }));
  CHECK(calls->destroyed_on == core_thread && !calls->chain_running_at_end);
}

}  // namespace
