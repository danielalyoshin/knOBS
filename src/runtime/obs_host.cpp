// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/obs_host.h"

#include <format>

#include "app_info.h"
#include "util/win_strings.h"

namespace knobs::runtime {

Result<std::unique_ptr<ObsHost>> ObsHost::Start(const HostOptions& options) {
  auto dirs = GetAppDirs();
  if (!dirs) return Error{dirs.error()};
  auto install = options.install    ? Result<ObsInstall>(*options.install)
                 : options.obs_dir ? InspectObsInstall(*options.obs_dir)
                                   : FindObsInstall();
  if (!install) return Error{install.error()};
  auto copy = EnsureRuntimeCopy(*install, dirs->RuntimeBase(), false);
  if (!copy) return Error{copy.error()};
  auto log = OpenNewLog(dirs->Logs(), options.log_prefix, options.kept_logs);
  if (!log) return Error{log.error()};
  (*log)->set_echo(options.verbose);
  (*log)->set_verbose(options.verbose);
  auto runtime = ObsRuntime::Load(copy->root);
  if (!runtime) return Error{runtime.error()};

  std::unique_ptr<ObsHost> host(new ObsHost());
  host->dirs_ = *dirs;
  host->install_ = *install;
  host->copy_ = *copy;
  host->log_ = std::move(*log);
  host->runtime_ = std::move(*runtime);
  host->log_->Attach(host->api());
  host->log_attached_ = true;
  host->log_->Write(LOG_INFO, std::format("{}: OBS {}, runtime {}", kDisplayName,
                                          host->install_.version.ToString(), ToUtf8(copy->root)));

  SessionOptions session_options;
  session_options.module_config_dir = dirs->ModuleConfig();
  session_options.video = options.video;
  session_options.samples_per_sec = options.samples_per_sec;
  session_options.speakers = options.speakers;
  auto session = ObsSession::Start(*host->runtime_, session_options);
  // The host and its log go away with this return, so say where the log is.
  if (!session) return Error{std::format("{} Log: {}", session.error(), ToUtf8(host->log_->path()))};
  host->session_ = std::move(*session);
  return host;
}

ObsHost::~ObsHost() { Shutdown(); }

long ObsHost::Shutdown() {
  const long leaks = session_ ? session_->Shutdown() : 0;
  // Only after obs_shutdown(); see ObsLog::Detach.
  if (log_attached_) {
    log_->Detach(api());
    log_attached_ = false;
  }
  return leaks;
}

}  // namespace knobs::runtime
