// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/runtime_copy.h"

#include <algorithm>
#include <charconv>
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

// Written last, so its presence marks a complete copy.
constexpr wchar_t kManifestName[] = L"knobs-runtime.txt";
constexpr std::string_view kManifestHeader = "knobs-runtime 1";

std::string Describe(const std::error_code& ec) {
  return ec.category() == std::system_category() ? DescribeWinError(static_cast<unsigned long>(ec.value()))
                                                 : ec.message();
}

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

struct ManifestSummary {
  size_t file_count = 0;
  uint64_t total_bytes = 0;
};

// Returns the summary if `root` holds a complete copy for `version` whose
// files all still have their recorded sizes.
std::optional<ManifestSummary> ReadIntactManifest(const fs::path& root, std::string_view version) {
  std::ifstream in(root / kManifestName, std::ios::binary);
  std::string line;
  if (!in || !std::getline(in, line) || line != kManifestHeader) return std::nullopt;

  ManifestSummary summary;
  bool version_matches = false;
  while (std::getline(in, line)) {
    if (line.starts_with("obs-version ")) {
      version_matches = line.substr(12) == version;
    } else if (line.starts_with("file ")) {
      const std::string_view rest = std::string_view(line).substr(5);
      const size_t space = rest.find(' ');
      uint64_t size = 0;
      if (space == std::string_view::npos ||
          std::from_chars(rest.data(), rest.data() + space, size).ec != std::errc()) {
        return std::nullopt;
      }
      std::error_code ec;
      if (fs::file_size(root / FromUtf8(rest.substr(space + 1)), ec) != size || ec) {
        return std::nullopt;
      }
      ++summary.file_count;
      summary.total_bytes += size;
    }
  }
  if (!version_matches || summary.file_count == 0) return std::nullopt;
  return summary;
}

Status WriteManifest(const fs::path& root, const ObsInstall& install,
                     const RuntimeFileSet& file_set) {
  std::ofstream out(root / kManifestName, std::ios::binary | std::ios::trunc);
  out << kManifestHeader << '\n'
      << "obs-version " << install.version.ToString() << '\n'
      << "source " << ToObsPath(install.root) << '\n';
  for (const fs::path& file : file_set.files) {
    std::error_code ec;
    out << "file " << fs::file_size(root / file, ec) << ' ' << ToObsPath(file) << '\n';
  }
  out.close();
  if (!out) return Error{std::format("Couldn't write {}.", ToUtf8(root / kManifestName))};
  return Ok{};
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
  const fs::path target = runtime_base / FromUtf8(version);
  if (!force) {
    if (auto intact = ReadIntactManifest(target, version)) {
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
  }
  if (auto written = WriteManifest(staging, install, *file_set); !written) {
    return Error{written.error()};
  }

  fs::remove_all(target, ec);
  if (ec) {
    return Error{std::format(
        "Couldn't replace the OBS runtime copy in {}. If {} is running, close it and try again. ({})",
        ToUtf8(target), kDisplayName, Describe(ec))};
  }
  fs::rename(staging, target, ec);
  if (ec) {
    return Error{std::format("Couldn't move {} into place: {}", ToUtf8(staging), Describe(ec))};
  }
  return RuntimeCopy{target, false, file_set->files.size(), file_set->total_bytes};
}

}  // namespace knobs::runtime
