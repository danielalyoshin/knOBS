// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <memory>

#include "runtime/obs_api.h"
#include "runtime/obs_version.h"
#include "util/result.h"

namespace knobs::runtime {

// obs.dll loaded from a runtime copy, with its exports resolved.
//
// Unloading is partial by design of libobs: it never unloads module DLLs
// (obs-module.c, free_module), and they keep obs.dll loaded. Switching to
// another OBS version needs a fresh process.
class ObsRuntime {
 public:
  // Loads <runtime_root>\bin\64bit\obs.dll and checks its version. Changes
  // process-wide state so libobs finds everything in the copy:
  //  - DLL search is limited to the system and bin\64bit
  //    (SetDefaultDllDirectories + AddDllDirectory), so nothing can resolve
  //    from the OBS install or PATH. libobs loads the graphics module by bare
  //    name, which needs the AddDllDirectory entry.
  //  - The working directory becomes bin\64bit: libobs looks for its data
  //    files in "../../data/libobs/" relative to it (obs-windows.c).
  static Result<std::unique_ptr<ObsRuntime>> Load(const std::filesystem::path& runtime_root);

  ~ObsRuntime();
  ObsRuntime(const ObsRuntime&) = delete;
  ObsRuntime& operator=(const ObsRuntime&) = delete;

  const ObsApi& api() const { return api_; }
  const ObsVersion& version() const { return version_; }
  const std::filesystem::path& root() const { return root_; }

 private:
  ObsRuntime() = default;

  ObsApi api_;
  ObsVersion version_;
  std::filesystem::path root_;
  void* module_ = nullptr;          // HMODULE
  void* dll_directory_ = nullptr;   // DLL_DIRECTORY_COOKIE
};

}  // namespace knobs::runtime
