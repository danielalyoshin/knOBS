// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/app_dirs.h"

#include <windows.h>
#include <shlobj.h>

#include <format>

#include "app_info.h"
#include "util/win_strings.h"

namespace knobs {
namespace {

Result<std::filesystem::path> KnownFolder(const KNOWNFOLDERID& id, const char* name) {
  wchar_t* raw = nullptr;
  const HRESULT hr = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
  std::filesystem::path path = SUCCEEDED(hr) ? std::filesystem::path(raw) : std::filesystem::path();
  CoTaskMemFree(raw);
  if (FAILED(hr)) {
    return Error{std::format("Couldn't find the {} folder: {}", name,
                             DescribeWinError(static_cast<unsigned long>(hr)))};
  }
  return path;
}

}  // namespace

Result<AppDirs> GetAppDirs() {
  auto roaming = KnownFolder(FOLDERID_RoamingAppData, "%AppData%");
  if (!roaming) return Error{roaming.error()};
  auto local = KnownFolder(FOLDERID_LocalAppData, "%LocalAppData%");
  if (!local) return Error{local.error()};
  return AppDirs{*roaming / kDisplayNameW, *local / kDisplayNameW};
}

}  // namespace knobs
