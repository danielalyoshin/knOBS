// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/obs_log.h"

#include <windows.h>
#include <share.h>

#include <algorithm>
#include <format>

#include "util/win_strings.h"

namespace knobs::runtime {

Result<std::unique_ptr<ObsLog>> ObsLog::Open(const std::filesystem::path& file) {
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  std::unique_ptr<ObsLog> log(new ObsLog());
  log->path_ = file;
  // _SH_DENYWR lets other programs read the log while knOBS runs; the
  // _wfopen_s default denies them all access.
  log->file_ = _wfsopen(file.c_str(), L"ab", _SH_DENYWR);
  if (!log->file_) {
    return Error{std::format("Couldn't open the log file {}.", ToUtf8(file))};
  }
  return log;
}

ObsLog::~ObsLog() {
  if (file_) fclose(file_);
}

void ObsLog::Attach(const ObsApi& api) { api.base_set_log_handler(&ObsLog::Handler, this); }

void ObsLog::Detach(const ObsApi& api) { api.base_set_log_handler(nullptr, nullptr); }

void ObsLog::Write(int level, std::string_view message) {
  if (level >= LOG_DEBUG && !verbose_) return;

  SYSTEMTIME now;
  GetLocalTime(&now);
  const char* label = level <= LOG_ERROR ? "error: " : level <= LOG_WARNING ? "warning: " : "";
  const std::string line = std::format("{:02}:{:02}:{:02}.{:03}: {}{}\n", now.wHour, now.wMinute,
                                       now.wSecond, now.wMilliseconds, label, message);

  std::lock_guard lock(mutex_);
  fwrite(line.data(), 1, line.size(), file_);
  fflush(file_);
  if (echo_) fwrite(line.data(), 1, line.size(), stderr);
  if (level <= LOG_WARNING) {
    ++problem_count_;
    if (recent_problems_.size() == kRecentProblems) recent_problems_.pop_front();
    recent_problems_.push_back(line.substr(0, line.size() - 1));
  }
}

size_t ObsLog::problem_count() const {
  std::lock_guard lock(mutex_);
  return problem_count_;
}

std::vector<std::string> ObsLog::RecentProblems() const {
  std::lock_guard lock(mutex_);
  return {recent_problems_.begin(), recent_problems_.end()};
}

void ObsLog::Handler(int level, const char* format, va_list args, void* param) {
  // libobs sets the handler and its parameter as two unsynchronized stores, so
  // a thread logging during Attach or Detach can see a null parameter.
  if (!param) return;
  char buffer[4096];
  va_list retry;
  va_copy(retry, args);
  const int length = vsnprintf(buffer, sizeof(buffer), format, args);
  std::string message;
  if (length < 0) {
    message = format;
  } else if (static_cast<size_t>(length) < sizeof(buffer)) {
    message.assign(buffer, static_cast<size_t>(length));
  } else {
    message.resize(static_cast<size_t>(length));
    vsnprintf(message.data(), message.size() + 1, format, retry);
  }
  va_end(retry);
  static_cast<ObsLog*>(param)->Write(level, message);
}

void PruneLogs(const std::filesystem::path& dir, std::wstring_view prefix, size_t keep) {
  std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> logs;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec) && entry.path().filename().native().starts_with(prefix)) {
      logs.emplace_back(entry.last_write_time(ec), entry.path());
    }
  }
  if (logs.size() <= keep) return;
  std::sort(logs.begin(), logs.end());
  for (size_t i = 0; i + keep < logs.size(); ++i) std::filesystem::remove(logs[i].second, ec);
}

}  // namespace knobs::runtime
