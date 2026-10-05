// SPDX-License-Identifier: GPL-2.0-or-later
#include "compare/obs_run.h"

#include <windows.h>

#include <array>
#include <chrono>
#include <cwctype>
#include <format>
#include <fstream>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include "app_info.h"
#include "runtime/obs_layout.h"
#include "util/json.h"
#include "util/text_file.h"
#include "util/win_strings.h"

namespace knobs::tools {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// Written last, so a copy without it is incomplete.
constexpr wchar_t kCopyMarker[] = L"knobs-copy.txt";
// Logged by OBSBasic::RecordingStart and RecordingStop.
constexpr std::string_view kRecordingStart = "==== Recording Start ====";
constexpr std::string_view kRecordingStop = "==== Recording Stop ====";
// FFmpeg's AV_CODEC_ID_PCM_F32LE, which OBS's custom output setting stores
// next to the encoder's name.
constexpr int kPcmF32Le = 0x10015;
// The Media Source's, so the scene item can find it (obs-scene.c,
// scene_load_item).
constexpr char kSourceUuid[] = "6b6e6f62-7300-4000-8000-000000000001";

// Next to bin\ (obs-main.cpp). The copy gets its own.
constexpr std::array<std::wstring_view, 4> kPortableMarkers = {L"portable_mode", L"obs_portable_mode",
                                                               L"portable_mode.txt", L"obs_portable_mode.txt"};
// The browser source and its Chromium files in obs-plugins\64bit.
constexpr std::array<std::wstring_view, 10> kBrowserFiles = {
    L"libcef.dll",  L"chrome_elf.dll",     L"libegl.dll",          L"libglesv2.dll",
    L"icudtl.dat",  L"snapshot_blob.bin",  L"v8_context_snapshot.bin", L"vk_swiftshader.dll",
    L"vulkan-1.dll", L"vk_swiftshader_icd.json"};

std::wstring Lower(std::wstring_view text) {
  std::wstring out(text);
  for (wchar_t& c : out) c = static_cast<wchar_t>(towlower(c));
  return out;
}

// Whether the copy leaves out `relative`, a path in the install.
bool LeftOut(const fs::path& relative) {
  const std::wstring name = Lower(relative.filename().native());
  if (relative.extension() == L".pdb" || name == L"uninstall.exe") return true;
  std::vector<std::wstring> parts;
  for (const fs::path& part : relative) parts.push_back(Lower(part.native()));
  if (parts.size() == 1) {
    // A portable install's settings, and its portable-mode marker.
    if (name == L"config") return true;
    for (const std::wstring_view marker : kPortableMarkers) {
      if (name == marker) return true;
    }
  }
  if (parts.size() >= 3 && parts[0] == L"obs-plugins" && parts[1] == L"64bit") {
    if (parts[2] == L"locales" || name.starts_with(L"obs-browser") || name.starts_with(L"obs-websocket") ||
        relative.extension() == L".pak") {
      return true;
    }
    for (const std::wstring_view file : kBrowserFiles) {
      if (name == file) return true;
    }
  }
  return parts.size() >= 3 && parts[0] == L"data" && parts[1] == L"obs-plugins" &&
         (parts[2] == L"obs-browser" || parts[2] == L"obs-websocket");
}

// Identifies the install a copy was made from.
std::string InstallStamp(const runtime::ObsInstall& install) {
  std::error_code ec;
  const fs::path dll = runtime::ObsDll(install.root);
  return std::format("{} {} {}", install.version.ToString(), fs::file_size(dll, ec),
                     fs::last_write_time(dll, ec).time_since_epoch().count());
}

Status WriteText(const fs::path& file, std::string_view text) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.close();
  if (!out) return Error{std::format("Couldn't write {}.", ToUtf8(file))};
  return Ok{};
}

// Deletes a folder this file made, and says so if it can't.
Status Clear(const fs::path& folder) {
  std::error_code ec;
  fs::remove_all(folder, ec);
  if (ec || fs::exists(folder, ec)) {
    return Error{std::format("Couldn't clear {}: {}. Is an OBS from an earlier run still open?", ToUtf8(folder),
                             ec ? ec.message() : "it's still there")};
  }
  return Ok{};
}

