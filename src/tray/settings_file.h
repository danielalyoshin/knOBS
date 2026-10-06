// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string>

#include "core/state.h"
#include "tray/first_run.h"
#include "util/result.h"

// knobs's settings, kept in %AppData%\knobs\settings.ini:
//
//   [OBS]
//   Install=D:\\Games\\obs-studio
//   Settings=D:\\Games\\obs-studio\\config
//   PauseWhileOpen=true
//   [Audio]
//   Mic=Mic/Aux
//   Cable={0.0.0.00000000}.{…}
//   CableName=CABLE In 16ch (VB-Audio Virtual Cable)
//   [Setup]
//   Done=false
//   Door=obs
//   Page=cable
//
// Keys left out keep their defaults (core::Settings, FirstRunProgress).
// Values are escaped as libobs's INI parser unescapes them (\\, \n, \r), and
// read with that parser (import::ObsIni).
namespace knobs::tray {

struct SavedSettings {
  core::Settings core;
  // How far the first run got: Door and Page while it's unfinished.
  FirstRunProgress first_run;

  friend bool operator==(const SavedSettings&, const SavedSettings&) = default;
};

std::string FormatSettings(const SavedSettings& settings);
SavedSettings ParseSettings(std::string_view text);

// Defaults if `file` doesn't exist; an error if it can't be read.
Result<SavedSettings> LoadSettings(const std::filesystem::path& file);
Status SaveSettings(const std::filesystem::path& file, const SavedSettings& settings);

}  // namespace knobs::tray
