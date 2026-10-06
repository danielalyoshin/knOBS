// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include <media-io/audio-io.h>

#include "util/result.h"

// Finds OBS's active profile and scene collection the way OBS 32 does
// (frontend/OBSApp.cpp, OBSBasic_Profiles.cpp, OBSBasic_SceneCollections.cpp).
// Only reads: OBS's settings are read-only to knobs.
namespace knobs::import {

// The folder OBS keeps its settings in, the one holding obs-studio\.
struct ObsConfigRoot {
  std::filesystem::path path;
  bool portable = false;
  // OBS keeps its settings here, so opening it once fills what's missing.
  // Only FindObsConfigRoot knows that: a folder given directly may be one
  // OBS never writes to.
  bool obs_writes_here = false;
};

// %AppData%, or <install>\config for a portable install: one with a marker
// file such as portable_mode.txt next to bin\ (obs-main.cpp). OBS's
// --portable flag also turns portable mode on, which can't be seen from
// outside; pass that folder explicitly instead.
Result<ObsConfigRoot> FindObsConfigRoot(const std::filesystem::path& install_root);

// A settings folder given directly. It's portable if it's the config\ folder
// of a portable install, which ignores global.ini's [Locations].
ObsConfigRoot ObsConfigRootAt(const std::filesystem::path& folder);

// The settings folder to read for the install at `install_root`: `picked`,
// if there is one, else FindObsConfigRoot's. A picked folder stays picked,
// even while it can't be read. OBS's own folder picked, however it's spelled
// (SameFolder), is as good as no pick: OBS writes there.
Result<ObsConfigRoot> ObsConfigRootFor(const std::filesystem::path& install_root,
                                       const std::optional<std::filesystem::path>& picked);

// The settings folder someone means by picking `folder`: the folder holding
// obs-studio\, or, when they picked obs-studio\ itself, the one above it.
std::filesystem::path SettingsFolderFor(const std::filesystem::path& folder);

// Whether two folders are the same as Windows names them: ignoring case and
// a trailing separator. An empty path matches nothing.
bool SameFolder(const std::filesystem::path& a, const std::filesystem::path& b);

// A profile's [Audio] settings in basic.ini, with the defaults OBS uses for
// missing keys (OBSBasic.cpp, InitBasicConfigDefaults).
struct ProfileAudio {
  uint32_t sample_rate = 48000;
  std::string channel_setup = "Stereo";
  // channel_setup as OBSBasic::ResetAudio maps it.
  speaker_layout speakers = SPEAKERS_STEREO;
  // "default", or a Windows endpoint ID.
  std::string monitoring_device_id = "default";
  // As saved. Empty when missing; OBS shows a translated "Default" then.
  std::string monitoring_device_name;
};

struct ActiveObsConfig {
  ObsConfigRoot root;
  // user.ini, or global.ini for settings OBS 30 or older saved, which OBS 31
  // and later copy to user.ini when they first start.
  std::filesystem::path settings_file;
  std::string profile;  // Its name, as OBS shows it.
  std::filesystem::path profile_dir;
  ProfileAudio audio;
  std::string collection;  // Its name, as OBS shows it.
  std::filesystem::path scenes_dir;
};

// Finds the active profile and reads its audio settings. The scene
// collection is only named here: finding its file means reading JSON (see
// FindSceneCollectionFile). The errors suggest opening OBS once only for a
// folder OBS writes (ObsConfigRoot::obs_writes_here).
Result<ActiveObsConfig> FindActiveObsConfig(const ObsConfigRoot& root);

// A scene collection file's saved "name", or "" if it has none or doesn't
// parse.
using CollectionNameReader = std::function<std::string(const std::filesystem::path& file)>;

// The file of the active collection. OBS 32 goes by the name saved in each
// .json file in the scenes folder, or the file name when there's none, and
// the first file found wins (OBSBasic::RefreshSceneCollectionCache).
Result<std::filesystem::path> FindSceneCollectionFile(const ActiveObsConfig& config,
                                                      const CollectionNameReader& read_name);

}  // namespace knobs::import
