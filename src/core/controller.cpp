// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/controller.h"

#include <algorithm>
#include <format>
#include <utility>

#include "app_info.h"
#include "util/win_strings.h"

namespace knobs::core {
namespace {

// win-wasapi's and OBS's name for the default device.
constexpr std::string_view kDefaultDevice = "default";

// Endpoint IDs, which may differ in case between OBS's settings and Windows.
bool SameId(std::string_view a, std::string_view b) { return AsciiLower(a) == AsciiLower(b); }

const audio::AudioDevice* FindById(const std::vector<audio::AudioDevice>& devices, std::string_view id) {
  const auto found = std::find_if(devices.begin(), devices.end(),
                                  [&](const audio::AudioDevice& device) { return SameId(device.id, id); });
  return found == devices.end() ? nullptr : &*found;
}

std::string Seconds(Controller::Clock::duration duration) {
  return std::format("{:.3g} s", std::chrono::duration<double>(duration).count());
}

}  // namespace

Controller::Controller(Backend& backend, Settings settings, Publish publish, Timing timing)
    : backend_(backend), settings_(std::move(settings)), publish_(std::move(publish)), timing_(std::move(timing)) {
  if (timing_.retry_delays.empty()) timing_.retry_delays.push_back(timing_.stall_timeout);
  snapshot_.settings = settings_;
}

void Controller::Start(bool obs_running, Clock::time_point now) {
  obs_running_ = obs_running;
  snapshot_.obs_running = obs_running;
  publish_(snapshot_);
  devices_ = backend_.ListDevices();
  Refresh();
  Reconcile(now);
}

void Controller::SetObsRunning(bool running, Clock::time_point now) {
  if (stopped_ || running == obs_running_) return;
  obs_running_ = running;
  if (running) {
    backend_.Log("OBS started.");
  } else {
    // OBS saves its settings as it exits, so they're complete by now.
    backend_.Log("OBS exited. Importing its settings again.");
    ClearFailures();
    Refresh();
  }
  Reconcile(now);
}

void Controller::DevicesChanged(Clock::time_point now) {
  if (stopped_) return;
  if (!devices_first_) devices_first_ = now;
  devices_due_ = std::min(now + timing_.device_settle, *devices_first_ + timing_.device_settle_max);
}

void Controller::Pause(Clock::time_point now) {
  if (stopped_) return;
  paused_by_user_ = true;
  Reconcile(now);
}

void Controller::Resume(Clock::time_point now) {
  if (stopped_) return;
  paused_by_user_ = false;
  ClearFailures();
  Reconcile(now);
}

void Controller::Reimport(Clock::time_point now) {
  if (stopped_) return;
  ClearFailures();
  Refresh();
  Reconcile(now);
}

void Controller::Apply(const Settings& settings, Clock::time_point now) {
  if (stopped_) return;
  settings_ = settings;
  ClearFailures();
  Refresh();
  Reconcile(now);
}

void Controller::Tick(Clock::time_point now) {
  if (stopped_) return;
  bool changed = false;
  if (devices_due_ && now >= *devices_due_) {
    devices_due_.reset();
    devices_first_.reset();
    audio::Endpoints devices = backend_.ListDevices();
    if (devices != devices_) {
      devices_ = std::move(devices);
      backend_.Log("Audio devices changed.");
    }
    // A device that came back may be what a stalled or failed chain lacked.
    chain_error_.reset();
    retry_at_.reset();
    changed = true;
  }
  if (loaded_ && now >= next_check_) {
    next_check_ = now + timing_.watchdog_interval;
    const uint64_t packets = backend_.ChainPackets();
    if (packets != last_packets_) {
      last_packets_ = packets;
      last_audio_ = now;
      if (now - running_since_ >= timing_.healthy_after) stalls_ = 0;
    } else if (now - last_audio_ >= timing_.stall_timeout) {
      const Clock::duration delay = timing_.retry_delays[std::min(stalls_, timing_.retry_delays.size() - 1)];
      ++stalls_;
      retry_at_ = now + delay;
      stall_detail_ = std::format("\"{}\" stopped sending audio. {} tries again in {}.", import_->mic->mic.name,
                                  kDisplayName, Seconds(delay));
      backend_.Log(std::format("No audio from the mic for {}.", Seconds(now - last_audio_)));
      changed = true;
    }
  }
  if (retry_at_ && now >= *retry_at_) {
    retry_at_.reset();
    changed = true;
  }
  if (changed) Reconcile(now);
}

std::optional<Controller::Clock::time_point> Controller::NextDeadline() const {
  if (stopped_) return std::nullopt;
  std::optional<Clock::time_point> next;
  const auto consider = [&next](std::optional<Clock::time_point> time) {
    if (time && (!next || *time < *next)) next = time;
  };
  consider(devices_due_);
  if (loaded_) consider(next_check_);
  consider(retry_at_);
  return next;
}

void Controller::Stop() {
  StopChain();
  stopped_ = true;
}

void Controller::Refresh() {
  problem_.reset();
  import_.reset();
  plan_.reset();
  obs_install_ = {};
  profile_cable_id_.clear();
  profile_cable_name_.clear();
  const auto fail = [this](State state, std::string detail, SetupNeed setup = SetupNeed::kNone) {
    problem_ = Problem{state, std::move(detail), setup};
  };

  const ObsCheck obs = backend_.CheckObs(settings_);
  if (obs.found == ObsFound::kMissing) return fail(State::kObsMissing, obs.message);
  obs_install_ = obs.install;
  if (obs.found == ObsFound::kUnsupported) return fail(State::kObsUnsupported, obs.message);
  // libobs stays loaded until the process ends (runtime::ObsRuntime).
  if (libobs_ && obs.install.version != libobs_->version) {
    return fail(State::kRestartNeeded, std::format("OBS was updated from {} to {}. {} has to restart to use it.",
                                                   libobs_->version.ToString(), obs.install.version.ToString(),
                                                   kDisplayName));
  }
  auto config = backend_.ReadObsConfig(settings_, obs.install);
  if (!config) return fail(State::kNeedsSetup, config.error(), SetupNeed::kObsSettings);
  const import::ProfileAudio& audio = config->audio;
  profile_cable_id_ = audio.monitoring_device_id;
  profile_cable_name_ = audio.monitoring_device_name;
  if (!libobs_) {
    const Status started = backend_.StartLibobs(obs.install, audio);
    if (!started) return fail(State::kFailed, started.error());
    libobs_ = Libobs{obs.install.version, audio.sample_rate, audio.speakers, audio.channel_setup};
  } else if (audio.sample_rate != libobs_->sample_rate || audio.speakers != libobs_->speakers) {
    // OBS restarts for this too (OBSBasic::GetRestartRequirements).
    return fail(State::kRestartNeeded,
                std::format("The OBS profile's audio changed from {} Hz {} to {} Hz {}. {} has to restart to follow "
                            "it, as OBS does.",
                            libobs_->sample_rate, libobs_->channel_setup, audio.sample_rate, audio.channel_setup,
                            kDisplayName));
  }

  auto imported = backend_.ImportMic(*config, settings_.mic);
  if (!imported) return fail(State::kFailed, imported.error());
  import_ = std::move(*imported);
  if (!import_->mic) {
    return fail(State::kNeedsSetup, import_->pick_error,
                import_->mics.empty() ? SetupNeed::kNoMic : SetupNeed::kPickMic);
  }
  const import::ImportedMic& mic = *import_->mic;
  if (mic.chain_key != chain_key_) {
    if (chain_revision_ > 0) backend_.Log("The mic or its chain changed in OBS.");
    chain_key_ = mic.chain_key;
    ++chain_revision_;
  }

  const std::string cable = settings_.cable.empty() ? audio.monitoring_device_id : settings_.cable;
  if (SameId(cable, kDefaultDevice)) {
    return fail(State::kNeedsSetup,
                "The OBS profile monitors to the default playback device, which is usually speakers. Choose the "
                "cable to send the mic to.",
                SetupNeed::kCable);
  }
  plan_ = ChainPlan{mic.source_json, mic.chain_key, mic.load_callbacks(), {"", cable}};
}

void Controller::Reconcile(Clock::time_point now) {
  if (stopped_) return;
  Snapshot next = Decide(now);
  if (next.state != State::kRunning) {
    StopChain();
  } else if (!loaded_ || !loaded_->SameAs(*plan_) || (MicIsDefault() && loaded_default_mic_ != devices_.default_mic)) {
    // A new chain, or the same one rebuilt: libobs's loader runs again, and
    // win-wasapi opens the device afresh.
    StopChain();
    ChainPlan plan = *plan_;
    plan.cable.name = CableName();
    const Status started = backend_.StartChain(plan);
    if (started) {
      loaded_ = std::move(plan);
      loaded_default_mic_ = devices_.default_mic;
      running_since_ = now;
      last_audio_ = now;
      last_packets_ = 0;
      next_check_ = now + timing_.watchdog_interval;
      backend_.Log(std::format("Loaded the chain: {}", FormatChain(*next.chain, false)));
    } else {
      chain_error_ = started.error();
      backend_.Log(std::format("The chain didn't start: {}", started.error()));
      next = Decide(now);
    }
  }
  if (next == snapshot_) return;
  snapshot_ = std::move(next);
  backend_.Log(DescribeSnapshot(snapshot_));
  publish_(snapshot_);
}

Snapshot Controller::Decide(Clock::time_point now) const {
  Snapshot next;
  next.obs_running = obs_running_;
  next.paused_by_user = paused_by_user_;
  next.chain_revision = chain_revision_;
  next.obs_cable = {profile_cable_name_, profile_cable_id_};
  next.outputs = devices_.outputs;
  next.inputs = devices_.mics;
  next.default_input = devices_.default_mic;
  next.obs = obs_install_;
  next.settings = settings_;
  if (import_) {
    next.mics = import_->mics;
    next.picked_mic = import_->picked;
    if (import_->mic) {
      next.notes = import_->mic->notes;
      next.chain = ChainSummary{import_->mic->mic.name, import_->mic->filters, CableName()};
    }
  }
  const auto set = [&next](State state, std::string detail = {}) {
    next.state = state;
    next.detail = std::move(detail);
  };
  if (problem_) {
    set(problem_->state, problem_->detail);
    next.setup = problem_->setup;
  } else if (chain_error_) {
    set(State::kFailed, *chain_error_);
  } else if (paused_by_user_) {
    set(State::kPausedByUser);
  } else if (obs_running_ && settings_.pause_for_obs) {
    set(State::kPausedForObs);
  } else if (!CablePresent()) {
    set(State::kCableMissing, std::format("\"{}\" isn't connected.", CableName()));
  } else if (!MicPresent()) {
    set(State::kMicMissing, MicIsDefault() ? std::string("There's no recording device.")
                                           : std::format("The recording device for \"{}\" isn't connected.",
                                                         import_->mic->mic.name));
  } else if (retry_at_ && now < *retry_at_) {
    set(State::kMicMissing, stall_detail_);
  } else {
    set(State::kRunning);
  }
  return next;
}

void Controller::StopChain() {
  if (!loaded_) return;
  backend_.StopChain();
  loaded_.reset();
}

void Controller::ClearFailures() {
  chain_error_.reset();
  retry_at_.reset();
  stalls_ = 0;
}

bool Controller::MicIsDefault() const {
  return import_ && import_->mic && SameId(import_->mic->mic.device_id, kDefaultDevice);
}

bool Controller::MicPresent() const {
  if (!import_ || !import_->mic) return false;
  if (MicIsDefault()) return !devices_.default_mic.empty();
  return FindById(devices_.mics, import_->mic->mic.device_id) != nullptr;
}

bool Controller::CablePresent() const { return plan_ && FindById(devices_.outputs, plan_->cable.id) != nullptr; }

std::string Controller::CableName() const {
  if (!plan_) return "";
  const std::string& id = plan_->cable.id;
  if (const audio::AudioDevice* device = FindById(devices_.outputs, id)) return device->name;
  if (SameId(id, profile_cable_id_) && !profile_cable_name_.empty()) return profile_cable_name_;
  if (SameId(id, settings_.cable) && !settings_.cable_name.empty()) return settings_.cable_name;
  return id;
}

}  // namespace knobs::core
