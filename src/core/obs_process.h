// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <vector>

namespace knobs::core {

// Tells whether OBS is running, cheaply enough to ask every second. Two
// signals (plan.md, M3 findings):
//  - OBS creates a named mutex as soon as it starts, before its window or any
//    module, and holds it until it has saved its settings
//    (frontend/obs-main.cpp, CheckIfAlreadyRunning). Checking for it takes
//    under a microsecond.
//  - A scan of the process list, which takes about a millisecond, every 2 s
//    and when the mutex appears, finds obs64.exe processes. That catches a
//    portable OBS, whose mutex is named after its settings folder. The
//    processes found are then watched through their handles.
// OBS is running while either says so: it has exited once its mutex is gone
// and every OBS process found has ended. Not thread-safe.
class ObsWatch {
 public:
  ObsWatch() = default;
  ~ObsWatch();
  ObsWatch(const ObsWatch&) = delete;
  ObsWatch& operator=(const ObsWatch&) = delete;

  bool Running();

 private:
  void Scan(std::chrono::steady_clock::time_point now);

  std::vector<void*> processes_;  // HANDLEs, opened for SYNCHRONIZE.
  std::vector<unsigned long> process_ids_;
  bool unopened_ = false;  // The last scan found OBS processes it couldn't open.
  bool had_mutex_ = false;
  std::chrono::steady_clock::time_point next_scan_{};
  std::vector<std::byte> buffer_;  // For the process list, kept between scans.
};

}  // namespace knobs::core
