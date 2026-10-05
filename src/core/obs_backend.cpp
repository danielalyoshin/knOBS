// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/obs_backend.h"

#include <format>
#include <utility>

#include "app_info.h"
#include "audio/audio_devices.h"
#include "runtime/obs_version.h"
#include "util/win_strings.h"

namespace knobs::core {

ObsBackend::ObsBackend(ObsBackendOptions options) : options_(std::move(options)) {}

ObsBackend::~ObsBackend() {
  StopChain();
  if (!host_) return;
  const long leaks = host_->Shutdown();
  if (leaks != 0) host_->log().Write(LOG_WARNING, std::format("libobs leaked {} allocation(s).", leaks));
  if (options_.on_shutdown) options_.on_shutdown(leaks, host_->log());
}

ObsCheck ObsBackend::CheckObs(const Settings& settings) {
  ObsCheck check;
  if (!settings.obs_dir && runtime::ObsInstallCandidates().empty()) {
    check.message = std::format("OBS Studio isn't installed. {} needs {}.", kDisplayName,
                                runtime::DescribeSupportedObsVersions());
    return check;
  }
  auto install = settings.obs_dir ? runtime::InspectObsInstall(*settings.obs_dir) : runtime::FindObsInstall();
  if (!install) {
    check.message = install.error();
    return check;
  }
  check.install = *install;
  if (!runtime::IsSupportedObsVersion(install->version)) {
    check.found = ObsFound::kUnsupported;
    check.message = runtime::UnsupportedObsMessage(install->version.ToString());
    return check;
  }
  check.found = ObsFound::kYes;
  return check;
}

Result<import::ActiveObsConfig> ObsBackend::ReadObsConfig(const Settings& settings,
                                                          const runtime::ObsInstall& install) {
  import::ObsConfigRoot root;
  if (settings.obs_config) {
    root = import::ObsConfigRootAt(*settings.obs_config);
  } else {
    auto found = import::FindObsConfigRoot(install.root);
    if (!found) return Error{found.error()};
    root = *found;
  }
  return import::FindActiveObsConfig(root);
}

Status ObsBackend::StartLibobs(const runtime::ObsInstall& install, const import::ProfileAudio& audio) {
  if (host_) return Error{"libobs is already running."};
  runtime::HostOptions options;
  options.install = install;
  options.log_prefix = options_.log_prefix;
  options.verbose = options_.verbose;
  options.samples_per_sec = audio.sample_rate;
  options.speakers = audio.speakers;
  auto host = runtime::ObsHost::Start(options);
  if (!host) return Error{host.error()};
  host_ = std::move(*host);
  return Ok{};
}

Result<MicImport> ObsBackend::ImportMic(const import::ActiveObsConfig& config, std::string_view pick) {
  if (!host_) return Error{"libobs isn't running."};
  auto collection = import::ReadActiveCollection(host_->api(), config);
  if (!collection) return Error{collection.error()};
  if ((*collection)->from_backup()) {
    Log(std::format("The scene collection doesn't parse, so this is its backup: {}", ToUtf8((*collection)->file())));
  }
  MicImport result;
  result.mics = (*collection)->Mics();
  auto picked = import::PickMic(result.mics, pick);
  if (!picked) {
    result.pick_error = picked.error();
    return result;
  }
  auto imported = (*collection)->Import(result.mics[*picked]);
  if (!imported) return Error{imported.error()};
  result.picked = *picked;
  result.mic = std::move(*imported);
  return result;
}

audio::Endpoints ObsBackend::ListDevices() { return audio::ListEndpoints(); }

Status ObsBackend::StartChain(const ChainPlan& plan) {
  if (!host_) return Error{"libobs isn't running."};
  StopChain();
  const runtime::ObsApi& api = host_->api();
  // First: the monitor opens its device as soon as monitoring starts.
  const Status routed = audio::SetMonitoringDevice(api, plan.cable);
  if (!routed) return routed;
  auto chain = audio::LiveChain::Start(api, host_->session(), plan.source_json, {.load_callbacks = plan.load_callbacks});
  if (!chain) return Error{chain.error()};
  chain_ = std::move(*chain);
  packets_ = 0;
  api.obs_source_add_audio_capture_callback(chain_->source(), CountPacket, this);
  return Ok{};
}

void ObsBackend::StopChain() {
  if (!chain_) return;
  host_->api().obs_source_remove_audio_capture_callback(chain_->source(), CountPacket, this);
  chain_.reset();
}

uint64_t ObsBackend::ChainPackets() { return packets_.load(std::memory_order_relaxed); }

void ObsBackend::Log(std::string_view line) {
  if (host_) host_->log().Write(LOG_INFO, line);
}

void ObsBackend::CountPacket(void* param, obs_source_t*, const audio_data*, bool) {
  static_cast<ObsBackend*>(param)->packets_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace knobs::core
