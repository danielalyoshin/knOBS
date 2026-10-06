// SPDX-License-Identifier: GPL-2.0-or-later
//
// Unit tests for the parts of the runtime layer that don't need OBS
// installed, and the test runner. The core's tests are in core_tests.cpp.

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "audio/audio_devices.h"
#include "audio/live_chain.h"
#include "common/audio_diff.h"
#include "common/console.h"
#include "common/envelope.h"
#include "common/sha256.h"
#include "common/wav.h"
#include "compare/obs_run.h"
#include "import/mic_import.h"
#include "import/obs_config.h"
#include "import/obs_ini.h"
#include "runtime/obs_api.h"
#include "runtime/obs_install.h"
#include "runtime/obs_log.h"
#include "runtime/obs_runtime.h"
#include "runtime/obs_version.h"
#include "runtime/pe_imports.h"
#include "runtime/runtime_copy.h"
#include "test_harness.h"
#include "util/json.h"
#include "util/win_strings.h"

namespace {

using namespace knobs;
using namespace knobs::runtime;
using namespace knobs::tools;
namespace fs = std::filesystem;

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
  fs::copy_file(self, plugins / L"obs-vst.dll");  // Not a module knobs loads.
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

  // Replaced copies are moved aside and then removed.
  for (const auto& entry : fs::directory_iterator(base)) {
    CHECK(entry.path().filename() == L"32.2.2");
  }

  // A different OBS version gets its own folder.
  const ObsInstall newer{install.root, {32, 3, 0}};
  auto other = EnsureRuntimeCopy(newer, base, false);
  CHECK(other.ok() && !other->reused && other->root == base / L"32.3.0");
}

TEST(EnsureRecopiesWhenInstallChanges) {
  TempDir dir(L"changed");
  const ObsInstall install{MakeFakeInstall(dir.path / L"obs"), {32, 2, 2}};
  const fs::path base = dir.path / L"runtime";
  CHECK(EnsureRuntimeCopy(install, base, false).ok());

  // Same version, rebuilt obs.dll (e.g. a beta replaced by the final release,
  // which has the same version resource). Same size, newer timestamp.
  const fs::path dll = install.root / L"bin" / L"64bit" / L"obs.dll";
  fs::last_write_time(dll, fs::last_write_time(dll) + std::chrono::hours(1));
  auto again = EnsureRuntimeCopy(install, base, false);
  CHECK(again.ok() && !again->reused);
  auto settled = EnsureRuntimeCopy(install, base, false);
  CHECK(settled.ok() && settled->reused);
}

TEST(EnsureRefusesUnsupportedVersions) {
  TempDir dir(L"unsupported");
  const fs::path base = dir.path / L"runtime";
  for (const ObsVersion version : {ObsVersion{31, 1, 4}, ObsVersion{33, 0, 0}}) {
    const ObsInstall install{MakeFakeInstall(dir.path / FromUtf8(version.ToString())), version};
    auto copy = EnsureRuntimeCopy(install, base, false);
    CHECK(!copy.ok());
    CHECK(!copy.ok() && copy.error().find("isn't supported") != std::string::npos);
  }
  CHECK(!fs::exists(base) || fs::is_empty(base));
}

// --- Runtime load --------------------------------------------------------------

TEST(FailedLoadRestoresWorkingDirectory) {
  TempDir dir(L"load");
  // A copy of this test binary stands in for obs.dll: it loads, but exports
  // none of the libobs functions.
  const fs::path root = MakeFakeInstall(dir.path / L"runtime");
  const fs::path before = fs::current_path();
  auto runtime = ObsRuntime::Load(root);
  CHECK(!runtime.ok());
  CHECK(!runtime.ok() && runtime.error().find("missing") != std::string::npos);
  CHECK(fs::current_path() == before);
}

// --- Log -----------------------------------------------------------------------

std::string ReadWhileOpen(const fs::path& file) {
  // How an editor opens a file another program is writing.
  HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return "<sharing violation>";
  char buffer[4096];
  DWORD read = 0;
  ReadFile(handle, buffer, sizeof(buffer), &read, nullptr);
  CloseHandle(handle);
  return std::string(buffer, read);
}

TEST(LogIsReadableWhileOpenAndAppends) {
  TempDir dir(L"log");
  const fs::path file = dir.path / L"run.txt";
  {
    auto log = ObsLog::Open(file);
    CHECK(log.ok());
    if (!log) return;
    (*log)->Write(LOG_INFO, "first run");
    CHECK(ReadWhileOpen(file).find("first run") != std::string::npos);
  }
  {
    auto log = ObsLog::Open(file);  // Same name, e.g. two runs in one second.
    CHECK(log.ok());
    if (!log) return;
    (*log)->Write(LOG_INFO, "second run");
  }
  const std::string contents = ReadWhileOpen(file);
  CHECK(contents.find("first run") != std::string::npos);
  CHECK(contents.find("second run") != std::string::npos);
}

