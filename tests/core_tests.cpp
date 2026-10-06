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
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/backend.h"
#include "core/controller.h"
#include "core/core.h"
#include "core/obs_process.h"
#include "core/state.h"
#include "test_harness.h"

namespace {

using namespace knobs;
using namespace knobs::core;
using namespace std::chrono_literals;
using Clock = Controller::Clock;

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
  ObsCheck obs{ObsFound::kYes, {"C:\\obs-studio", {32, 2, 2}}, ""};
  std::optional<std::string> config_error;
  import::ActiveObsConfig config = DefaultConfig();
  std::optional<std::string> libobs_error;
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
  std::function<void(bool chain_running)> on_destroy;

  ~FakeBackend() override {
    if (on_destroy) on_destroy(chain_running);
  }

  ObsCheck CheckObs(const Settings&) override {
    Called();
    return obs;
  }
  Result<import::ActiveObsConfig> ReadObsConfig(const Settings&, const runtime::ObsInstall&) override {
    Called();
    if (config_error) return Error{*config_error};
    return config;
  }
  Status StartLibobs(const runtime::ObsInstall&, const import::ProfileAudio& audio) override {
    Called();
    ++libobs_starts;
    libobs_audio = audio;
    if (libobs_error) return Error{*libobs_error};
    return Ok{};
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
      : controller(backend, std::move(settings), [this](const Snapshot& snapshot) { published.push_back(snapshot); }) {}

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

// --- ObsWatch ----------------------------------------------------------------------

TEST(ObsWatchSeesObsByItsMutex) {
  if (ObsWatch().Running()) return;  // OBS is running for real; nothing to tell apart.
  ObsWatch watch;
  // What an installed OBS creates first thing.
  const HANDLE mutex = CreateMutexW(nullptr, FALSE, L"OBSStudioCore");
  CHECK(mutex != nullptr);
  CHECK(watch.Running());
  CloseHandle(mutex);
  CHECK(!watch.Running());
}

TEST(ObsWatchSeesObsByItsProcessUntilItEnds) {
  if (ObsWatch().Running()) return;
  // Any program named obs64.exe counts, as a portable OBS does.
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / std::format(L"knobs-tests-{}-obs", GetCurrentProcessId());
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path exe = dir / L"obs64.exe";
  wchar_t system[MAX_PATH];
  GetSystemDirectoryW(system, MAX_PATH);
  fs::copy_file(fs::path(system) / L"ping.exe", exe, fs::copy_options::overwrite_existing, ec);
  CHECK(!ec);
  std::wstring command = L"obs64.exe -n 30 127.0.0.1";
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process = {};
  const bool started = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                      nullptr, &startup, &process);
  CHECK(started);
  if (started) {
    ObsWatch watch;  // Scans on its first call.
    CHECK(watch.Running());
    TerminateProcess(process.hProcess, 0);
    WaitForSingleObject(process.hProcess, 5000);
    CHECK(!watch.Running());  // Its handle says so, without another scan.
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
  fs::remove_all(dir, ec);
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
