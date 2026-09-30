// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/obs_api.h"
#include "util/result.h"

namespace knobs::runtime {

// Writes libobs's log (via base_set_log_handler) and knOBS's own lines to one
// file. Thread-safe: libobs logs from many threads.
class ObsLog {
 public:
  static Result<std::unique_ptr<ObsLog>> Open(const std::filesystem::path& file);
  ~ObsLog();
  ObsLog(const ObsLog&) = delete;
  ObsLog& operator=(const ObsLog&) = delete;

  // Routes libobs's log here. Detach before destroying this object.
  void Attach(const ObsApi& api);
  // Restores libobs's default handler.
  void Detach(const ObsApi& api);

  // `level` is one of libobs's LOG_* levels.
  void Write(int level, std::string_view message);

  // Also print lines to stderr.
  void set_echo(bool echo) { echo_ = echo; }
  // Keep LOG_DEBUG lines, which libobs produces a lot of.
  void set_verbose(bool verbose) { verbose_ = verbose; }

  // Warning and error lines written so far.
  std::vector<std::string> Problems() const;
  const std::filesystem::path& path() const { return path_; }

 private:
  ObsLog() = default;
  static void Handler(int level, const char* format, va_list args, void* param);

  mutable std::mutex mutex_;
  std::filesystem::path path_;
  FILE* file_ = nullptr;
  bool echo_ = false;
  bool verbose_ = false;
  std::vector<std::string> problems_;
};

}  // namespace knobs::runtime