TEST(LogKeepsRecentProblemsOnly) {
  TempDir dir(L"problems");
  auto log = ObsLog::Open(dir.path / L"run.txt");
  CHECK(log.ok());
  if (!log) return;
  (*log)->Write(LOG_INFO, "not a problem");
  for (int i = 0; i < 120; ++i) (*log)->Write(LOG_WARNING, std::format("warning {}", i));
  CHECK((*log)->problem_count() == 120);
  const auto recent = (*log)->RecentProblems();
  CHECK(recent.size() == ObsLog::kRecentProblems);
  CHECK(!recent.empty() && recent.back().ends_with("warning 119"));
}

TEST(PruneLogsKeepsNewest) {
  TempDir dir(L"prune");
  const auto now = fs::file_time_type::clock::now();
  for (int i = 0; i < 5; ++i) {
    const fs::path file = dir.path / std::format(L"smoke {}.txt", i);
    WriteFile(file, "log");
    fs::last_write_time(file, now - std::chrono::minutes(10 - i));
  }
  WriteFile(dir.path / L"other.txt", "not a smoke log");
  PruneLogs(dir.path, L"smoke ", 2);
  CHECK(!fs::exists(dir.path / L"smoke 0.txt"));
  CHECK(!fs::exists(dir.path / L"smoke 2.txt"));
  CHECK(fs::exists(dir.path / L"smoke 3.txt"));
  CHECK(fs::exists(dir.path / L"smoke 4.txt"));
  CHECK(fs::exists(dir.path / L"other.txt"));
}

TEST(OpenNewLogPrunesToKeep) {
  TempDir dir(L"newlog");
  for (int i = 0; i < 3; ++i) WriteFile(dir.path / std::format(L"live {}.txt", i), "log");
  auto log = OpenNewLog(dir.path, L"live ", 2);
  CHECK(log.ok());
  size_t count = 0;
  for (const auto& entry : fs::directory_iterator(dir.path)) {
    if (entry.path().filename().native().starts_with(L"live ")) ++count;
  }
  CHECK(count == 2);
}

TEST(OpenNewLogNeedsPrefix) {
  TempDir dir(L"noprefix");
  WriteFile(dir.path / L"other.txt", "not ours");
  CHECK(!OpenNewLog(dir.path, L"", 1).ok());
  CHECK(fs::exists(dir.path / L"other.txt"));
}

TEST(OpenNewLogNamesConcurrentRunsApart) {
  TempDir dir(L"samesecond");
  // Both stay open, as two runs of one tool would.
  auto first = OpenNewLog(dir.path, L"live ", 5);
  auto second = OpenNewLog(dir.path, L"live ", 5);
  CHECK(first.ok() && second.ok());
  if (!first || !second) return;
  CHECK((*first)->path() != (*second)->path());
}

// --- Tool helpers --------------------------------------------------------------

TEST(PeakHoldTakesAndResets) {
  PeakHold peak;
  const float samples[] = {0.1f, -0.5f, 0.25f};
  peak.Add(samples, 3);
  CHECK(peak.Take() == 0.5f);
  CHECK(peak.Take() == 0.0f);
}

TEST(FormatPeakSaysSilence) {
  CHECK(FormatPeak(0.0f) == "silence");
  CHECK(FormatPeak(0.5f) == "-6.0 dBFS");
}

// --- JSON ----------------------------------------------------------------------

TEST(JsonQuoteEscapes) {
  CHECK(JsonQuote("plain") == "\"plain\"");
  CHECK(JsonQuote("a\"b\\c") == "\"a\\\"b\\\\c\"");
  CHECK(JsonQuote("tab\tnew\n") == "\"tab\\u0009new\\u000a\"");
  CHECK(JsonQuote("\xC3\xA9") == "\"\xC3\xA9\"");  // UTF-8 passes through.
}

