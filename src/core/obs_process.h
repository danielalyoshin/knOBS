// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/state.h"
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

// A process with OBS's name, as a scan of the process list finds it.
struct ObsProcess {
  unsigned long id = 0;
  unsigned long session = 0;  // Its Windows session.
};

// Tells whether OBS is running in this Windows session, cheaply enough to ask
// every second. Two signals (docs/design.md, Following OBS):
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
// and every OBS process found has ended. OBS in another session, another
// user's or this user's signed in again, doesn't count, but the scan notes it
// (Others): it can send audio to the same cable. A scan that can't read the
// process list changes nothing. Not thread-safe.
class ObsWatch {
 public:
  // What it asks Windows, for tests to stand in for. Empty asks Windows.
  struct System {
    // The processes named `exe`, in every session, or nullopt if the process
    // list can't be read.
    std::function<std::optional<std::vector<ObsProcess>>(std::wstring_view exe)> processes;
    // The account signed in to a session, as DOMAIN\name, or "" where it
    // can't be read.
    std::function<std::string(unsigned long session)> account;
  };

  explicit ObsWatch(ObsNames names = {}, System system = {});
  ~ObsWatch();
  ObsWatch(const ObsWatch&) = delete;
  ObsWatch& operator=(const ObsWatch&) = delete;

  bool Running();
  // OBS in other Windows sessions, as of the last scan, by session. Not
  // session 0, which runs services: no one signs in to it.
  const std::vector<OtherObs>& Others() const { return others_; }

 private:
  void Scan(std::chrono::steady_clock::time_point now);

  ObsNames names_;
  System system_;
  unsigned long session_ = 0;     // This process's Windows session.
  std::string account_;           // Its account, as System::account names it.
  std::vector<void*> processes_;  // HANDLEs, opened for SYNCHRONIZE.
  std::vector<unsigned long> process_ids_;
  bool unopened_ = false;  // The last scan found OBS processes it couldn't open.
  std::vector<unsigned long> other_sessions_;  // Sessions of other OBS processes, sorted.
  std::vector<OtherObs> others_;
  bool had_mutex_ = false;
  std::chrono::steady_clock::time_point next_scan_{};
};

}  // namespace knobs::core
