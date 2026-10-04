// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdarg>
#include <cstdio>
#include <deque>
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
// file. Thread-safe: libobs logs from many threads. The file stays readable
// by other programs while open, and is appended to if it already exists.
class ObsLog {
 public:
  static Result<std::unique_ptr<ObsLog>> Open(const std::filesystem::path& file);
  ~ObsLog();
  ObsLog(const ObsLog&) = delete;
  ObsLog& operator=(const ObsLog&) = delete;

  // Routes libobs's log here. Attach before obs_startup().
  void Attach(const ObsApi& api);
  // Restores libobs's default handler. Only call this, and only destroy the
  // ObsLog, after obs_shutdown(): libobs swaps its handler without
  // synchronization, so a thread logging during the swap could reach a
  // destroyed ObsLog.
  void Detach(const ObsApi& api);

  // `level` is one of libobs's LOG_* levels.
  void Write(int level, std::string_view message);

  // Also print lines to stderr.
  void set_echo(bool echo) { echo_ = echo; }
  // Keep LOG_DEBUG lines, which libobs produces a lot of.
  void set_verbose(bool verbose) { verbose_ = verbose; }

  // Warning and error lines written so far.
  size_t problem_count() const;
  // The most recent of them, oldest first (at most kRecentProblems).
  std::vector<std::string> RecentProblems() const;
  static constexpr size_t kRecentProblems = 50;

  const std::filesystem::path& path() const { return path_; }

 private:
  ObsLog() = default;
  static void Handler(int level, const char* format, va_list args, void* param);

  mutable std::mutex mutex_;
  std::filesystem::path path_;
  FILE* file_ = nullptr;
  bool echo_ = false;
  bool verbose_ = false;
  size_t problem_count_ = 0;
  std::deque<std::string> recent_problems_;
};

// Deletes the oldest files in `dir` whose names start with `prefix`, keeping
// the newest `keep`.
void PruneLogs(const std::filesystem::path& dir, std::wstring_view prefix, size_t keep);

// Opens "<prefix><local date and time>.txt" in `dir`, first pruning older
// logs with that prefix so that `keep` remain, counting the new one. A run in
// the same second as another gets " (2)" and so on. `prefix` can't be empty.
Result<std::unique_ptr<ObsLog>> OpenNewLog(const std::filesystem::path& dir,
                                           std::wstring_view prefix, size_t keep);

}  // namespace knobs::runtime
