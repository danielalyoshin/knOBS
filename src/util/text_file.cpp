// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/text_file.h"

#include <windows.h>

#include <format>
#include <fstream>
#include <sstream>

#include "util/win_strings.h"

namespace knobs {

Result<std::string> ReadText(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return Error{std::format("Couldn't open {}.", ToUtf8(file))};
  std::ostringstream text;
  text << in.rdbuf();
  if (in.bad()) return Error{std::format("Couldn't read {}.", ToUtf8(file))};
  return text.str();
}

Status WriteText(const std::filesystem::path& file, std::string_view text) {
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  std::filesystem::path temporary = file;
  temporary += L".tmp";
  const HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Error{std::format("Couldn't create {}: {}", ToUtf8(temporary), DescribeWinError(GetLastError()))};
  }
  DWORD written = 0;
  const bool wrote = WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                     written == text.size() && FlushFileBuffers(handle);
  const DWORD error = GetLastError();
  CloseHandle(handle);
  if (!wrote) {
    DeleteFileW(temporary.c_str());
    return Error{std::format("Couldn't write {}: {}", ToUtf8(temporary), DescribeWinError(error))};
  }
  if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD move_error = GetLastError();
    DeleteFileW(temporary.c_str());
    return Error{std::format("Couldn't replace {}: {}", ToUtf8(file), DescribeWinError(move_error))};
  }
  return Ok{};
}

}  // namespace knobs
