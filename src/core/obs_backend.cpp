// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/obs_backend.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <utility>
#include <vector>

#include "app_info.h"
#include "audio/audio_devices.h"
#include "runtime/obs_layout.h"
#include "runtime/obs_version.h"
#include "runtime/runtime_copy.h"
#include "util/win_strings.h"

namespace knobs::core {
namespace {

namespace fs = std::filesystem;

// OBS's install where its installer puts it.
Result<runtime::ObsInstall> FindInstalledObs() {
  const std::vector<fs::path> candidates = runtime::ObsInstallCandidates();
  if (candidates.empty()) {
    return Error{std::format("OBS Studio isn't installed. {} needs {}.", kDisplayName,
                             runtime::DescribeSupportedObsVersions())};
  }
  return runtime::FindObsInstall(candidates);
}

// What pruning did, for the log. Nothing when there was nothing to prune.
void LogPruned(const runtime::PrunedCopies& pruned, runtime::ObsLog& log) {
  if (pruned.busy) {
    log.Write(LOG_INFO,
              "Another program was making or loading an OBS runtime copy, so old copies stay until next time.");
    return;
  }
  if (!pruned.removed.empty()) {
    std::string names;
    for (const fs::path& folder : pruned.removed) {
      names += std::format("{}{}", names.empty() ? "" : ", ", ToUtf8(folder.filename()));
    }
    log.Write(LOG_INFO, std::format("Removed OBS runtime copies no longer needed ({:.1f} MB): {}",
                                    static_cast<double>(pruned.removed_bytes) / (1024 * 1024), names));
  }
  for (const runtime::PrunedCopies::Left& left : pruned.left) {
    log.Write(LOG_INFO, std::format("Left {} for next time: {}", ToUtf8(left.folder), left.why));
  }
  if (pruned.stopped) {
    log.Write(LOG_INFO, "Stopped removing old OBS runtime copies to shut down, so the rest stay until next time.");
  }
}

}  // namespace

ObsBackend::ObsBackend(ObsBackendOptions options) : options_(std::move(options)) {}

ObsBackend::~ObsBackend() {
  // It writes to host_'s log. Asked to stop, it stops once it's done with the
  // file it's on, so quitting doesn't wait for the rest.
  pruner_.request_stop();
  if (pruner_.joinable()) pruner_.join();
  StopChain();
  if (!host_) return;
  const long leaks = host_->Shutdown();
  if (leaks != 0) host_->log().Write(LOG_WARNING, std::format("libobs leaked {} allocation(s).", leaks));
  if (options_.on_shutdown) options_.on_shutdown(leaks, host_->log());
}

ObsCheck ObsBackend::CheckObs(const Settings& settings) {
  ObsCheck check;
  // A picked folder OBS has gone from, or that holds an OBS knobs doesn't
  // support, stays the user's to change: the first run offers to pick
  // another or to look for OBS as with no pick.
  auto install = settings.obs_dir ? runtime::InspectObsInstall(*settings.obs_dir) : FindInstalledObs();
  if (!install) {
    check.message = install.error();
    return check;
  }
  check.install = *install;
  // The same for a picked settings folder. Opening OBS fills only the folder
  // it keeps its settings in, so the errors suggest it only for that one.
  check.config = import::ObsConfigRootFor(install->root, settings.obs_config);
  if (!runtime::IsSupportedObsVersion(install->version)) {
    check.found = ObsFound::kUnsupported;
    check.message = runtime::UnsupportedObsMessage(install->version.ToString());
    return check;
  }
  check.found = ObsFound::kYes;
  return check;
}

Result<import::ActiveObsConfig> ObsBackend::ReadObsConfig(const import::ObsConfigRoot& root) {
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
  if (options_.prune_runtime) {
    // Now the copy in use is known and loaded. Off the core's thread, so the
    // chain doesn't wait for 50 MB of deletes.
    pruner_ = std::jthread([base = host_->app_dirs().RuntimeBase(), keep = host_->copy().root,
                            &log = host_->log()](std::stop_token stop) {
      const runtime::PruneOptions options{.stop_requested = [&stop] { return stop.stop_requested(); }};
      LogPruned(runtime::PruneRuntimeCopies(base, keep, options), log);
    });
  }
  return Ok{};
}

bool ObsBackend::LibobsCanRetry() {
  // A failed start frees obs.dll, which unloads it unless a module libobs
  // opened keeps it loaded. Then libobs's state, and the modules', stayed
  // with it, and starting again would run on them.
  return GetModuleHandleW(runtime::kObsDll.data()) == nullptr;
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
  auto picked =
      options_.pick_by_number ? import::PickMic(result.mics, pick) : import::PickMicByName(result.mics, pick);
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
  peak_ = 0;
  volume_ = api.obs_source_get_volume(chain_->source());
  api.obs_source_add_audio_capture_callback(chain_->source(), OnChainAudio, this);
  return Ok{};
}

void ObsBackend::StopChain() {
  if (!chain_) return;
  host_->api().obs_source_remove_audio_capture_callback(chain_->source(), OnChainAudio, this);
  chain_.reset();
}

uint64_t ObsBackend::ChainPackets() { return packets_.load(std::memory_order_relaxed); }

float ObsBackend::TakeChainPeak() { return peak_.exchange(0, std::memory_order_relaxed) * volume_; }

void ObsBackend::RestartMonitor() {
  if (chain_) chain_->RestartMonitor();
}

void ObsBackend::Log(std::string_view line) {
  if (host_) host_->log().Write(LOG_INFO, line);
}

void ObsBackend::OnChainAudio(void* param, obs_source_t*, const audio_data* audio, bool) {
  auto* backend = static_cast<ObsBackend*>(param);
  backend->packets_.fetch_add(1, std::memory_order_relaxed);
  // Planar float, one plane per channel, as libobs mixes; the planes past
  // the last channel are null.
  float peak = 0;
  for (size_t c = 0; c < MAX_AV_PLANES && audio->data[c]; ++c) {
    const float* samples = reinterpret_cast<const float*>(audio->data[c]);
    for (uint32_t i = 0; i < audio->frames; ++i) peak = std::max(peak, std::fabs(samples[i]));
  }
  float seen = backend->peak_.load(std::memory_order_relaxed);
  while (peak > seen && !backend->peak_.compare_exchange_weak(seen, peak, std::memory_order_relaxed)) {
  }
}

}  // namespace knobs::core
