// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/win_strings.h"

#include <windows.h>

#include <format>

namespace knobs {

std::string ToUtf8(std::wstring_view wide) {
  if (wide.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                       nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size,
                      nullptr, nullptr);
  return out;
}

std::wstring FromUtf8(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int size =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
  return out;
}

std::string ToObsPath(const std::filesystem::path& path) {
  return ToUtf8(path.generic_wstring());
}

std::string AsciiLower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

std::string DescribeWinError(unsigned long code) {
  wchar_t* buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  std::wstring text(buffer ? buffer : L"", length);
  LocalFree(buffer);
  while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) {
    text.pop_back();
  }
  if (text.empty()) return std::format("Windows error {}", code);
  return std::format("{} (Windows error {})", ToUtf8(text), code);
}

}  // namespace knobs
