// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/runtime_copy.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>

#include "app_info.h"
#include "runtime/obs_layout.h"
#include "runtime/pe_imports.h"
#include "util/win_strings.h"

namespace knobs::runtime {
namespace {

namespace fs = std::filesystem;

// Written last, so its presence marks a complete copy. Records each file's
// size and the install file's timestamp, so a copy is only reused while the
// install still has exactly the files it was made from. OBS drops beta and RC
// suffixes from obs.dll's version resource, so the version alone can't tell
// 32.3.0-beta1 from 32.3.0.
constexpr wchar_t kManifestName[] = L"knobs-runtime.txt";
constexpr std::string_view kManifestHeader = "knobs-runtime 2";

std::string Describe(const std::error_code& ec) {
  return ec.category() == std::system_category() ? DescribeWinError(static_cast<unsigned long>(ec.value()))
                                                 : ec.message();
}

// The binaries the file set is built from. Recorded in the manifest so a
// knOBS that loads more modules doesn't reuse a copy made without them.
std::string RootsLine() {
  std::string roots(kGraphicsModule);
  for (std::string_view module : kObsModules) roots += std::format(" {}", module);
  return roots;
}

int64_t WriteTime(const fs::path& file, std::error_code& ec) {
  return fs::last_write_time(file, ec).time_since_epoch().count();
}

// Forces a file's data to disk, so the manifest can't reach the disk before
// the files it vouches for.
bool FlushToDisk(const fs::path& file) {
  HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  const bool flushed = FlushFileBuffers(handle);
  CloseHandle(handle);
  return flushed;
}

// Serializes runtime copies across processes in this logon session, so two
// knOBS instances starting together can't clobber each other's staging folder.
class CopyLock {
 public:
  CopyLock() {
    const std::wstring name = std::format(L"Local\\{}-runtime-copy", kDisplayNameW);
    mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
    // Best effort: without the mutex, copying still works for a single instance.
    if (mutex_) {
      const DWORD result = WaitForSingleObject(mutex_, INFINITE);
      locked_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
  }
  ~CopyLock() {
    if (locked_) ReleaseMutex(mutex_);
    if (mutex_) CloseHandle(mutex_);
  }
  CopyLock(const CopyLock&) = delete;
  CopyLock& operator=(const CopyLock&) = delete;

 private:
  HANDLE mutex_ = nullptr;
  bool locked_ = false;
};

// DLLs in `dir`, keyed by lowercase file name.
std::map<std::string, fs::path> ListDlls(const fs::path& dir) {
  std::map<std::string, fs::path> dlls;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec) && AsciiLower(ToUtf8(entry.path().extension())) == ".dll") {
      dlls.emplace(AsciiLower(ToUtf8(entry.path().filename())), entry.path());
    }
  }
  return dlls;
}

// API set names (api-ms-*, ext-ms-*) are resolved by the loader, not files.
bool IsApiSet(std::string_view lower_name) {
  return lower_name.starts_with("api-ms-") || lower_name.starts_with("ext-ms-");
}

// Splits "<number> <rest>". Returns false if there's no valid number.
template <typename Number>
bool SplitNumber(std::string_view& text, Number& number) {
  const size_t space = text.find(' ');
  if (space == std::string_view::npos ||
      std::from_chars(text.data(), text.data() + space, number).ec != std::errc()) {
    return false;
  }
  text.remove_prefix(space + 1);
  return true;
}

struct ManifestSummary {
  size_t file_count = 0;
  uint64_t total_bytes = 0;
};

// Returns the summary if `copy_root` holds a complete copy of `install`: made
// for its version and the current module list, every file still whole, and
// every install file unchanged since the copy was made.
std::optional<ManifestSummary> ReadIntactManifest(const fs::path& copy_root, const ObsInstall& install) {
  std::ifstream in(copy_root / kManifestName, std::ios::binary);
  std::string line;
  if (!in || !std::getline(in, line) || line != kManifestHeader) return std::nullopt;

  ManifestSummary summary;
  bool version_matches = false;
  bool roots_match = false;
  while (std::getline(in, line)) {
    std::string_view rest = line;
    if (rest.starts_with("obs-version ")) {
      version_matches = rest.substr(12) == install.version.ToString();
    } else if (rest.starts_with("roots ")) {
      roots_match = rest.substr(6) == RootsLine();
    } else if (rest.starts_with("file ")) {
      rest.remove_prefix(5);
      uint64_t size = 0;
      int64_t write_time = 0;
      if (!SplitNumber(rest, size) || !SplitNumber(rest, write_time)) return std::nullopt;
      const fs::path file = FromUtf8(rest);
      std::error_code ec;
      if (fs::file_size(copy_root / file, ec) != size || ec) return std::nullopt;
      if (fs::file_size(install.root / file, ec) != size || ec) return std::nullopt;
      if (WriteTime(install.root / file, ec) != write_time || ec) return std::nullopt;
      ++summary.file_count;
      summary.total_bytes += size;
    }
  }
  if (!version_matches || !roots_match || summary.file_count == 0) return std::nullopt;
  return summary;
}

