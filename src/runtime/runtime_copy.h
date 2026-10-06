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

// Works out what knobs needs from an OBS install:
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

// Serializes making, loading and pruning runtime copies across the processes
// in this logon session. Hold it from EnsureRuntimeCopy until obs.dll has
// loaded from the copy, so another process pruning copies can't remove it in
// between. A thread that holds it can take it again, as with any mutex.
class RuntimeCopyLock {
 public:
  // Waits for the lock, or with `wait` false, takes it only if it's free.
  // Best effort: without the mutex, a single process still works, so only
  // pruning checks locked().
  explicit RuntimeCopyLock(bool wait = true);
  ~RuntimeCopyLock();
  RuntimeCopyLock(const RuntimeCopyLock&) = delete;
  RuntimeCopyLock& operator=(const RuntimeCopyLock&) = delete;

  bool locked() const { return locked_; }

 private:
  void* mutex_ = nullptr;  // HANDLE
  bool locked_ = false;
};

// Makes sure <runtime_base>\<version> holds a complete copy of the install's
// runtime files. Refuses unsupported OBS versions before copying anything.
// Reuses the existing copy unless `force` is set or the copy no longer matches
// the install (a file changed, or knobs now needs different modules).
// Otherwise copies into a staging folder, flushes it to disk and swaps it in,
// so an interrupted copy is never used. Copies are serialized across
// processes (RuntimeCopyLock). Replacing a copy fails while a running knobs
// has it loaded.
Result<RuntimeCopy> EnsureRuntimeCopy(const ObsInstall& install,
                                      const std::filesystem::path& runtime_base, bool force);

struct PrunedCopies {
  struct Left {
    std::filesystem::path folder;
    std::string why;
  };
  // Folders removed, by the names they had.
  std::vector<std::filesystem::path> removed;
  uint64_t removed_bytes = 0;
  // Folders left for next time.
  std::vector<Left> left;
  // Another process was making or loading a copy (it held the
  // RuntimeCopyLock), so nothing was looked at.
  bool busy = false;
};

// Removes from runtime_base what knobs no longer needs: copies of OBS other
// than `keep` (the copy this process loaded), copies set aside when they were
// replaced (<version>.old-<n>), and staging folders of copies that were
// interrupted (<version>.partial). Nothing else in runtime_base is touched,
// nor anything outside it, and links are never followed.
//
// A copy another process uses stays, whole. Each folder is renamed aside
// before it's deleted, never emptied in place, and the rename fails while a
// process has a file open in the folder or works in it (ObsRuntime::Load
// makes the copy's bin\64bit the working directory). A loaded DLL doesn't stop
// the rename, so DLLs are checked first: Windows won't open a loaded one for
// writing. Doesn't wait for the RuntimeCopyLock: while another process holds
// it, nothing is pruned.
PrunedCopies PruneRuntimeCopies(const std::filesystem::path& runtime_base,
                                const std::filesystem::path& keep);

}  // namespace knobs::runtime
