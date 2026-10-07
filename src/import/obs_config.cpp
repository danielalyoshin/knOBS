// SPDX-License-Identifier: GPL-2.0-or-later
#include "import/obs_config.h"

#include <windows.h>

#include <array>
#include <cstdlib>
#include <format>
#include <optional>
#include <string_view>
#include <system_error>

#include "import/obs_ini.h"
#include "util/app_dirs.h"
#include "util/win_strings.h"

namespace knobs::import {
namespace {

namespace fs = std::filesystem;

// Checked next to bin\, in this order (obs-main.cpp).
constexpr std::array<std::wstring_view, 4> kPortableMarkers = {L"portable_mode", L"obs_portable_mode",
                                                               L"portable_mode.txt", L"obs_portable_mode.txt"};

// Relative to a location such as [Locations] Profiles (OBSApp.cpp).
const fs::path kProfilesSubdir = fs::path(L"obs-studio") / L"basic" / L"profiles";
const fs::path kScenesSubdir = fs::path(L"obs-studio") / L"basic" / L"scenes";

bool Exists(const fs::path& path) {
  std::error_code ec;
  return fs::exists(path, ec);
}

bool IsPortableInstall(const fs::path& install_root) {
  for (const std::wstring_view marker : kPortableMarkers) {
    if (Exists(install_root / marker)) return true;
  }
  return false;
}

// How OBS reads a number setting (config-file.c, str_to_uint64): strtoull,
// in hex after "0x", so leading whitespace and trailing junk are fine and
// anything else reads as 0. OBSBasic::ResetAudio keeps the low 32 bits.
uint32_t ObsUint(const std::string& text) {
  if (text.starts_with("0x")) return static_cast<uint32_t>(std::strtoull(text.c_str() + 2, nullptr, 16));
  return static_cast<uint32_t>(std::strtoull(text.c_str(), nullptr, 10));
}

// A [Locations] folder from global.ini, used only if it exists; otherwise,
// and always in portable mode, the config root (OBSApp::InitGlobalConfig).
fs::path Location(const ObsIni* global, const ObsConfigRoot& root, std::string_view key) {
  if (root.portable || !global) return root.path;
  const auto value = global->Get("Locations", key);
  if (!value || value->empty()) return root.path;
  const fs::path path = FromUtf8(*value);
  return Exists(path) ? path : root.path;
}

speaker_layout SpeakersFor(std::string_view setup) {
  if (setup == "Mono") return SPEAKERS_MONO;
  if (setup == "2.1") return SPEAKERS_2POINT1;
  if (setup == "4.0") return SPEAKERS_4POINT0;
  if (setup == "4.1") return SPEAKERS_4POINT1;
  if (setup == "5.1") return SPEAKERS_5POINT1;
  if (setup == "7.1") return SPEAKERS_7POINT1;
  return SPEAKERS_STEREO;
}

Result<ProfileAudio> ReadProfileAudio(const ObsIni& basic, const fs::path& file) {
  ProfileAudio audio;
  if (const auto rate = basic.Get("Audio", "SampleRate")) {
    // OBS can't start its audio at 0 Hz either.
    audio.sample_rate = ObsUint(*rate);
    if (audio.sample_rate == 0) {
      return Error{std::format("{} has an invalid sample rate ({}).", ToUtf8(file), *rate)};
    }
  }
  if (const auto setup = basic.Get("Audio", "ChannelSetup")) audio.channel_setup = *setup;
  audio.speakers = SpeakersFor(audio.channel_setup);
  if (const auto id = basic.Get("Audio", "MonitoringDeviceId")) audio.monitoring_device_id = *id;
  if (const auto name = basic.Get("Audio", "MonitoringDeviceName")) audio.monitoring_device_name = *name;
  return audio;
}

}  // namespace

Result<ObsConfigRoot> FindObsConfigRoot(const fs::path& install_root) {
  if (IsPortableInstall(install_root)) return ObsConfigRoot{install_root / L"config", true, true};
  // %AppData%, even for a portable knobs.
  auto app_data = UserAppData();
  if (!app_data) return Error{app_data.error()};
  return ObsConfigRoot{*app_data, false, true};
}

ObsConfigRoot ObsConfigRootAt(const fs::path& folder) {
  const fs::path path = folder.has_filename() ? folder : folder.parent_path();
  const bool portable = AsciiLower(ToUtf8(path.filename())) == "config" && IsPortableInstall(path.parent_path());
  return {path, portable};
}

Result<ObsConfigRoot> ObsConfigRootFor(const fs::path& install_root, const std::optional<fs::path>& picked) {
  auto own = FindObsConfigRoot(install_root);
  if (!picked || (own && SameFolder(*picked, own->path))) return own;
  return ObsConfigRootAt(*picked);
}

fs::path SettingsFolderFor(const fs::path& folder) {
  const fs::path path = folder.has_filename() ? folder : folder.parent_path();
  if (AsciiLower(ToUtf8(path.filename())) == "obs-studio" && !Exists(path / L"obs-studio")) return path.parent_path();
  return path;
}

bool SameFolder(const fs::path& a, const fs::path& b) {
  if (a.empty() || b.empty()) return false;
  const auto plain = [](const fs::path& path) {
    std::wstring text = path.lexically_normal().native();
    while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/')) text.pop_back();
    return text;
  };
  const std::wstring x = plain(a);
  const std::wstring y = plain(b);
  return CompareStringOrdinal(x.data(), static_cast<int>(x.size()), y.data(), static_cast<int>(y.size()), TRUE) ==
         CSTR_EQUAL;
}

Result<ActiveObsConfig> FindActiveObsConfig(const ObsConfigRoot& root) {
  const std::string_view open_obs = root.obs_writes_here ? " Open OBS once and close it." : "";
  const fs::path global_file = root.path / L"obs-studio" / L"global.ini";
  if (!Exists(global_file)) {
    return Error{std::format("OBS has no settings in {}{}.{}", ToUtf8(root.path / L"obs-studio"),
                             root.obs_writes_here ? " yet" : "", open_obs)};
  }
  auto global = ObsIni::Read(global_file);
  if (!global) return Error{global.error()};

  ActiveObsConfig config;
  config.root = root;
  const fs::path user_file = Location(&*global, root, "Configuration") / L"obs-studio" / L"user.ini";
  std::optional<ObsIni> user;
  if (Exists(user_file)) {
    auto read = ObsIni::Read(user_file);
    if (!read) return Error{read.error()};
    user = std::move(*read);
    config.settings_file = user_file;
  } else {
    config.settings_file = global_file;
  }
  const ObsIni& settings = user ? *user : *global;
  const auto profile = settings.Get("Basic", "Profile");
  const auto collection = settings.Get("Basic", "SceneCollection");
  if (!profile || profile->empty() || !collection || collection->empty()) {
    return Error{std::format("{} doesn't name an active profile and scene collection.{}", ToUtf8(config.settings_file),
                             open_obs)};
  }
  config.profile = *profile;
  config.collection = *collection;
  config.scenes_dir = Location(&*global, root, "SceneCollections") / kScenesSubdir;

  // Profiles go by the Name in their basic.ini, or their folder's name.
  const fs::path profiles_dir = Location(&*global, root, "Profiles") / kProfilesSubdir;
  std::error_code ec;
  for (fs::directory_iterator it(profiles_dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_directory(ec)) continue;
    const fs::path basic_file = it->path() / L"basic.ini";
    if (!Exists(basic_file)) continue;
    auto basic = ObsIni::Read(basic_file);
    if (!basic) continue;
    const std::string name = basic->Get("General", "Name").value_or(ToUtf8(it->path().filename()));
    if (name != config.profile) continue;
    config.profile_dir = it->path();
    auto audio = ReadProfileAudio(*basic, basic_file);
    if (!audio) return Error{audio.error()};
    config.audio = std::move(*audio);
    return config;
  }
  return Error{std::format("OBS's active profile \"{}\" isn't in {}.", config.profile, ToUtf8(profiles_dir))};
}

Result<fs::path> FindSceneCollectionFile(const ActiveObsConfig& config, const CollectionNameReader& read_name) {
  std::error_code ec;
  for (fs::directory_iterator it(config.scenes_dir, ec), end; !ec && it != end; it.increment(ec)) {
    // OBS skips folders and compares the extension case-sensitively.
    if (it->is_directory(ec) || it->path().extension().native() != L".json") continue;
    std::string name = read_name(it->path());
    if (name.empty()) name = ToUtf8(it->path().stem());
    if (name == config.collection) return it->path();
  }
  return Error{std::format("OBS's active scene collection \"{}\" isn't in {}.", config.collection,
                           ToUtf8(config.scenes_dir))};
}

}  // namespace knobs::import
