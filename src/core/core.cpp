// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/core.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <format>
#include <string>
#include <utility>

#include "audio/device_watch.h"
#include "core/obs_process.h"

namespace knobs::core {

using Clock = Controller::Clock;

Result<std::unique_ptr<Core>> Core::Start(std::unique_ptr<Backend> backend, Observer& observer, CoreOptions options) {
  if (!options.obs_running) {
    auto watch = std::make_shared<ObsWatch>();
    options.obs_running = [watch] { return watch->Running(); };
    if (!options.other_obs) options.other_obs = [watch] { return watch->OtherAccounts(); };
  }
  std::unique_ptr<Core> core(new Core(observer, std::move(options)));
  if (!core->wake_) return Error{"Couldn't create an event for the core's thread."};
  std::promise<Status> started;
  std::future<Status> result = started.get_future();
  core->thread_ = std::thread(&Core::Run, core.get(), std::move(backend), &started);
  const Status status = result.get();
  if (!status) return Error{status.error()};
  return core;
}

Core::Core(Observer& observer, CoreOptions options)
    : observer_(observer), options_(std::move(options)), wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

Core::~Core() {
  if (thread_.joinable()) {
    quit_ = true;
    SetEvent(wake_);
    thread_.join();
  }
  if (wake_) CloseHandle(wake_);
}

void Core::Pause() {
  Post([](Controller& controller, Clock::time_point now) { controller.Pause(now); });
}

void Core::Resume() {
  Post([](Controller& controller, Clock::time_point now) { controller.Resume(now); });
}

void Core::Reimport() {
  Post([](Controller& controller, Clock::time_point now) { controller.Reimport(now); });
}

void Core::Apply(Settings settings) {
  Post([settings = std::move(settings)](Controller& controller, Clock::time_point now) {
    controller.Apply(settings, now);
  });
}

void Core::DevicesChanged() {
  Post([](Controller& controller, Clock::time_point now) { controller.DevicesChanged(now); });
}

void Core::Post(Task task) {
  {
    std::lock_guard lock(mutex_);
    tasks_.push_back(std::move(task));
  }
  SetEvent(wake_);
}

void Core::Wait(std::optional<Clock::time_point> deadline) {
  const HANDLE wake = wake_;
  for (;;) {
    DWORD timeout = INFINITE;
    if (deadline) {
      const Clock::duration left = *deadline - Clock::now();
      if (left <= Clock::duration::zero()) return;
      const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(left).count();
      timeout = static_cast<DWORD>(std::min<long long>(milliseconds, INFINITE - 1));
    }
    const DWORD woke = MsgWaitForMultipleObjectsEx(1, &wake, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (woke != WAIT_OBJECT_0 + 1) return;  // Posted, timed out, or failed.
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
}

void Core::Run(std::unique_ptr<Backend> backend, std::promise<Status>* started) {
  // libobs makes its thread a single-threaded apartment in obs_startup. Doing
  // it first lets the device watch run before libobs does, and after it has
  // shut down.
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) {
    started->set_value(Error{std::format("Couldn't initialize COM for the core (0x{:08X}).", static_cast<uint32_t>(com))});
    return;
  }
  started->set_value(Ok{});  // `started` is gone after this.
  {
    Controller controller(
        *backend, options_.settings, [this](const Snapshot& snapshot) { observer_.OnSnapshot(snapshot); },
        options_.timing);
    std::unique_ptr<audio::DeviceWatch> devices;
    std::string device_watch_error;
    if (options_.watch_devices) {
      auto watch = audio::DeviceWatch::Start([this] { DevicesChanged(); });
      if (watch) {
        devices = std::move(*watch);
      } else {
        device_watch_error = watch.error();
      }
    }

    bool obs_running = options_.obs_running();
    controller.Start(obs_running, Clock::now());
    // Logged once there's a log, which libobs's start opens.
    if (!device_watch_error.empty()) {
      backend->Log(device_watch_error + " Devices that come back won't be noticed until the next re-import.");
    }
    Clock::time_point next_poll = Clock::now() + options_.obs_poll;
    while (!quit_) {
      std::optional<Clock::time_point> deadline = next_poll;
      if (const auto next = controller.NextDeadline(); next && *next < *deadline) deadline = next;
      Wait(deadline);
      std::deque<Task> tasks;
      {
        std::lock_guard lock(mutex_);
        tasks.swap(tasks_);
      }
      for (Task& task : tasks) {
        if (quit_) break;
        task(controller, Clock::now());
      }
      if (quit_) break;
      const Clock::time_point now = Clock::now();
      if (now >= next_poll) {
        const bool running = options_.obs_running();
        if (running != obs_running) {
          obs_running = running;
          controller.SetObsRunning(running, now);
        }
        if (options_.other_obs) controller.SetOtherObs(options_.other_obs(), now);
        next_poll = now + options_.obs_poll;
      }
      controller.Tick(now);
    }
    controller.Stop();
    devices.reset();  // Nothing is posted after this.
    backend.reset();  // Shuts libobs down on the thread that started it.
  }
  CoUninitialize();
}

}  // namespace knobs::core
