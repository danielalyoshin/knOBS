// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "runtime/obs_install.h"
#include "util/result.h"

namespace knobs::runtime {

struct RuntimeFileSet {
  // Relative to the install root, sorted.
  std::vector<std::filesystem::path> files;
  // Imported DLLs the install doesn't ship (Windows, VC++ runtime), as
  // spelled in the import tables. Not copied.
  std::vector<std::string> system_dlls;
  uint64_t total_bytes = 0;
};

// Works out what knOBS needs from an OBS install:
//  - obs.dll, the graphics module and the two modules, plus every DLL they
//    import, directly or indirectly, that the install ships, and the PDBs
//    next to them;
//  - data\libobs and the two modules' data folders.
// Imports are resolved like the loader will once they run from the copy: the
// importing DLL's own folder, then bin\64bit, then the system.
Result<RuntimeFileSet> PlanRuntimeCopy(const std::filesystem::path& install_root);

struct RuntimeCopy {
  std::filesystem::path root;  // <runtime_base>\<version>, laid out like the install.
  bool reused = false;         // An intact copy was already there.
  size_t file_count = 0;
  uint64_t total_bytes = 0;
};

// Makes sure <runtime_base>\<version> holds a complete copy of the install's
// runtime files. Refuses unsupported OBS versions before copying anything.
// Reuses the existing copy unless `force` is set or the copy no longer matches
// the install (a file changed, or knOBS now needs different modules).
// Otherwise copies into a staging folder, flushes it to disk and swaps it in,
// so an interrupted copy is never used. Copies are serialized across
// processes. Replacing a copy fails while a running knOBS has it loaded.
Result<RuntimeCopy> EnsureRuntimeCopy(const ObsInstall& install,
                                      const std::filesystem::path& runtime_base, bool force);

}  // namespace knobs::runtime
