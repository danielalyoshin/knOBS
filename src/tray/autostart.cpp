// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/autostart.h"

#include <windows.h>

#include <format>
#include <vector>

#include "util/win_strings.h"

namespace knobs::tray {
namespace {

bool TurnedOffInTaskManager(const RunEntry& entry) {
  BYTE data[64] = {};
  DWORD size = sizeof(data);
  const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, entry.approved_key.c_str(), entry.name.c_str(),
                                      RRF_RT_REG_BINARY, nullptr, data, &size);
  return status == ERROR_SUCCESS && size > 0 && (data[0] & 1) != 0;
}

// Deleting what isn't there is fine.
LSTATUS DeleteValue(const std::wstring& key, const std::wstring& name) {
  const LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, key.c_str(), name.c_str());
  return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
}

}  // namespace

bool StartsWithWindows(const RunEntry& entry) {
  DWORD size = 0;
  if (RegGetValueW(HKEY_CURRENT_USER, entry.run_key.c_str(), entry.name.c_str(), RRF_RT_REG_SZ, nullptr, nullptr,
                   &size) != ERROR_SUCCESS) {
    return false;
  }
  std::vector<wchar_t> command(size / sizeof(wchar_t) + 1);
  size = static_cast<DWORD>(command.size() * sizeof(wchar_t));
  if (RegGetValueW(HKEY_CURRENT_USER, entry.run_key.c_str(), entry.name.c_str(), RRF_RT_REG_SZ, nullptr,
                   command.data(), &size) != ERROR_SUCCESS) {
    return false;
  }
  const std::wstring_view saved(command.data());
  const bool same = CompareStringOrdinal(saved.data(), static_cast<int>(saved.size()), entry.command.data(),
                                         static_cast<int>(entry.command.size()), TRUE) == CSTR_EQUAL;
  return same && !TurnedOffInTaskManager(entry);
}

Status SetStartWithWindows(const RunEntry& entry, bool on) {
  LSTATUS status = ERROR_SUCCESS;
  if (on) {
    status = RegSetKeyValueW(HKEY_CURRENT_USER, entry.run_key.c_str(), entry.name.c_str(), REG_SZ,
                             entry.command.c_str(), static_cast<DWORD>((entry.command.size() + 1) * sizeof(wchar_t)));
    if (status == ERROR_SUCCESS && TurnedOffInTaskManager(entry)) status = DeleteValue(entry.approved_key, entry.name);
  } else {
    status = DeleteValue(entry.run_key, entry.name);
    if (status == ERROR_SUCCESS) status = DeleteValue(entry.approved_key, entry.name);
  }
  if (status != ERROR_SUCCESS) {
    return Error{std::format("Couldn't change the sign-in entry in the registry: {}", DescribeWinError(status))};
  }
  return Ok{};
}

}  // namespace knobs::tray
