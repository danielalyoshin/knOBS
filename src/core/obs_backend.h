// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "audio/live_chain.h"
#include "core/backend.h"
#include "runtime/obs_host.h"

namespace knobs::core {

struct ObsBackendOptions {
  // Names the log in %LocalAppData%\knobs\logs (runtime::HostOptions).
  std::wstring log_prefix;
  // Also print the log to stderr, debug lines included.
  bool verbose = false;
  // Called once libobs has shut down, with the libobs allocations still live
  // (anything but 0 is a leak) and the log. For the dev tools.
  std::function<void(long leaks, const runtime::ObsLog& log)> on_shutdown;
  // Read Settings::mic as the tools' --pick: a number or a name
  // (import::PickMic). Otherwise it's a name, as the tray saves it.
  bool pick_by_number = false;
  // Once libobs has started, remove the runtime copies and leftovers no
  // process uses, on a thread of its own (runtime::PruneRuntimeCopies). The
  // app turns it on. Off, tests and tools leave %LocalAppData%\knobs\runtime
  // as it is.
  bool prune_runtime = false;
};

// Sums up the peaks of the packets a chain filtered, oldest first. The
// typical level leaves out the loudest tenth; the latest is the loudest of
// the last 20 (200 ms of win-wasapi's).
ChainLevel SummarizeChainLevel(std::vector<float> packet_peaks);

// The real thing: the user's OBS install and settings, libobs from its
// runtime copy (runtime::ObsHost), and the chain (audio::LiveChain). libobs
// starts on the thread that calls StartLibobs, and the destructor, which
// shuts it down, has to run on that thread too.
class ObsBackend : public Backend {
 public:
  explicit ObsBackend(ObsBackendOptions options);
  // Stops pruning, stops the chain and shuts libobs down.
  ~ObsBackend() override;

  ObsCheck CheckObs(const Settings& settings) override;
  Result<import::ActiveObsConfig> ReadObsConfig(const import::ObsConfigRoot& root) override;
  Status StartLibobs(const runtime::ObsInstall& install, const import::ProfileAudio& audio) override;
  bool LibobsCanRetry() override;
  Result<MicImport> ImportMic(const import::ActiveObsConfig& config, std::string_view pick) override;
  audio::Endpoints ListDevices() override;
  Status StartChain(const ChainPlan& plan) override;
  void StopChain() override;
  uint64_t ChainPackets() override;
  ChainLevel TakeChainLevel() override;
  void RestartMonitor() override;
  void Log(std::string_view line) override;

 protected:
  // Null until StartLibobs succeeds.
  runtime::ObsHost* host() { return host_.get(); }

 private:
  // A capture callback on the chain: counts what it filters and keeps each
  // packet's peak. It only reads the samples.
  static void OnChainAudio(void* param, obs_source_t* source, const audio_data* audio, bool muted);

  ObsBackendOptions options_;
  std::unique_ptr<runtime::ObsHost> host_;
  std::unique_ptr<audio::LiveChain> chain_;
  std::atomic<uint64_t> packets_ = 0;
  std::mutex level_mutex_;
  // Each packet's peak since the last TakeChainLevel, oldest first, before
  // the volume. Guarded by level_mutex_.
  std::vector<float> packet_peaks_;
  // TakeChainLevel's, swapped with packet_peaks_ so the callback doesn't
  // allocate once both have grown.
  std::vector<float> taken_peaks_;
  // The chain's volume, which the monitor applies after the capture
  // callbacks. knobs never changes it while the chain runs.
  float volume_ = 1;
  // Prunes runtime copies, writing to host_'s log. Only the file system and
  // the log, which is thread-safe. Stopped and joined first thing in the
  // destructor.
  std::jthread pruner_;
};

}  // namespace knobs::core