Status WriteManifest(const fs::path& copy_root, const ObsInstall& install,
                     const RuntimeFileSet& file_set) {
  const fs::path manifest = copy_root / kManifestName;
  std::ofstream out(manifest, std::ios::binary | std::ios::trunc);
  out << kManifestHeader << '\n'
      << "obs-version " << install.version.ToString() << '\n'
      << "roots " << RootsLine() << '\n'
      << "source " << ToObsPath(install.root) << '\n';
  for (const fs::path& file : file_set.files) {
    std::error_code size_error;
    std::error_code time_error;
    const uint64_t size = fs::file_size(install.root / file, size_error);
    const int64_t write_time = WriteTime(install.root / file, time_error);
    if (size_error || time_error) {
      return Error{std::format("Couldn't read {}: {}", ToUtf8(install.root / file),
                               Describe(size_error ? size_error : time_error))};
    }
    out << "file " << size << ' ' << write_time << ' ' << ToObsPath(file) << '\n';
  }
  out.close();
  if (!out || !FlushToDisk(manifest)) {
    return Error{std::format("Couldn't write {}.", ToUtf8(manifest))};
  }
  return Ok{};
}

// Deletes <version>.old-* folders left by earlier swaps. Ones still in use
// by a running knOBS stay until a later copy.
void RemoveRetiredCopies(const fs::path& runtime_base, const std::string& version) {
  const std::string prefix = AsciiLower(version + ".old-");
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(runtime_base, ec)) {
    if (AsciiLower(ToUtf8(entry.path().filename())).starts_with(prefix)) {
      std::error_code ignored;
      fs::remove_all(entry.path(), ignored);
    }
  }
}

}  // namespace

Result<RuntimeFileSet> PlanRuntimeCopy(const fs::path& install_root) {
  const fs::path bin = BinDir(install_root);
  std::map<fs::path, std::map<std::string, fs::path>> dlls_by_dir = {
      {bin, ListDlls(bin)},
      {PluginBinDir(install_root), ListDlls(PluginBinDir(install_root))},
  };
  auto find_dll = [&](const fs::path& dir, const std::string& lower_name) -> const fs::path* {
    const auto dir_it = dlls_by_dir.find(dir);
    if (dir_it == dlls_by_dir.end()) return nullptr;
    const auto it = dir_it->second.find(lower_name);
    return it == dir_it->second.end() ? nullptr : &it->second;
  };

  std::vector<fs::path> pending = {ObsDll(install_root),
                                   bin / (fs::path(kGraphicsModule) += L".dll")};
  for (std::string_view module : kObsModules) pending.push_back(PluginDll(install_root, module));

  std::set<fs::path> binaries;
  std::map<std::string, std::string> system_dlls;  // Lowercase name -> as spelled.
  while (!pending.empty()) {
    const fs::path binary = std::move(pending.back());
    pending.pop_back();
    if (!binaries.insert(binary).second) continue;

    auto imports = ReadDllImports(binary);
    if (!imports) return Error{imports.error()};
    for (const std::string& name : *imports) {
      const std::string lower = AsciiLower(name);
      if (IsApiSet(lower)) continue;
      const fs::path* local = find_dll(binary.parent_path(), lower);
      if (!local) local = find_dll(bin, lower);
      if (local) {
        pending.push_back(*local);
      } else {
        system_dlls.emplace(lower, name);
      }
    }
  }

  RuntimeFileSet file_set;
  std::error_code ec;
  auto add = [&](const fs::path& file) -> Status {
    const uint64_t size = fs::file_size(file, ec);
    if (ec) return Error{std::format("Couldn't read {}: {}", ToUtf8(file), Describe(ec))};
    file_set.files.push_back(file.lexically_relative(install_root));
    file_set.total_bytes += size;
    return Ok{};
  };

  for (const fs::path& binary : binaries) {
    if (auto added = add(binary); !added) return Error{added.error()};
    // OBS ships PDBs; keeping them next to the copies makes crashes debuggable.
    fs::path pdb = binary;
    pdb.replace_extension(L".pdb");
    if (fs::exists(pdb, ec)) {
      if (auto added = add(pdb); !added) return Error{added.error()};
    }
  }

  std::vector<fs::path> data_dirs = {LibobsDataDir(install_root)};
  for (std::string_view module : kObsModules) data_dirs.push_back(PluginDataDir(install_root, module));
  for (const fs::path& dir : data_dirs) {
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      if (auto added = add(it->path()); !added) return Error{added.error()};
    }
    if (ec) return Error{std::format("Couldn't read {}: {}", ToUtf8(dir), Describe(ec))};
  }

  std::sort(file_set.files.begin(), file_set.files.end());
  for (auto& [lower, name] : system_dlls) file_set.system_dlls.push_back(std::move(name));
  return file_set;
}

