// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/app_dirs.h"

#include <windows.h>
#include <shlobj.h>

#include <format>
#include <string>
#include <system_error>

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

std::filesystem::path ExeFolder() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) return {};
    if (length < path.size()) {
      path.resize(length);
      return std::filesystem::path(path).parent_path();
    }
    path.resize(path.size() * 2);
  }
}

bool IsPortable(const std::filesystem::path& exe_folder) {
  std::error_code ec;
  return !exe_folder.empty() && std::filesystem::is_regular_file(exe_folder / kPortableMarker, ec);
}

}  // namespace

AppDirs AppDirsFor(const std::filesystem::path& exe_folder, const std::filesystem::path& roaming_app_data,
                   const std::filesystem::path& local_app_data) {
  if (IsPortable(exe_folder)) {
    const std::filesystem::path data = exe_folder / L"data";
    return AppDirs{data, data, true};
  }
  return AppDirs{roaming_app_data / kDisplayNameW, local_app_data / kDisplayNameW, false};
}

Result<AppDirs> GetAppDirs() {
  const std::filesystem::path exe_folder = ExeFolder();
  // A portable copy needs neither of the user's folders.
  if (IsPortable(exe_folder)) return AppDirsFor(exe_folder, {}, {});
  auto roaming = KnownFolder(FOLDERID_RoamingAppData, "%AppData%");
  if (!roaming) return Error{roaming.error()};
  auto local = KnownFolder(FOLDERID_LocalAppData, "%LocalAppData%");
  if (!local) return Error{local.error()};
  return AppDirsFor(exe_folder, *roaming, *local);
}

Result<std::filesystem::path> UserAppData() { return KnownFolder(FOLDERID_RoamingAppData, "%AppData%"); }

}  // namespace knobs
