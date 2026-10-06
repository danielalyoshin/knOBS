// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/open_obs.h"

#include <windows.h>

#include <format>
#include <string>

#include "runtime/obs_layout.h"
#include "util/win_strings.h"

namespace knobs::tray {

Status OpenObs(const std::filesystem::path& install_root) {
  if (install_root.empty()) return Error{"OBS Studio wasn't found."};
  const std::filesystem::path bin = runtime::BinDir(install_root);
  const std::filesystem::path exe = bin / runtime::kObsExe;
  std::wstring command = std::format(L"\"{}\"", exe.native());
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, bin.c_str(), &startup,
                      &process)) {
    return Error{std::format("Couldn't open OBS ({}): {}", ToUtf8(exe), DescribeWinError(GetLastError()))};
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return Ok{};
}

}  // namespace knobs::tray
