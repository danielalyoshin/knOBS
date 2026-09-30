// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/obs_runtime.h"

#include <windows.h>

#include <format>
#include <string>

#include "app_info.h"
#include "runtime/obs_layout.h"
#include "util/win_strings.h"

namespace knobs::runtime {
namespace {

// Restores the working directory when a load fails, so a failed load doesn't
// leave the process inside a runtime copy (which would also keep that folder
// from being deleted).
class WorkingDirectoryGuard {
 public:
  WorkingDirectoryGuard() {
    std::error_code ec;
    saved_ = std::filesystem::current_path(ec);
  }
  ~WorkingDirectoryGuard() {
    if (armed_ && !saved_.empty()) SetCurrentDirectoryW(saved_.c_str());
  }
  void Keep() { armed_ = false; }

 private:
  std::filesystem::path saved_;
  bool armed_ = true;
};

}  // namespace

Result<std::unique_ptr<ObsRuntime>> ObsRuntime::Load(const std::filesystem::path& runtime_root) {
  const std::filesystem::path bin = BinDir(runtime_root);
  const std::filesystem::path dll = ObsDll(runtime_root);
  std::unique_ptr<ObsRuntime> runtime(new ObsRuntime());
  runtime->root_ = runtime_root;

  runtime->dll_directory_ = AddDllDirectory(bin.c_str());
  if (!runtime->dll_directory_) {
    return Error{std::format("Couldn't add {} to the DLL search path: {}", ToUtf8(bin),
                             DescribeWinError(GetLastError()))};
  }
  WorkingDirectoryGuard working_directory;
  if (!SetCurrentDirectoryW(bin.c_str())) {
    return Error{std::format("Couldn't switch to {}: {}", ToUtf8(bin),
                             DescribeWinError(GetLastError()))};
  }

  HMODULE module = LoadLibraryExW(dll.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!module) {
    return Error{std::format("Couldn't load {}: {}", ToUtf8(dll), DescribeWinError(GetLastError()))};
  }
  runtime->module_ = module;

  // A different obs.dll already in the process would be returned instead.
  wchar_t loaded[MAX_PATH * 2] = {};
  GetModuleFileNameW(module, loaded, static_cast<DWORD>(std::size(loaded)));
  std::error_code ec;
  if (!std::filesystem::equivalent(loaded, dll, ec)) {
    return Error{std::format("Expected obs.dll from {}, but got {}.", ToUtf8(dll),
                             ToUtf8(std::wstring_view(loaded)))};
  }

  const auto missing = ResolveObsApi(runtime->api_, [module](const char* name) {
    return reinterpret_cast<AnyFunction>(GetProcAddress(module, name));
  });
  const ObsApi& api = runtime->api_;
  const std::string version_string =
      api.obs_get_version_string ? api.obs_get_version_string() : "(unknown version)";

  // Version first: outside the range, missing exports are expected and the
  // version is the useful thing to report.
  if (api.obs_get_version) {
    runtime->version_ = ObsVersion::FromLibobs(api.obs_get_version());
    if (!IsSupportedObsVersion(runtime->version_)) {
      return Error{UnsupportedObsMessage(version_string)};
    }
  }
  if (!missing.empty()) {
    std::string names;
    for (std::string_view name : missing) names += std::format("{}{}", names.empty() ? "" : ", ", name);
    return Error{std::format("OBS {} is missing {} function(s) {} needs: {}.", version_string,
                             missing.size(), kDisplayName, names)};
  }
  working_directory.Keep();
  return runtime;
}

ObsRuntime::~ObsRuntime() {
  if (module_) FreeLibrary(static_cast<HMODULE>(module_));
  if (dll_directory_) RemoveDllDirectory(static_cast<DLL_DIRECTORY_COOKIE>(dll_directory_));
}

}  // namespace knobs::runtime