// The mic as a Media Source playing `input`: the same filters and
// source-level state (balance, Mono), with what would keep it out of the
// recording or change its level turned off. knobs-compare's side leaves the
// volume out too, so both compare what the filters output.
Result<std::string> MediaSourceJson(const runtime::ObsApi& api, const ObsRecording& run, std::string* name) {
  obs_data_t* source = api.obs_data_create_from_json(run.mic_json.c_str());
  if (!source) return Error{"libobs couldn't read the imported mic."};
  api.obs_data_set_string(source, "id", "ffmpeg_source");
  api.obs_data_set_string(source, "versioned_id", "ffmpeg_source");
  obs_data_t* settings = api.obs_data_create();
  api.obs_data_set_string(settings, "local_file", ToObsPath(run.input).c_str());
  api.obs_data_set_bool(settings, "is_local_file", true);
  api.obs_data_set_bool(settings, "looping", false);
  api.obs_data_set_bool(settings, "restart_on_activate", false);
  api.obs_data_set_bool(settings, "close_when_inactive", false);
  api.obs_data_set_bool(settings, "hw_decode", false);
  api.obs_data_set_obj(source, "settings", settings);
  api.obs_data_release(settings);
  api.obs_data_set_double(source, "volume", 1.0);
  api.obs_data_set_int(source, "sync", 0);
  api.obs_data_set_int(source, "mixers", 1);  // Track 1, which the recording takes.
  api.obs_data_set_bool(source, "enabled", true);
  api.obs_data_set_bool(source, "muted", false);
  api.obs_data_set_bool(source, "push-to-talk", false);
  api.obs_data_set_bool(source, "push-to-mute", false);
  api.obs_data_set_int(source, "monitoring_type", OBS_MONITORING_TYPE_NONE);
  api.obs_data_set_bool(source, "monitoring_enabled", false);
  api.obs_data_set_string(source, "uuid", kSourceUuid);
  api.obs_data_erase(source, "hotkeys");
  const char* found_name = api.obs_data_get_string(source, "name");
  *name = found_name ? found_name : "";
  const char* json = api.obs_data_get_json(source);
  std::string text = json ? json : "";
  api.obs_data_release(source);
  if (text.empty()) return Error{"libobs couldn't write the media source."};
  return text;
}

// The scene collection: the source where the mic was (in the scene, or as
// Mic/Aux), and the output timer (frontend-tools) set to stop the recording.
std::string CollectionJson(const ObsRecording& run, const std::string& source_json, const std::string& source_name,
                           std::string_view collection_name) {
  const std::string items =
      run.mic_from_sources
          ? std::format(R"([{{"name": {}, "source_uuid": "{}", "visible": true, "locked": false, "id": 1}}])",
                        JsonQuote(source_name), kSourceUuid)
          : "[]";
  const std::string scene = std::format(
      R"({{"id": "scene", "versioned_id": "scene", "name": "Scene",
      "settings": {{"id_counter": 1, "custom_size": false, "items": {}}}}})",
      items);
  const std::string sources = run.mic_from_sources ? std::format("[\n    {},\n    {}\n  ]", scene, source_json)
                                                   : std::format("[\n    {}\n  ]", scene);
  const std::string device = run.mic_from_sources ? "" : std::format(R"(  "AuxAudioDevice1": {},
)",
                                                                     source_json);
  const int seconds = run.seconds;
  return std::format(R"({{
  "name": {0},
  "current_scene": "Scene",
  "current_program_scene": "Scene",
  "scene_order": [{{"name": "Scene"}}],
  "sources": {1},
{2}  "modules": {{
    "output-timer": {{
      "streamTimerHours": 0, "streamTimerMinutes": 0, "streamTimerSeconds": 30,
      "recordTimerHours": {3}, "recordTimerMinutes": {4}, "recordTimerSeconds": {5},
      "autoStartStreamTimer": false, "autoStartRecordTimer": true, "pauseRecordTimer": true
    }}
  }},
  "version": 2
}}
)",
                     JsonQuote(collection_name), sources, device, seconds / 3600, seconds / 60 % 60, seconds % 60);
}

