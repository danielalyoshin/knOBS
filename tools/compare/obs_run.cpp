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

#include "common/text_file.h"
#include "runtime/obs_layout.h"
#include "util/win_strings.h"

namespace knobs::tools {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// Written last, so a copy without it is incomplete.
constexpr wchar_t kCopyMarker[] = L"knobs-copy.txt";
constexpr char kProfileName[] = "knOBS comparison";
constexpr wchar_t kProfileDir[] = L"knOBS";
constexpr char kCollectionName[] = "knOBS comparison";
constexpr wchar_t kCollectionFile[] = L"knOBS.json";
// Logged by OBSBasic::RecordingStart and RecordingStop.
constexpr std::string_view kRecordingStart = "==== Recording Start ====";
constexpr std::string_view kRecordingStop = "==== Recording Stop ====";
// FFmpeg's AV_CODEC_ID_PCM_F32LE, which OBS's custom output setting stores
// next to the encoder's name.
constexpr int kPcmF32Le = 0x10015;

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

// OBS's settings for the run, in the copy's portable config folder.
Status WriteSettings(const ObsRecording& run, const fs::path& obs_studio, const fs::path& recordings) {
  const uint32_t packed = (run.version.major << 24) | (run.version.minor << 16) | run.version.patch;
  // A known LastVersion and FirstRun skip the first-run wizard and the
  // what's-new dialog.
  Status written = WriteText(obs_studio / L"global.ini", std::format("[General]\r\n"
                                                                     "LastVersion={}\r\n"
                                                                     "EnableAutoUpdates=false\r\n",
                                                                     packed));
  if (!written) return written;
  written = WriteText(obs_studio / L"user.ini", std::format("[General]\r\n"
                                                            "FirstRun=true\r\n"
                                                            "ConfirmOnExit=false\r\n"
                                                            "\r\n"
                                                            "[BasicWindow]\r\n"
                                                            "SysTrayEnabled=true\r\n"
                                                            "\r\n"
                                                            "[Basic]\r\n"
                                                            "Profile={0}\r\n"
                                                            "ProfileDir={1}\r\n"
                                                            "SceneCollection={2}\r\n"
                                                            "SceneCollectionFile={3}\r\n",
                                                            kProfileName, ToUtf8(std::wstring_view(kProfileDir)),
                                                            kCollectionName,
                                                            ToUtf8(std::wstring_view(kCollectionFile))));
  if (!written) return written;
  // Advanced output, recording with the custom FFmpeg output: a WAV file with
  // 32-bit float PCM from track 1, and no video stream (WAV has none).
  written = WriteText(obs_studio / L"basic" / L"profiles" / kProfileDir / L"basic.ini",
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
                                  kProfileName, ToObsPath(recordings), kPcmF32Le, run.sample_rate,
                                  run.channel_setup));
  if (!written) return written;
  // The source as the Mic/Aux device, an empty scene, and the output timer
  // (frontend-tools) set to stop the recording.
  const int seconds = run.seconds;
  return WriteText(obs_studio / L"basic" / L"scenes" / kCollectionFile,
                   std::format(R"({{
  "name": "{0}",
  "current_scene": "Scene",
  "current_program_scene": "Scene",
  "scene_order": [{{"name": "Scene"}}],
  "sources": [
    {{"id": "scene", "versioned_id": "scene", "name": "Scene",
      "settings": {{"id_counter": 0, "custom_size": false, "items": []}}}}
  ],
  "AuxAudioDevice1": {1},
  "modules": {{
    "output-timer": {{
      "streamTimerHours": 0, "streamTimerMinutes": 0, "streamTimerSeconds": 30,
      "recordTimerHours": {2}, "recordTimerMinutes": {3}, "recordTimerSeconds": {4},
      "autoStartStreamTimer": false, "autoStartRecordTimer": true, "pauseRecordTimer": true
    }}
  }},
  "version": 2
}}
)",
                               kCollectionName, run.source_json, seconds / 3600, seconds / 60 % 60, seconds % 60));
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

  // Into a staging folder first, so an interrupted copy is never used.
  const fs::path staging = base / (name + L".partial");
  std::error_code ec;
  fs::remove_all(staging, ec);
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

  if (fs::exists(copy, ec)) {
    // Only ever replace a copy made here.
    if (!fs::exists(copy / kCopyMarker, ec)) {
      return Error{std::format("{} exists and isn't a copy of OBS made by knobs-compare.", ToUtf8(copy))};
    }
    fs::remove_all(copy, ec);
    if (ec) return Error{std::format("Couldn't replace {}: {}", ToUtf8(copy), ec.message())};
  }
  fs::rename(staging, copy, ec);
  if (ec) return Error{std::format("Couldn't move the copy to {}: {}", ToUtf8(copy), ec.message())};
  return copy;
}

Result<ObsRecordingResult> RecordWithObs(const ObsRecording& run) {
  // Both folders are the copy's own, recreated for every run.
  const fs::path config = run.copy / L"config";
  const fs::path recordings = run.copy / L"recordings";
  std::error_code ec;
  fs::remove_all(config, ec);
  fs::remove_all(recordings, ec);
  fs::create_directories(recordings, ec);
  if (ec) return Error{std::format("Couldn't set up {}: {}", ToUtf8(run.copy), ec.message())};
  const fs::path obs_studio = config / L"obs-studio";
  const Status written = WriteSettings(run, obs_studio, recordings);
  if (!written) return Error{written.error()};

  const fs::path bin = runtime::BinDir(run.copy);
  const fs::path exe = bin / L"obs64.exe";
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

  for (fs::directory_iterator it(recordings, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() == L".wav") {
      result.wav = it->path();
      return result;
    }
  }
  return Error{"OBS stopped recording, but there's no recording." + log_note};
}

}  // namespace knobs::tools
