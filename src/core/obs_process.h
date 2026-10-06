// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "runtime/obs_layout.h"

namespace knobs::core {

// What ObsWatch looks for. Tests give names of their own, so that they don't
// look like OBS to a knobs or an OBS that's running.
struct ObsNames {
  // The mutex an installed OBS creates. A portable one adds its settings
  // folder to "OBSStudioPortable" instead.
  std::wstring mutex = L"OBSStudioCore";
  std::wstring exe{runtime::kObsExe};
};

// Tells whether OBS is running in this Windows session, cheaply enough to
// ask every second. Two signals (plan.md, M3 findings):
//  - OBS creates a named mutex as soon as it starts, before its window or any
//    module, and holds it until it has saved its settings
//    (frontend/obs-main.cpp, CheckIfAlreadyRunning). The name is the
//    session's own, and checking for it takes under a microsecond.
//  - A scan of the process list, which takes about a millisecond, every 2 s
//    and when the mutex appears, finds obs64.exe processes in this session.
//    That catches a portable OBS, whose mutex is named after its settings
//    folder. The processes found are then watched through their handles, and
//    one that can't be opened counts until a scan no longer finds it.
// OBS is running while either says so: it has exited once its mutex is gone
// and every OBS process found has ended. Another user's OBS, in a session of
// its own, doesn't count. Not thread-safe.
class ObsWatch {
 public:
  explicit ObsWatch(ObsNames names = {});
  ~ObsWatch();
  ObsWatch(const ObsWatch&) = delete;
  ObsWatch& operator=(const ObsWatch&) = delete;

  bool Running();

 private:
  void Scan(std::chrono::steady_clock::time_point now);

  ObsNames names_;
  unsigned long session_ = 0;     // This process's Windows session.
  std::vector<void*> processes_;  // HANDLEs, opened for SYNCHRONIZE.
  std::vector<unsigned long> process_ids_;
  bool unopened_ = false;  // The last scan found OBS processes it couldn't open.
  bool had_mutex_ = false;
  std::chrono::steady_clock::time_point next_scan_{};
  std::vector<std::byte> buffer_;  // For the process list, kept between scans.
};

}  // namespace knobs::core