// OBS's settings for the run, in the copy's portable config folder.
Status WriteSettings(const runtime::ObsApi& api, const ObsRecording& run, const fs::path& obs_studio,
                     const fs::path& recordings) {
  const std::string name = std::format("{} comparison", kDisplayName);
  const std::wstring profile_dir(kDisplayNameW);
  const std::wstring collection_file = std::wstring(kDisplayNameW) + L".json";
  const uint32_t packed = (run.version.major << 24) | (run.version.minor << 16) | run.version.patch;
  // A known LastVersion and FirstRun skip the first-run wizard and the
  // what's-new dialog.
  Status written = WriteText(obs_studio / L"global.ini", std::format("[General]\r\n"
                                                                     "LastVersion={}\r\n"
                                                                     "EnableAutoUpdates=false\r\n",
                                                                     packed));
  if (!written) return written;
  written = WriteText(obs_studio / L"user.ini",
                      std::format("[General]\r\n"
                                  "FirstRun=true\r\n"
                                  "ConfirmOnExit=false\r\n"
                                  "\r\n"
                                  "[BasicWindow]\r\n"
                                  "SysTrayEnabled=true\r\n"
                                  "\r\n"
                                  "[Basic]\r\n"
                                  "Profile={0}\r\n"
                                  "ProfileDir={1}\r\n"
                                  "SceneCollection={0}\r\n"
                                  "SceneCollectionFile={2}\r\n",
                                  name, ToUtf8(profile_dir), ToUtf8(collection_file)));
  if (!written) return written;
  // Advanced output, recording with the custom FFmpeg output: a WAV file with
  // 32-bit float PCM from track 1, and no video stream (WAV has none).
  written = WriteText(obs_studio / L"basic" / L"profiles" / profile_dir / L"basic.ini",
                      std::format("[General]\r\n"
                                  "Name={}\r\n"
                                  "\r\n"
                                  "[Output]\r\n"
                                  "Mode=Advanced\r\n"
                                  "FilenameFormatting=obs\r\n"
                                  "OverwriteIfExists=true\r\n"
                                  "\r\n"
                                  "[AdvOut]\r\n"
                                  "RecType=FFmpeg\r\n"
                                  "FFOutputToFile=true\r\n"
                                  "FFFilePath={}\r\n"
                                  "FFExtension=wav\r\n"
                                  "FFFormat=wav\r\n"
                                  "FFFormatMimeType=audio/x-wav\r\n"
                                  "FFAEncoder=pcm_f32le\r\n"
                                  "FFAEncoderId={}\r\n"
                                  "FFAudioMixes=1\r\n"
                                  "FFVEncoder=\r\n"
                                  "FFVEncoderId=0\r\n"
                                  "FFIgnoreCompat=true\r\n"
                                  "\r\n"
                                  "[Audio]\r\n"
                                  "SampleRate={}\r\n"
                                  "ChannelSetup={}\r\n"
                                  "\r\n"
                                  "[Video]\r\n"
                                  "BaseCX=640\r\n"
                                  "BaseCY=360\r\n"
                                  "OutputCX=640\r\n"
                                  "OutputCY=360\r\n"
                                  "FPSType=0\r\n"
                                  "FPSCommon=30\r\n",
                                  name, ToObsPath(recordings), kPcmF32Le, run.sample_rate, run.channel_setup));
  if (!written) return written;
  std::string source_name;
  auto source = MediaSourceJson(api, run, &source_name);
  if (!source) return Error{source.error()};
  return WriteText(obs_studio / L"basic" / L"scenes" / collection_file,
                   CollectionJson(run, *source, source_name, name));
}

// The newest log OBS has written, or "".
std::string LatestLog(const fs::path& logs, fs::path* file) {
  std::error_code ec;
  fs::path newest;
  fs::file_time_type newest_time;
  for (fs::directory_iterator it(logs, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() != L".txt") continue;
    const auto time = it->last_write_time(ec);
    if (newest.empty() || time > newest_time) {
      newest = it->path();
      newest_time = time;
    }
  }
  if (newest.empty()) return "";
  *file = newest;
  auto text = ReadText(newest);
  return text ? *text : "";
}

// Asks OBS to close, the way taskkill does without /f, and waits.
bool CloseObs(const PROCESS_INFORMATION& process) {
  EnumWindows(
      [](HWND window, LPARAM pid) -> BOOL {
        DWORD owner = 0;
        GetWindowThreadProcessId(window, &owner);
        if (owner == static_cast<DWORD>(pid)) PostMessageW(window, WM_CLOSE, 0, 0);
        return TRUE;
      },
      static_cast<LPARAM>(process.dwProcessId));
  return WaitForSingleObject(process.hProcess, 30'000) == WAIT_OBJECT_0;
}

}  // namespace

