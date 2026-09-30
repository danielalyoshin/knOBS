// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/obs_log.h"

#include <windows.h>

#include <format>

#include "util/win_strings.h"

namespace knobs::runtime {

Result<std::unique_ptr<ObsLog>> ObsLog::Open(const std::filesystem::path& file) {
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  std::unique_ptr<ObsLog> log(new ObsLog());
  log->path_ = file;
  if (_wfopen_s(&log->file_, file.c_str(), L"wb") != 0 || !log->file_) {
    return Error{std::format("Couldn't create the log file {}.", ToUtf8(file))};
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
  if (level <= LOG_WARNING) problems_.push_back(line.substr(0, line.size() - 1));
}

std::vector<std::string> ObsLog::Problems() const {
  std::lock_guard lock(mutex_);
  return problems_;
}

void ObsLog::Handler(int level, const char* format, va_list args, void* param) {
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

}  // namespace knobs::runtime
