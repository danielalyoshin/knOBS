// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/obs_runtime.h"
#include "util/result.h"

namespace knobs::runtime {

enum class VideoMode {
  // No obs_reset_video(): no graphics thread, no GPU. libobs only fires
  // sources' activate/show callbacks from the video tick, so they never run,
  // and win-wasapi starts its device-reconnect thread in activate.
  kNone,
  // An 8x8, 1 fps D3D11 canvas that nothing renders. Its graphics thread runs
  // the tick that fires activate callbacks.
  kDummy,
};

struct SessionOptions {
  // Passed to obs_startup(). Created if missing.
  std::filesystem::path module_config_dir;
  uint32_t samples_per_sec = 48000;
  speaker_layout speakers = SPEAKERS_STEREO;
  VideoMode video = VideoMode::kNone;
};

// A started libobs with win-wasapi and obs-filters loaded. libobs is a
// process-wide singleton, so there's at most one of these at a time.
// `runtime` must outlive the session.
//
// Start, Shutdown and the destructor must run on the same thread.
// obs_startup() initializes COM as a single-threaded apartment on its calling
// thread and obs_shutdown() uninitializes COM on its calling thread
// (obs-windows.c, initialize_com). Use a thread that isn't already in a
// multithreaded apartment, or libobs logs a CoInitializeEx error.
class ObsSession {
 public:
  static Result<std::unique_ptr<ObsSession>> Start(const ObsRuntime& runtime,
                                                   const SessionOptions& options);
  // Shuts down if Shutdown() wasn't called.
  ~ObsSession();
  ObsSession(const ObsSession&) = delete;
  ObsSession& operator=(const ObsSession&) = delete;

  // A loaded module by name (see kObsModules), or null.
  obs_module_t* module(std::string_view name) const;

  const SessionOptions& options() const { return options_; }

  // Waits until every source released so far is destroyed. The graphics and
  // audio threads hold references to sources during each tick, so a source
  // can reach its last release on one of them after the caller's release.
  // This mirrors obs_wait_for_destroy_queue() (obs.c): let each running
  // thread finish its tick, then wait for the destroy queue. The libobs
  // function itself returns early when there's no video thread, and
  // obs_shutdown() only waits through it.
  void DrainDestroyQueue();

  // Shuts libobs down. Returns the number of libobs allocations still live
  // afterwards; anything but 0 is a leak.
  long Shutdown();

 private:
  ObsSession(const ObsRuntime& runtime, const SessionOptions& options);

  const ObsRuntime& runtime_;
  SessionOptions options_;
  unsigned long thread_id_ = 0;  // The thread that called obs_startup().
  bool running_ = false;
  bool audio_running_ = false;
  bool video_running_ = false;
  std::vector<std::pair<std::string, obs_module_t*>> modules_;
};

}  // namespace knobs::runtime
