// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "runtime/obs_install.h"
#include "runtime/obs_log.h"
#include "runtime/obs_runtime.h"
#include "runtime/obs_session.h"
#include "runtime/runtime_copy.h"
#include "util/app_dirs.h"
#include "util/result.h"

namespace knobs::runtime {

struct HostOptions {
  // The OBS install to use. Default: FindObsInstall().
  std::optional<std::filesystem::path> obs_dir;
  // Names this program's logs in %LocalAppData%\knOBS\logs, e.g. L"harness ".
  // Required: only logs with this prefix are pruned.
  std::wstring log_prefix;
  size_t kept_logs = 20;
  // Also print the log to stderr, debug lines included.
  bool verbose = false;
  VideoMode video = VideoMode::kNone;
};

// libobs up and running: finds the OBS install, makes sure its runtime copy
// is intact, opens a log, loads obs.dll from the copy and starts a session
// with the two modules. Tears down in reverse. ObsSession's threading rule
// applies: create and destroy the host on the same thread.
class ObsHost {
 public:
  static Result<std::unique_ptr<ObsHost>> Start(const HostOptions& options);
  ~ObsHost();
  ObsHost(const ObsHost&) = delete;
  ObsHost& operator=(const ObsHost&) = delete;

  const ObsApi& api() const { return runtime_->api(); }
  ObsSession& session() { return *session_; }
  ObsLog& log() { return *log_; }
  const AppDirs& app_dirs() const { return dirs_; }
  const ObsInstall& install() const { return install_; }
  const RuntimeCopy& copy() const { return copy_; }

  // Shuts libobs down and stops logging to the log. Returns the number of
  // libobs allocations still live; anything but 0 is a leak.
  long Shutdown();

 private:
  ObsHost() = default;

  AppDirs dirs_;
  ObsInstall install_;
  RuntimeCopy copy_;
  // Declared in dependency order, so they're destroyed in reverse.
  std::unique_ptr<ObsLog> log_;
  std::unique_ptr<ObsRuntime> runtime_;
  std::unique_ptr<ObsSession> session_;
  bool log_attached_ = false;
};

}  // namespace knobs::runtime
