// SPDX-License-Identifier: GPL-2.0-or-later
//
// Unit tests for the parts of the runtime layer that don't need OBS
// installed. A deliberately tiny harness: TEST registers, CHECK records.

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/obs_api.h"
#include "runtime/obs_install.h"
#include "runtime/obs_version.h"
#include "runtime/pe_imports.h"
#include "runtime/runtime_copy.h"
#include "util/win_strings.h"

namespace {

using namespace knobs;
using namespace knobs::runtime;
namespace fs = std::filesystem;

struct TestCase {
  const char* name;
  void (*run)();
};

std::vector<TestCase>& Tests() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(const char* name, void (*run)()) { Tests().push_back({name, run}); }
};

int g_failures = 0;

#define TEST(name)                                     \
  void name();                                         \
  const Registrar name##_registrar(#name, &name);      \
  void name()

#define CHECK(condition)                                                              \
  do {                                                                                \
    if (!(condition)) {                                                               \
      std::fprintf(stderr, "%s(%d): CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                                                   \
    }                                                                                 \
  } while (false)

fs::path SelfPath() {
  wchar_t path[MAX_PATH * 2];
  GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
  return path;
}

// A fresh folder under %TEMP%, removed when the test ends.
struct TempDir {
  fs::path path;
  explicit TempDir(std::wstring_view name) {
    path = fs::temp_directory_path() / std::format(L"knobs-tests-{}-{}", GetCurrentProcessId(), name);
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void WriteFile(const fs::path& path, std::string_view contents) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << contents;
}

bool ContainsName(const std::vector<std::string>& names, std::string_view name) {
  return std::any_of(names.begin(), names.end(),
                     [&](const std::string& n) { return AsciiLower(n) == AsciiLower(name); });
}

bool ContainsPath(const std::vector<fs::path>& paths, const fs::path& path) {
  return std::find(paths.begin(), paths.end(), path) != paths.end();
}

// --- ObsVersion ----------------------------------------------------------------

TEST(VersionUnpacksLibobsFormat) {
  const ObsVersion v = ObsVersion::FromLibobs((32u << 24) | (2u << 16) | 2u);
  CHECK(v == (ObsVersion{32, 2, 2}));
  CHECK(v.ToString() == "32.2.2");
  CHECK(ObsVersion::FromLibobs((31u << 24) | (0u << 16) | 300u) == (ObsVersion{31, 0, 300}));
}

TEST(VersionRangeBoundaries) {
  CHECK(!IsSupportedObsVersion({32, 1, 9}));
  CHECK(IsSupportedObsVersion({32, 2, 0}));
  CHECK(IsSupportedObsVersion({32, 2, 2}));
  CHECK(IsSupportedObsVersion({32, 99, 0}));
  CHECK(!IsSupportedObsVersion({33, 0, 0}));
  CHECK(!DescribeSupportedObsVersions().empty());
}

// --- ObsApi --------------------------------------------------------------------

void Placeholder() {}

TEST(ApiResolvesEveryEntry) {
  ObsApi api;
  const auto missing = ResolveObsApi(api, [](const char*) { return &Placeholder; });
  CHECK(missing.empty());
  CHECK(api.obs_startup != nullptr);
  CHECK(api.obs_shutdown != nullptr);
}

TEST(ApiReportsMissingExports) {
  ObsApi api;
  const auto missing = ResolveObsApi(api, [](const char* name) -> AnyFunction {
    return std::string_view(name) == "obs_reset_audio" ? nullptr : &Placeholder;
  });
  CHECK(missing.size() == 1);
  CHECK(!missing.empty() && missing.front() == "obs_reset_audio");
  CHECK(api.obs_reset_audio == nullptr);
  CHECK(ObsApiSize() > 1);
}

// --- PE imports ----------------------------------------------------------------

TEST(PeImportsOfThisBinary) {
  auto imports = ReadDllImports(SelfPath());
  CHECK(imports.ok());
  if (!imports) return;
  CHECK(ContainsName(*imports, "kernel32.dll"));
  // Delay-loaded; see /DELAYLOAD in CMakeLists.txt.
  CHECK(ContainsName(*imports, "version.dll"));
}

TEST(PeImportsRejectsNonBinaries) {
  TempDir dir(L"pe");
  WriteFile(dir.path / L"not-a-dll.dll", "MZ but not really a PE file");
  CHECK(!ReadDllImports(dir.path / L"not-a-dll.dll").ok());
  CHECK(!ReadDllImports(dir.path / L"missing.dll").ok());
}

// --- OBS install ---------------------------------------------------------------

TEST(InspectRejectsIncompleteInstall) {
  TempDir dir(L"install");
  auto install = InspectObsInstall(dir.path);
  CHECK(!install.ok());
  CHECK(!install.ok() && install.error().find("obs.dll") != std::string::npos);
}

// --- Runtime copy --------------------------------------------------------------

// A fake install whose "DLLs" are copies of this test binary: real PE files
// with known imports (kernel32.dll, and version.dll delay-loaded).
fs::path MakeFakeInstall(const fs::path& root) {
  const fs::path self = SelfPath();
  const fs::path bin = root / L"bin" / L"64bit";
  const fs::path plugins = root / L"obs-plugins" / L"64bit";
  fs::create_directories(bin);
  fs::create_directories(plugins);
  fs::copy_file(self, bin / L"obs.dll");
  fs::copy_file(self, bin / L"libobs-d3d11.dll");
  fs::copy_file(self, bin / L"version.dll");  // Shadows the system DLL, so it's copied.
  fs::copy_file(self, bin / L"unrelated.dll");  // Nothing imports it.
  WriteFile(bin / L"obs.pdb", "symbols");
  fs::copy_file(self, plugins / L"win-wasapi.dll");
  fs::copy_file(self, plugins / L"obs-filters.dll");
  fs::copy_file(self, plugins / L"obs-vst.dll");  // Not a module knOBS loads.
  WriteFile(root / L"data" / L"libobs" / L"default.effect", "effect");
  WriteFile(root / L"data" / L"obs-plugins" / L"win-wasapi" / L"locale" / L"en-US.ini", "a=b");
  WriteFile(root / L"data" / L"obs-plugins" / L"obs-filters" / L"locale" / L"en-US.ini", "c=d");
  WriteFile(root / L"data" / L"obs-plugins" / L"obs-vst" / L"locale" / L"en-US.ini", "e=f");
  return root;
}

TEST(PlanCoversImportClosureAndData) {
  TempDir dir(L"plan");
  const fs::path root = MakeFakeInstall(dir.path / L"obs");
  auto plan = PlanRuntimeCopy(root);
  CHECK(plan.ok());
  if (!plan) return;

  const std::vector<fs::path>& files = plan->files;
  CHECK(ContainsPath(files, L"bin/64bit/obs.dll"));
  CHECK(ContainsPath(files, L"bin/64bit/obs.pdb"));
  CHECK(ContainsPath(files, L"bin/64bit/libobs-d3d11.dll"));
  CHECK(ContainsPath(files, L"bin/64bit/version.dll"));
  CHECK(ContainsPath(files, L"obs-plugins/64bit/win-wasapi.dll"));
  CHECK(ContainsPath(files, L"obs-plugins/64bit/obs-filters.dll"));
  CHECK(ContainsPath(files, L"data/libobs/default.effect"));
  CHECK(ContainsPath(files, L"data/obs-plugins/win-wasapi/locale/en-US.ini"));
  CHECK(ContainsPath(files, L"data/obs-plugins/obs-filters/locale/en-US.ini"));
  CHECK(!ContainsPath(files, L"bin/64bit/unrelated.dll"));
  CHECK(!ContainsPath(files, L"obs-plugins/64bit/obs-vst.dll"));
  CHECK(!ContainsPath(files, L"data/obs-plugins/obs-vst/locale/en-US.ini"));
  CHECK(files.size() == 9);

  CHECK(ContainsName(plan->system_dlls, "kernel32.dll"));
  CHECK(!ContainsName(plan->system_dlls, "version.dll"));
}

TEST(EnsureCopiesReusesAndRepairs) {
  TempDir dir(L"copy");
  const ObsInstall install{MakeFakeInstall(dir.path / L"obs"), {32, 2, 2}};
  const fs::path base = dir.path / L"runtime";

  auto first = EnsureRuntimeCopy(install, base, false);
  CHECK(first.ok());
  if (!first) return;
  CHECK(!first->reused);
  CHECK(first->root == base / L"32.2.2");
  CHECK(first->file_count == 9);
  CHECK(fs::exists(first->root / L"bin" / L"64bit" / L"obs.dll"));
  CHECK(!fs::exists(base / L"32.2.2.partial"));

  auto second = EnsureRuntimeCopy(install, base, false);
  CHECK(second.ok() && second->reused);

  // A damaged copy is replaced.
  fs::remove(first->root / L"data" / L"libobs" / L"default.effect");
  auto third = EnsureRuntimeCopy(install, base, false);
  CHECK(third.ok() && !third->reused);
  CHECK(fs::exists(first->root / L"data" / L"libobs" / L"default.effect"));

  auto forced = EnsureRuntimeCopy(install, base, true);
  CHECK(forced.ok() && !forced->reused);

  // A different OBS version gets its own folder.
  const ObsInstall newer{install.root, {32, 3, 0}};
  auto other = EnsureRuntimeCopy(newer, base, false);
  CHECK(other.ok() && !other->reused && other->root == base / L"32.3.0");
}

}  // namespace

int main() {
  for (const TestCase& test : Tests()) {
    const int before = g_failures;
    test.run();
    std::printf("[%s] %s\n", g_failures == before ? "ok" : "FAIL", test.name);
  }
  std::printf("%zu tests, %d failed checks\n", Tests().size(), g_failures);
  return g_failures == 0 ? 0 : 1;
}