Result<RuntimeCopy> EnsureRuntimeCopy(const ObsInstall& install, const fs::path& runtime_base,
                                      bool force) {
  const std::string version = install.version.ToString();
  if (!IsSupportedObsVersion(install.version)) return Error{UnsupportedObsMessage(version)};

  const CopyLock lock;
  const fs::path target = runtime_base / FromUtf8(version);
  if (!force) {
    if (auto intact = ReadIntactManifest(target, install)) {
      return RuntimeCopy{target, true, intact->file_count, intact->total_bytes};
    }
  }

  auto file_set = PlanRuntimeCopy(install.root);
  if (!file_set) return Error{file_set.error()};

  const fs::path staging = runtime_base / FromUtf8(version + ".partial");
  std::error_code ec;
  fs::remove_all(staging, ec);
  if (ec) return Error{std::format("Couldn't clear {}: {}", ToUtf8(staging), Describe(ec))};

  for (const fs::path& file : file_set->files) {
    const fs::path from = install.root / file;
    const fs::path to = staging / file;
    fs::create_directories(to.parent_path(), ec);
    if (!ec) fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      return Error{std::format("Couldn't copy {} to {}: {}", ToUtf8(from), ToUtf8(to), Describe(ec))};
    }
    // The copy is knOBS's own; a read-only attribute carried over from the
    // install would block flushing it now and replacing it later.
    const DWORD attributes = GetFileAttributesW(to.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)) {
      SetFileAttributesW(to.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    }
    if (!FlushToDisk(to)) {
      return Error{std::format("Couldn't write {}: {}", ToUtf8(to), DescribeWinError(GetLastError()))};
    }
  }
  if (auto written = WriteManifest(staging, install, *file_set); !written) {
    return Error{written.error()};
  }

  // Move the old copy aside rather than deleting it: the rename fails as a
  // whole while a running knOBS has it loaded, instead of half-deleting it.
  RemoveRetiredCopies(runtime_base, version);
  const bool replacing = fs::exists(target, ec);
  const fs::path retired =
      runtime_base / FromUtf8(std::format("{}.old-{}", version, GetTickCount64()));
  if (replacing) {
    fs::rename(target, retired, ec);
    if (ec) {
      return Error{std::format(
          "Couldn't replace the OBS runtime copy in {}. If {} is running, close it and try again. ({})",
          ToUtf8(target), kDisplayName, Describe(ec))};
    }
  }
  // Antivirus and the search indexer briefly hold handles to freshly written
  // files, which makes renaming their folder fail for a moment.
  for (int attempt = 1; attempt <= 5; ++attempt) {
    fs::rename(staging, target, ec);
    if (!ec) break;
    Sleep(200 * attempt);
  }
  if (ec) {
    std::error_code ignored;
    if (replacing) fs::rename(retired, target, ignored);
    return Error{std::format("Couldn't move {} into place: {}", ToUtf8(staging), Describe(ec))};
  }
  RemoveRetiredCopies(runtime_base, version);
  return RuntimeCopy{target, false, file_set->files.size(), file_set->total_bytes};
}

}  // namespace knobs::runtime
