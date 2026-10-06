// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/obs_backend.h"

#include <windows.h>

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

// OBS's settings where OBS keeps them for `install_root`.
Result<import::ActiveObsConfig> ReadOwnObsConfig(const fs::path& install_root) {
  auto root = import::FindObsConfigRoot(install_root);
  if (!root) return Error{root.error()};
  return import::FindActiveObsConfig(*root);
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
}

}  // namespace

ObsBackend::ObsBackend(ObsBackendOptions options) : options_(std::move(options)) {}

ObsBackend::~ObsBackend() {
  // It writes to host_'s log.
  if (pruner_.joinable()) pruner_.join();
  StopChain();
  if (!host_) return;
  const long leaks = host_->Shutdown();
  if (leaks != 0) host_->log().Write(LOG_WARNING, std::format("libobs leaked {} allocation(s).", leaks));
  if (options_.on_shutdown) options_.on_shutdown(leaks, host_->log());
}

ObsCheck ObsBackend::CheckObs(const Settings& settings) {
  ObsCheck check;
  // A picked folder OBS has gone from stays the user's to change: the first
  // run offers to pick another or to look for OBS as with no pick.
  auto install = settings.obs_dir ? runtime::InspectObsInstall(*settings.obs_dir) : FindInstalledObs();
  if (!install) {
    check.message = install.error();
    return check;
  }
  check.install = *install;
  if (auto own = import::FindObsConfigRoot(install->root)) check.own_config = own->path;
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
  if (!settings.obs_config) return ReadOwnObsConfig(install.root);
  // As for the install: a picked folder that can't be read stays picked
  // until the user picks another or goes back to OBS's own. Opening OBS
  // fills only the folder it keeps its settings in, so the error suggests
  // it only for that one.
  const auto own = import::FindObsConfigRoot(install.root);
  const bool own_folder = own && import::SameFolder(*settings.obs_config, own->path);
  return import::FindActiveObsConfig(import::ObsConfigRootAt(*settings.obs_config), own_folder);
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
    pruner_ = std::thread([base = host_->app_dirs().RuntimeBase(), keep = host_->copy().root, &log = host_->log()] {
      LogPruned(runtime::PruneRuntimeCopies(base, keep), log);
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