Result<fs::path> PrepareObsCopy(const runtime::ObsInstall& install, const fs::path& base) {
  const std::wstring name = FromUtf8("obs-" + install.version.ToString());
  const fs::path copy = base / name;
  const std::string stamp = InstallStamp(install);
  if (auto marker = ReadText(copy / kCopyMarker); marker && *marker == stamp) return copy;
  std::error_code ec;
  if (fs::exists(copy, ec) && !fs::exists(copy / kCopyMarker, ec)) {
    // Only ever replace a copy made here.
    return Error{std::format("{} exists and isn't a copy of OBS made by knobs-compare.", ToUtf8(copy))};
  }

  // Into a staging folder first, so an interrupted copy is never used. Both
  // it and the folder an old copy is moved aside to are this file's own.
  const fs::path staging = base / (name + L".partial");
  const fs::path old = base / (name + L".old");
  for (const fs::path& folder : {staging, old}) {
    const Status cleared = Clear(folder);
    if (!cleared) return Error{cleared.error()};
  }
  fs::create_directories(staging, ec);
  if (ec) return Error{std::format("Couldn't create {}.", ToUtf8(staging))};
  for (fs::recursive_directory_iterator it(install.root, ec), end; !ec && it != end; it.increment(ec)) {
    const fs::path relative = fs::relative(it->path(), install.root, ec);
    if (ec) break;
    if (LeftOut(relative)) {
      if (it->is_directory(ec)) it.disable_recursion_pending();
      continue;
    }
    if (it->is_directory(ec)) {
      fs::create_directories(staging / relative, ec);
    } else {
      fs::copy_file(it->path(), staging / relative, fs::copy_options::overwrite_existing, ec);
    }
    if (ec) return Error{std::format("Couldn't copy {}: {}", ToUtf8(it->path()), ec.message())};
  }
  if (ec) return Error{std::format("Couldn't copy {}: {}", ToUtf8(install.root), ec.message())};
  // OBS checks for this next to bin\ (obs-main.cpp).
  Status written = WriteText(staging / L"portable_mode.txt", "");
  if (written) written = WriteText(staging / kCopyMarker, stamp);
  if (!written) return Error{written.error()};

  // Swapped by renaming, so an interruption leaves either copy whole.
  if (fs::exists(copy, ec)) {
    fs::rename(copy, old, ec);
    if (ec) return Error{std::format("Couldn't move the old copy aside from {}: {}", ToUtf8(copy), ec.message())};
  }
  fs::rename(staging, copy, ec);
  if (ec) return Error{std::format("Couldn't move the copy to {}: {}", ToUtf8(copy), ec.message())};
  Clear(old);  // Cleared next time if it's still in use.
  return copy;
}

Result<ObsRecordingResult> RecordWithObs(const runtime::ObsApi& api, const ObsRecording& run) {
  // Both folders are the copy's own, recreated for every run, so a failed
  // run can't leave an earlier recording to be taken for its own.
  const fs::path config = run.copy / L"config";
  const fs::path recordings = run.copy / L"recordings";
  for (const fs::path& folder : {config, recordings}) {
    const Status cleared = Clear(folder);
    if (!cleared) return Error{cleared.error()};
  }
  std::error_code ec;
  fs::create_directories(recordings, ec);
  if (ec) return Error{std::format("Couldn't create {}: {}", ToUtf8(recordings), ec.message())};
  const fs::path obs_studio = config / L"obs-studio";
  const Status written = WriteSettings(api, run, obs_studio, recordings);
  if (!written) return Error{written.error()};

  const fs::path bin = runtime::BinDir(run.copy);
  const fs::path exe = bin / runtime::kObsExe;
  std::wstring command = std::format(
      L"\"{}\" --portable --multi --disable-updater --disable-missing-files-check --minimize-to-tray "
      L"--startrecording",
      exe.native());
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process = {};
  // OBS finds its data and plugins relative to the working directory.
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, bin.c_str(), &startup,
                      &process)) {
    return Error{std::format("Couldn't start {}: {}", ToUtf8(exe), DescribeWinError(GetLastError()))};
  }
  CloseHandle(process.hThread);

  // Waits for the output timer to stop the recording.
  const fs::path logs = obs_studio / L"logs";
  fs::path log;
  std::optional<std::string> failure;
  const auto started = std::chrono::steady_clock::now();
  bool recording = false;
  for (;;) {
    const std::string text = LatestLog(logs, &log);
    recording = recording || text.find(kRecordingStart) != std::string::npos;
    if (text.find(kRecordingStop) != std::string::npos) break;
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
      failure = "OBS exited before it finished recording.";
    } else if (!recording && elapsed > 60s) {
      failure = "OBS didn't start recording within a minute.";
    } else if (elapsed > std::chrono::seconds(run.seconds) + 120s) {
      failure = "OBS didn't stop recording in time.";
    }
    if (failure) break;
    std::this_thread::sleep_for(250ms);
  }
  ObsRecordingResult result;
  result.log = log;
  if (!CloseObs(process)) {
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, 10'000);
    result.ended = true;
  }
  CloseHandle(process.hProcess);
  const std::string log_note = log.empty() ? "" : std::format(" OBS's log: {}", ToUtf8(log));
  if (failure) return Error{*failure + log_note};
  if (!recording) return Error{"OBS stopped recording without starting it." + log_note};

  for (fs::directory_iterator it(recordings, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() == L".wav") {
      result.wav = it->path();
      return result;
    }
  }
  return Error{"OBS stopped recording, but there's no recording." + log_note};
}

}  // namespace knobs::tools