TEST(MicSourceJsonQuotesTheDevice) {
  const std::string json = audio::MicWithGainSourceJson("{0.0.1}.{\"odd\"}", -3.5);
  CHECK(json.find(R"("device_id": "{0.0.1}.{\"odd\"}")") != std::string::npos);
  CHECK(json.find(R"("db": -3.5)") != std::string::npos);
  CHECK(json.find(R"("id": "gain_filter")") != std::string::npos);
}

// --- OBS settings --------------------------------------------------------------

TEST(IniReadsLikeLibobs) {
  const auto ini = import::ObsIni::Parse(
      "\xEF\xBB\xBF"
      "ignored=before any section\r\n"
      "[Basic]\r\n"
      "Profile=Streaming Voice\r\n"
      "  # a comment\n"
      "Key With Space = padded \r\n"
      "no equals sign\n"
      "Path=C:\\\\Users\\\\me\\nnext\\tab\n"
      "Empty=\n"
      "Twice=first\n"
      "Twice=second\n"
      "[Video]Inline=after the header\n"
      "[Basic]\n"
      "SceneCollection=Main\n");
  // A repeated section replaces the earlier one, as libobs's lookup finds the
  // last.
  CHECK(!ini.Get("Basic", "Profile"));
  CHECK(ini.Get("Basic", "SceneCollection") == "Main");
  CHECK(ini.Get("Video", "Inline") == "after the header");
  CHECK(!ini.Get("General", "ignored"));
  CHECK(!ini.Get("Missing", "Profile"));

  const auto first = import::ObsIni::Parse(
      "[Basic]\nKey With Space = padded \nno equals sign\nPath=C:\\\\Users\\\\me\\nnext\\tab\nEmpty=\n"
      "Twice=first\nTwice=second\n#Commented=1\n");
  CHECK(first.Get("Basic", "Key With Space ") == " padded ");
  CHECK(!first.Get("Basic", "no equals sign"));
  CHECK(first.Get("Basic", "Path") == "C:\\Users\\me\nnext\\tab");
  CHECK(first.Get("Basic", "Empty") == "");
  CHECK(first.Get("Basic", "Twice") == "second");
  CHECK(!first.Get("Basic", "#Commented"));

  // An empty section name stops libobs's parser.
  const auto stopped = import::ObsIni::Parse("[A]\nx=1\n[]\n[B]\ny=2\n");
  CHECK(stopped.Get("A", "x") == "1");
  CHECK(!stopped.Get("B", "y"));
}

// An OBS settings folder with the given files, relative to obs-studio\.
void WriteObsConfig(const fs::path& root, std::initializer_list<std::pair<const char*, std::string_view>> files) {
  for (const auto& [name, contents] : files) WriteFile(root / L"obs-studio" / name, contents);
}

TEST(ConfigRootIsPortableWithAMarker) {
  TempDir dir(L"portable");
  auto normal = import::FindObsConfigRoot(dir.path);
  CHECK(normal.ok() && !normal->portable && normal->path.filename() != L"config");
  WriteFile(dir.path / L"obs_portable_mode.txt", "");
  auto portable = import::FindObsConfigRoot(dir.path);
  CHECK(portable.ok() && portable->portable && portable->path == dir.path / L"config");

  // A folder given directly is portable if it's that config\ folder.
  CHECK(import::ObsConfigRootAt(dir.path / L"config").portable);
  CHECK(import::ObsConfigRootAt(dir.path / L"CONFIG" / L"").portable);
  CHECK(import::ObsConfigRootAt(dir.path / L"CONFIG" / L"").path == dir.path / L"CONFIG");
  CHECK(!import::ObsConfigRootAt(dir.path / L"settings").portable);
  CHECK(!import::ObsConfigRootAt(dir.path).portable);
}

TEST(SettingsFolderIsTheOneAboveObsStudio) {
  TempDir dir(L"picked");
  WriteFile(dir.path / L"config" / L"obs-studio" / L"global.ini", "");
  // Picked as meant, or one level too deep.
  CHECK(import::SettingsFolderFor(dir.path / L"config") == dir.path / L"config");
  CHECK(import::SettingsFolderFor(dir.path / L"config" / L"obs-studio") == dir.path / L"config");
  CHECK(import::SettingsFolderFor(dir.path / L"config" / L"OBS-Studio" / L"") == dir.path / L"config");
  // A folder named obs-studio that holds one is the settings folder itself.
  WriteFile(dir.path / L"obs-studio" / L"obs-studio" / L"global.ini", "");
  CHECK(import::SettingsFolderFor(dir.path / L"obs-studio") == dir.path / L"obs-studio");
}

TEST(ConfigReadsSampleRateLikeObs) {
  // libobs reads numbers with strtoull: whitespace first and junk after are
  // fine, and "0x" means hex.
  TempDir dir(L"rate");
  const auto rate = [&](std::string_view value) -> std::optional<uint32_t> {
    WriteObsConfig(dir.path, {{"global.ini", ""},
                              {"user.ini", "[Basic]\nProfile=P\nSceneCollection=C\n"},
                              {"basic/profiles/P/basic.ini", std::format("[Audio]\nSampleRate={}\n", value)}});
    auto config = import::FindActiveObsConfig({dir.path, false});
    if (!config) return std::nullopt;
    return config->audio.sample_rate;
  };
  CHECK(rate("44100") == 44100u);
  CHECK(rate(" 48000 ") == 48000u);
  CHECK(rate("48000Hz") == 48000u);
  CHECK(rate("0xBB80") == 48000u);
  CHECK(!rate("fast"));
  CHECK(!rate("0"));
}

TEST(ConfigFindsProfileByName) {
  TempDir dir(L"config");
  WriteObsConfig(dir.path, {
                               {"global.ini", "[General]\nLastVersion=537001986\n"},
                               {"user.ini", "[Basic]\nProfile=Voice\nSceneCollection=Main\n"},
                               // Folder names don't matter, the saved Name does.
                               {"basic/profiles/Voice/basic.ini", "[General]\nName=Other\n"},
                               {"basic/profiles/Folder/basic.ini",
                                "[General]\nName=Voice\n[Audio]\nSampleRate=44100\nChannelSetup=5.1\n"
                                "MonitoringDeviceId={x}\nMonitoringDeviceName=Cable\n"},
                           });
  auto config = import::FindActiveObsConfig({dir.path, false});
  CHECK(config.ok());
  if (!config) return;
  CHECK(config->settings_file.filename() == L"user.ini");
  CHECK(config->profile == "Voice" && config->profile_dir.filename() == L"Folder");
  CHECK(config->audio.sample_rate == 44100 && config->audio.speakers == SPEAKERS_5POINT1);
  CHECK(config->audio.monitoring_device_id == "{x}" && config->audio.monitoring_device_name == "Cable");
  CHECK(config->collection == "Main");
  CHECK(config->scenes_dir == dir.path / L"obs-studio" / L"basic" / L"scenes");
}

TEST(ConfigDefaultsAndLegacyGlobalIni) {
  TempDir dir(L"legacy");
  // No user.ini: OBS 30 and older kept [Basic] in global.ini. A profile
  // without a Name goes by its folder, and missing [Audio] keys get OBS's
  // defaults.
  WriteObsConfig(dir.path, {
                               {"global.ini", "[Basic]\nProfile=Untitled\nSceneCollection=Untitled\n"},
                               {"basic/profiles/Untitled/basic.ini", "[Video]\nBaseCX=1920\n"},
                           });
  auto config = import::FindActiveObsConfig({dir.path, false});
  CHECK(config.ok());
  if (!config) return;
  CHECK(config->settings_file.filename() == L"global.ini");
  CHECK(config->audio.sample_rate == 48000 && config->audio.channel_setup == "Stereo");
  CHECK(config->audio.speakers == SPEAKERS_STEREO && config->audio.monitoring_device_id == "default");
}

TEST(ConfigUsesExistingLocationsOnly) {
  TempDir dir(L"locations");
  TempDir elsewhere(L"locations-elsewhere");
  // OBS writes paths with escaped backslashes.
  std::string escaped;
  for (const char c : ToUtf8(elsewhere.path)) escaped += c == '\\' ? std::string("\\\\") : std::string(1, c);
  const std::string global = std::format(
      "[Locations]\nConfiguration=C:\\\\knobs-missing\nProfiles={}\nSceneCollections={}\n", escaped, escaped);
  WriteObsConfig(dir.path, {
                               {"global.ini", global},
                               {"user.ini", "[Basic]\nProfile=P\nSceneCollection=C\n"},
                           });
  WriteFile(elsewhere.path / L"obs-studio" / L"basic" / L"profiles" / L"P" / L"basic.ini", "[General]\nName=P\n");
  auto config = import::FindActiveObsConfig({dir.path, false});
  CHECK(config.ok());
  if (!config) return;
  CHECK(config->profile_dir == elsewhere.path / L"obs-studio" / L"basic" / L"profiles" / L"P");
  CHECK(config->scenes_dir == elsewhere.path / L"obs-studio" / L"basic" / L"scenes");
  // Portable mode ignores [Locations].
  CHECK(!import::FindActiveObsConfig({dir.path, true}).ok());
}

TEST(SameFolderComparesAsWindowsDoes) {
  CHECK(import::SameFolder(L"C:\\Users\\you\\AppData\\Roaming", L"c:\\users\\YOU\\appdata\\roaming\\"));
  CHECK(import::SameFolder(L"E:/Portable/config", L"E:\\Portable\\config"));
  CHECK(import::SameFolder(L"E:\\Portable\\obs\\..\\config", L"E:\\Portable\\config"));
  CHECK(!import::SameFolder(L"E:\\Portable\\config", L"E:\\Portable\\config2"));
  CHECK(!import::SameFolder(L"E:\\Portable\\config", L""));
  CHECK(!import::SameFolder(L"", L""));
}

TEST(ConfigReportsWhatsMissing) {
  TempDir dir(L"missing");
  auto none = import::FindActiveObsConfig({dir.path, false});
  CHECK(!none.ok() && none.error().find("Open OBS once and close it.") != std::string::npos);
  // A folder OBS doesn't keep its settings in: opening OBS wouldn't fill it.
  none = import::FindActiveObsConfig({dir.path, false}, false);
  CHECK(!none.ok() && none.error().ends_with("obs-studio.") && none.error().find("Open OBS") == std::string::npos);
  WriteObsConfig(dir.path, {{"global.ini", ""}, {"user.ini", "[Basic]\nProfile=Gone\nSceneCollection=C\n"}});
  auto gone = import::FindActiveObsConfig({dir.path, false});
  CHECK(!gone.ok() && gone.error().find("\"Gone\"") != std::string::npos);
  WriteObsConfig(dir.path, {{"basic/profiles/Gone/basic.ini", "[Audio]\nSampleRate=fast\n"}});
  auto bad_rate = import::FindActiveObsConfig({dir.path, false});
  CHECK(!bad_rate.ok() && bad_rate.error().find("sample rate") != std::string::npos);
}

TEST(CollectionFoundBySavedName) {
  TempDir dir(L"collections");
  const fs::path scenes = dir.path / L"obs-studio" / L"basic" / L"scenes";
  WriteFile(scenes / L"Main_Scenes.json", "named");
  WriteFile(scenes / L"Main Scenes.json.bak", "a backup");
  WriteFile(scenes / L"Untitled.json", "unnamed");
  WriteFile(scenes / L"Upper.JSON", "wrong case");
  import::ActiveObsConfig config;
  config.scenes_dir = scenes;
  const auto names = [](const fs::path& file) -> std::string {
    if (file.filename() == L"Main_Scenes.json") return "Main Scenes";
    if (file.filename() == L"Upper.JSON") return "Upper";
    return "";
  };
  config.collection = "Main Scenes";
  auto main = import::FindSceneCollectionFile(config, names);
  CHECK(main.ok() && main->filename() == L"Main_Scenes.json");
  config.collection = "Untitled";  // No saved name: the file name counts.
  auto untitled = import::FindSceneCollectionFile(config, names);
  CHECK(untitled.ok() && untitled->filename() == L"Untitled.json");
  config.collection = "Upper";  // OBS only looks at ".json".
  CHECK(!import::FindSceneCollectionFile(config, names).ok());
}

TEST(PickMicByNumberOrName) {
  std::vector<import::MicCandidate> mics(3);
  mics[0].name = "Mic/Aux";
  mics[0].location = "AuxAudioDevice1";
  mics[0].monitored = true;
  mics[1].name = "Podcast Mic";
  mics[1].location = "sources[2]";
  mics[2].name = "podcast mic";
  mics[2].location = "sources[5]";
  CHECK(import::DescribeMic(mics[0], 1) == "1. \"Mic/Aux\" (AuxAudioDevice1, monitored)");
  CHECK(import::PickMic(mics, "2").ok() && *import::PickMic(mics, "2") == 1);
  CHECK(import::PickMic(mics, "MIC/AUX").ok() && *import::PickMic(mics, "MIC/AUX") == 0);
  const auto ambiguous = import::PickMic(mics, "Podcast Mic");
  CHECK(!ambiguous.ok() && ambiguous.error().find("Several") != std::string::npos);
  CHECK(!import::PickMic(mics, "4").ok());
  CHECK(!import::PickMic(mics, "0").ok());
  const auto unpicked = import::PickMic(mics, "");
  CHECK(!unpicked.ok() && unpicked.error().find("3. \"podcast mic\" (sources[5])") != std::string::npos);
  CHECK(import::PickMic({mics[1]}, "").ok());
  CHECK(!import::PickMic({}, "").ok());
}

TEST(SavedMicPickIsAName) {
  // As the tray saves picks: a name that's a number is still a name.
  std::vector<import::MicCandidate> mics;
  for (const char* name : {"Desk", "Podcast Mic", "1", "7", "podcast mic"}) mics.push_back({.name = name});
  const auto pick = [&](std::string_view name) { return import::PickMicByName(mics, name); };
  CHECK(pick("1").ok() && *pick("1") == 2);
  CHECK(pick("7").ok() && *pick("7") == 3);
  const auto numbered = pick("2");
  CHECK(!numbered.ok() && numbered.error().starts_with("There's no mic named \"2\". Choose one:"));
  // The exact name first, then one that differs only in case.
  CHECK(pick("Podcast Mic").ok() && *pick("Podcast Mic") == 1);
  CHECK(pick("podcast mic").ok() && *pick("podcast mic") == 4);
  CHECK(pick("DESK").ok() && *pick("DESK") == 0);
  const auto ambiguous = pick("PODCAST MIC");
  CHECK(!ambiguous.ok() && ambiguous.error().starts_with("Several mics are named"));
  CHECK(!pick("").ok());
  CHECK(import::PickMicByName({mics[2]}, "").ok());
  CHECK(!import::PickMicByName({}, "1").ok());
}

// --- Devices -------------------------------------------------------------------

TEST(FindDeviceByIdOrName) {
  const std::vector<audio::AudioDevice> devices = {
      {"Default", "default"},
      {"Analogue 1/2 (Audient iD4)", "{a}"},
      {"Loop-back 1/2 (Audient iD4)", "{b}"},
      {"CABLE Input (VB-Audio Virtual Cable)", "{c}"},
      {"CABLE In 16ch (VB-Audio Virtual Cable)", "{d}"},
  };
  CHECK(audio::FindDevice(devices, "{b}", "mic").ok() && audio::FindDevice(devices, "{b}", "mic")->id == "{b}");
  CHECK(audio::FindDevice(devices, "default", "mic")->name == "Default");
  CHECK(audio::FindDevice(devices, "cable input", "output")->id == "{c}");
  CHECK(audio::FindDevice(devices, "analogue", "mic")->id == "{a}");
  // IDs may differ in case between OBS's settings and Windows.
  CHECK(audio::FindDevice(devices, "{B}", "mic").ok() && audio::FindDevice(devices, "{B}", "mic")->id == "{b}");
  CHECK(audio::SameId("default", "Default") && !audio::SameId("{a}", "{b}"));
  CHECK(audio::FindById(devices, "{C}") == &devices[3] && !audio::FindById(devices, "CABLE Input"));

  const auto ambiguous = audio::FindDevice(devices, "iD4", "mic");
  CHECK(!ambiguous.ok() && ambiguous.error().find("Several") != std::string::npos);
  CHECK(!audio::FindDevice(devices, "Scarlett", "mic").ok());
}

// --- WAV -----------------------------------------------------------------------

TEST(WavRoundTrips) {
  TempDir dir(L"wav");
  const FloatAudio audio{48000, 2, {0.0f, -1.0f, 0.5f, 0.25f, 1e-30f, -0.0f}};
  CHECK(WriteFloatWav(dir.path / L"a.wav", audio).ok());
  auto read = ReadFloatWav(dir.path / L"a.wav");
  CHECK(read.ok());
  if (!read) return;
  CHECK(read->sample_rate == 48000 && read->channels == 2 && read->frames() == 3);
  CHECK(std::memcmp(read->samples.data(), audio.samples.data(), audio.samples.size() * sizeof(float)) == 0);
}

// A header with the given format tag and bit depth, then `data_bytes` of data.
std::string WavBytes(uint16_t tag, uint16_t bits, uint16_t channels, uint32_t data_bytes, bool extensible_float) {
  std::string fmt;
  auto put = [](std::string& s, auto value) { s.append(reinterpret_cast<const char*>(&value), sizeof(value)); };
  put(fmt, tag);
  put(fmt, channels);
  put(fmt, uint32_t{48000});
  put(fmt, uint32_t{48000u * channels * bits / 8});
  put(fmt, static_cast<uint16_t>(channels * bits / 8));
  put(fmt, bits);
  if (tag == 0xFFFE) {
    put(fmt, uint16_t{22});
    put(fmt, bits);
    put(fmt, uint32_t{3});
    const uint8_t guid[16] = {static_cast<uint8_t>(extensible_float ? 3 : 1), 0, 0, 0, 0, 0, 0x10, 0,
                              0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
    fmt.append(reinterpret_cast<const char*>(guid), sizeof(guid));
  }
  std::string out = "RIFF";
  put(out, static_cast<uint32_t>(4 + 8 + fmt.size() + 8 + data_bytes));
  out += "WAVEfmt ";
  put(out, static_cast<uint32_t>(fmt.size()));
  out += fmt;
  out += "data";
  put(out, data_bytes);
  out.append(data_bytes, '\0');
  return out;
}

TEST(WavAcceptsExtensibleFloatOnly) {
  TempDir dir(L"wavfmt");
  WriteFile(dir.path / L"ext.wav", WavBytes(0xFFFE, 32, 1, 16, true));
  auto ext = ReadFloatWav(dir.path / L"ext.wav");
  CHECK(ext.ok() && ext->channels == 1 && ext->frames() == 4);

  WriteFile(dir.path / L"pcm16.wav", WavBytes(1, 16, 2, 16, false));
  auto pcm = ReadFloatWav(dir.path / L"pcm16.wav");
  CHECK(!pcm.ok() && pcm.error().find("32-bit float") != std::string::npos);
  WriteFile(dir.path / L"extpcm.wav", WavBytes(0xFFFE, 32, 2, 16, false));
  CHECK(!ReadFloatWav(dir.path / L"extpcm.wav").ok());
  WriteFile(dir.path / L"junk.wav", "RIFF....WAVE");
  CHECK(!ReadFloatWav(dir.path / L"junk.wav").ok());
}

TEST(WavToleratesOversizedDataChunk) {
  // Streaming writers can leave the data size at its maximum.
  TempDir dir(L"wavstream");
  std::string bytes = WavBytes(3, 32, 2, 16, false);
  const size_t size_at = bytes.size() - 16 - 4;
  const uint32_t huge = 0xFFFFFFFF;
  bytes.replace(size_at, 4, reinterpret_cast<const char*>(&huge), 4);
  WriteFile(dir.path / L"stream.wav", bytes);
  auto read = ReadFloatWav(dir.path / L"stream.wav");
  CHECK(read.ok() && read->frames() == 2);
}

// --- Hash ----------------------------------------------------------------------

TEST(Sha256KnownVectors) {
  CHECK(Sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

// --- Envelopes -----------------------------------------------------------------

// `seconds` of sparse noise bursts at 48 kHz, mono.
std::vector<float> Bursts(double seconds, uint32_t seed) {
  std::vector<float> samples(static_cast<size_t>(seconds * 48000));
  uint32_t state = seed;
  auto next = [&] {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state >> 8) / 16777216.0f;
  };
  for (size_t at = 4800; at + 480 < samples.size(); at += 4800 + static_cast<size_t>(next() * 14400)) {
    for (size_t i = 0; i < 240; ++i) samples[at + i] = next() - 0.5f;
  }
  return samples;
}

TEST(EstimateDelayFindsAShift) {
  const uint64_t origin = 1'000'000'000;
  const uint64_t bin = 1'000'000;
  const std::vector<float> clicks = Bursts(6, 7);
  Envelope reference(origin, bin, 8000);
  Envelope delayed(origin, bin, 8000);
  reference.Add(origin, 48000, 1, clicks.data(), clicks.size());
  // 37.4 ms later, quieter, over a noise floor.
  std::vector<float> later(clicks.size());
  for (size_t i = 0; i < later.size(); ++i) later[i] = 0.3f * clicks[i] + 0.001f * static_cast<float>(i % 7);
  delayed.Add(origin + 37'400'000, 48000, 1, later.data(), later.size());

  const DelayEstimate estimate = EstimateDelay(reference, delayed, 500'000'000, 0, 6000);
  CHECK(std::fabs(estimate.delay_ms - 37.4) < 0.5);
  CHECK(estimate.correlation > 0.9);

  // Unrelated audio doesn't match.
  const std::vector<float> other = Bursts(6, 99);
  Envelope unrelated(origin, bin, 8000);
  unrelated.Add(origin, 48000, 1, other.data(), other.size());
  CHECK(EstimateDelay(reference, unrelated, 500'000'000, 0, 6000).correlation < 0.5);
}

// --- Comparing audio -----------------------------------------------------------

// Stereo bursts, the right channel quieter.
FloatAudio StereoBursts(double seconds, uint32_t seed) {
  const std::vector<float> mono = Bursts(seconds, seed);
  FloatAudio audio{48000, 2, {}};
  for (const float x : mono) {
    audio.samples.push_back(x);
    audio.samples.push_back(0.5f * x);
  }
  return audio;
}

// `audio` starting `frames` later (positive), or with its first -frames cut.
FloatAudio Shifted(const FloatAudio& audio, int64_t frames) {
  FloatAudio out{audio.sample_rate, audio.channels, {}};
  if (frames >= 0) {
    out.samples.assign(static_cast<size_t>(frames) * audio.channels, 0.0f);
    out.samples.insert(out.samples.end(), audio.samples.begin(), audio.samples.end());
  } else {
    out.samples.assign(audio.samples.begin() + static_cast<ptrdiff_t>(-frames * audio.channels), audio.samples.end());
  }
  return out;
}

TEST(AlignAudioFindsOffsetsBothWays) {
  const FloatAudio a = StereoBursts(4, 3);
  const auto later = AlignAudio(a, Shifted(a, 12345), -48000, 48000);
  // 257.19 ms: not a whole number of envelope bins, so the coarse match isn't
  // perfect, but the frame search is.
  CHECK(later.ok() && later->offset == 12345 && later->correlation > 0.9);
  const auto earlier = AlignAudio(a, Shifted(a, -777), -48000, 48000);
  CHECK(earlier.ok() && earlier->offset == -777);
  CHECK(!AlignAudio(a, FloatAudio{44100, 2, a.samples}, -48000, 48000).ok());
  // Outside the range searched.
  const auto outside = AlignAudio(a, Shifted(a, 12345), -48000, 0);
  CHECK(!outside.ok() || outside->offset != 12345);
}

TEST(AlignAudioWithMostlySilence) {
  // knobs-compare's case: a short clip after 5 s of silence, and a recording
  // that starts 1.5 s into it and runs 2 s past it. Only the clip lines up.
  const FloatAudio clip = StereoBursts(1.5, 11);
  FloatAudio ours{48000, 2, std::vector<float>(5 * 48000 * 2, 0.0f)};
  ours.samples.insert(ours.samples.end(), clip.samples.begin(), clip.samples.end());
  FloatAudio theirs = Shifted(ours, -72000);
  theirs.samples.resize(theirs.samples.size() + 2 * 48000 * 2, 0.0f);
  const auto alignment = AlignAudio(ours, theirs, -8 * 48000, 2 * 48000);
  CHECK(alignment.ok() && alignment->offset == -72000);
}

TEST(DiffAudioComparesBits) {
  const FloatAudio a{48000, 1, {0.0f, 0.5f, 0.25f}};
  const AudioDiff zeros = DiffAudio(a, FloatAudio{48000, 1, {-0.0f, 0.5f, 0.25f}}, 0);
  CHECK(!zeros.bit_identical() && zeros.identical_samples == 2 && zeros.residual_peak == 0);
  const AudioDiff nan = DiffAudio(a, FloatAudio{48000, 1, {0.0f, std::nanf(""), 0.0f}}, 0);
  CHECK(std::isnan(nan.residual_peak) && std::isnan(nan.residual_rms));
  CHECK(DiffAudio(a, a, 0).bit_identical());
}

TEST(MixLikeObsMatchesLibobs) {
  FloatAudio audio{48000, 1, {0.5f, 1.5f, -2.0f, std::nanf(""), 1.0f, -0.0f, -1e-40f}};
  const MixChanges changes = MixLikeObs(audio);
  CHECK(changes.clamped == 3 && changes.negative_zeros == 1);
  CHECK(audio.samples[0] == 0.5f && audio.samples[1] == 1.0f && audio.samples[2] == -1.0f);
  CHECK(audio.samples[3] == 0.0f && audio.samples[4] == 1.0f);
  CHECK(audio.samples[5] == 0.0f && !std::signbit(audio.samples[5]));
  CHECK(audio.samples[6] == -1e-40f);  // Denormals pass through.
}

TEST(DiffAudioMeasuresTheResidual) {
  const FloatAudio a = StereoBursts(4, 5);
  FloatAudio b = Shifted(a, 100);
  const AudioDiff same = DiffAudio(a, b, 100);
  CHECK(same.frames == a.frames() && same.identical_samples == same.samples && same.residual_rms == 0);
  CHECK(same.signal_rms > 0 && std::fabs(same.gain - 1) < 1e-12);

  // One sample off by 2^-20, in the second 100 ms window.
  b.samples[(100 + 4800 + 10) * 2 + 1] += 1.0f / 1048576;
  const AudioDiff one = DiffAudio(a, b, 100);
  CHECK(one.identical_samples + 1 == one.samples);
  CHECK(std::fabs(one.residual_peak - 1.0 / 1048576) < 1e-9);
  CHECK(one.worst_window_frame == 4800);
  const FloatAudio residual = Residual(a, b, 100);
  CHECK(residual.frames() == a.frames() && residual.samples[(4800 + 10) * 2 + 1] != 0);

  // Half as loud: the best-fit gain says so.
  FloatAudio quiet = a;
  for (float& x : quiet.samples) x *= 0.5f;
  CHECK(std::fabs(ToDb(DiffAudio(a, quiet, 0).gain) - 6.0206) < 0.001);
  CHECK(DiffAudio(a, b, 1'000'000).frames == 0);
}

// --- knobs-compare's OBS copy --------------------------------------------------

TEST(ObsCopyLeavesOutSettingsAndSwapsWhole) {
  TempDir dir(L"obscopy");
  const fs::path install = dir.path / L"install";
  const fs::path work = dir.path / L"work";
  WriteFile(install / L"bin" / L"64bit" / L"obs.dll", "v1");
  WriteFile(install / L"bin" / L"64bit" / L"obs64.exe", "exe");
  WriteFile(install / L"bin" / L"64bit" / L"obs.pdb", "symbols");
  WriteFile(install / L"obs-plugins" / L"64bit" / L"obs-filters.dll", "filters");
  WriteFile(install / L"obs-plugins" / L"64bit" / L"libcef.dll", "chromium");
  WriteFile(install / L"obs-plugins" / L"64bit" / L"locales" / L"en-US.pak", "pak");
  WriteFile(install / L"data" / L"obs-plugins" / L"obs-browser" / L"page.html", "browser");
  // A portable install's own settings stay out of the copy.
  WriteFile(install / L"config" / L"obs-studio" / L"global.ini", "private");
  WriteFile(install / L"obs_portable_mode.txt", "");
  const ObsInstall obs{install, {32, 2, 2}};

  auto copy = PrepareObsCopy(obs, work);
  CHECK(copy.ok());
  if (!copy) return;
  CHECK(fs::exists(*copy / L"bin" / L"64bit" / L"obs64.exe"));
  CHECK(fs::exists(*copy / L"obs-plugins" / L"64bit" / L"obs-filters.dll"));
  CHECK(!fs::exists(*copy / L"bin" / L"64bit" / L"obs.pdb"));
  CHECK(!fs::exists(*copy / L"obs-plugins" / L"64bit" / L"libcef.dll"));
  CHECK(!fs::exists(*copy / L"obs-plugins" / L"64bit" / L"locales"));
  CHECK(!fs::exists(*copy / L"data" / L"obs-plugins" / L"obs-browser"));
  CHECK(!fs::exists(*copy / L"config") && !fs::exists(*copy / L"obs_portable_mode.txt"));
  CHECK(fs::exists(*copy / L"portable_mode.txt"));  // The copy's own.

  // Reused while the install is the same, replaced whole when it changes.
  WriteFile(*copy / L"bin" / L"64bit" / L"left-over.txt", "");
  CHECK(PrepareObsCopy(obs, work).ok() && fs::exists(*copy / L"bin" / L"64bit" / L"left-over.txt"));
  WriteFile(install / L"bin" / L"64bit" / L"obs.dll", "v2, a different size");
  auto replaced = PrepareObsCopy(obs, work);
  CHECK(replaced.ok() && !fs::exists(*copy / L"bin" / L"64bit" / L"left-over.txt"));
  CHECK(!fs::exists(work / L"obs-32.2.2.old") && !fs::exists(work / L"obs-32.2.2.partial"));

  // A folder that isn't a copy made here is never replaced.
  fs::remove(*copy / L"knobs-copy.txt");
  WriteFile(install / L"bin" / L"64bit" / L"obs.dll", "v3, a different size again");
  CHECK(!PrepareObsCopy(obs, work).ok() && fs::exists(*copy / L"bin" / L"64bit" / L"obs64.exe"));
}

TEST(EnvelopeIgnoresFramesOffTheGrid) {
  Envelope envelope(1'000'000'000, 1'000'000, 2);
  const std::vector<float> ones(480, 1.0f);
  envelope.Add(999'995'000, 48000, 1, ones.data(), ones.size());  // Starts 5 us early, ends past the grid.
  const std::vector<double> amplitude = envelope.Amplitude();
  CHECK(amplitude.size() == 2 && amplitude[0] == 1.0 && amplitude[1] == 1.0);
  Envelope empty(0, 1'000'000, 0);
  empty.Add(0, 48000, 1, ones.data(), ones.size());
  CHECK(empty.Amplitude().empty());
}

}  // namespace

int main() {
  using knobs::test::g_failures;
  using knobs::test::Tests;
  if (knobs::test::RunAsStandInObs()) return 0;
  for (const knobs::test::TestCase& test : Tests()) {
    const int before = g_failures;
    test.run();
    std::printf("[%s] %s\n", g_failures == before ? "ok" : "FAIL", test.name);
  }
  std::printf("%zu tests, %d failed checks\n", Tests().size(), g_failures);
  return g_failures == 0 ? 0 : 1;
}
