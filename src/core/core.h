// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/backend.h"
#include "core/controller.h"
#include "core/state.h"
#include "util/result.h"

// knobs's always-on core: what the tray app runs, minus the tray.
namespace knobs::core {

struct CoreOptions {
  Settings settings;
  // Whether OBS is running. Called on the core's thread. Default: an
  // ObsWatch.
  std::function<bool()> obs_running;
  // OBS running in other Windows sessions, asked right after obs_running.
  // Default: the same ObsWatch's; none when obs_running is given without it.
  std::function<std::vector<OtherObs>()> other_obs;
  // How often to ask. OBS takes more than a second and a half from starting to
  // loading its audio sources (docs/design.md, Following OBS), so a second is
  // soon enough to make way for it.
  std::chrono::milliseconds obs_poll{1000};
  // Listen for audio devices coming and going (audio::DeviceWatch).
  bool watch_devices = true;
  Controller::Timing timing;
};

// Runs a Controller on a thread of its own, which also runs libobs through
// the backend, from start to shutdown, as libobs requires. The thread feeds
// the controller OBS starting and exiting, device notifications, timers and
// the commands below, and the observer hears every change of state on it.
class Core {
 public:
  // Starts the thread and returns; the observer hears kStarting, then the
  // outcome. The observer must outlive the core.
  static Result<std::unique_ptr<Core>> Start(std::unique_ptr<Backend> backend, Observer& observer,
                                             CoreOptions options);
  // Stops the chain and libobs, on the core's thread, and waits for it.
  ~Core();
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;

  // Commands, from any thread, the observer's callback included. They return
  // at once; the observer hears what comes of them.
  void Pause();
  void Resume();
  void Reimport();
  void Apply(Settings settings);
  // The audio devices may have changed, as a device watch reports it. For a
  // watch other than the core's own (CoreOptions::watch_devices off).
  void DevicesChanged();

 private:
  using Task = std::function<void(Controller& controller, Controller::Clock::time_point now)>;

  Core(Observer& observer, CoreOptions options);
  void Post(Task task);
  void Run(std::unique_ptr<Backend> backend, std::promise<Status>* started);
  // Waits for a task or `deadline`, pumping window messages meanwhile: the
  // thread is a COM single-threaded apartment, as libobs makes it.
  void Wait(std::optional<Controller::Clock::time_point> deadline);

  Observer& observer_;
  CoreOptions options_;
  std::mutex mutex_;  // Guards tasks_.
  std::deque<Task> tasks_;
  void* wake_ = nullptr;  // An auto-reset event (HANDLE), set when a task is posted.
  std::atomic<bool> quit_ = false;
  std::thread thread_;
};

}  // namespace knobs::core
