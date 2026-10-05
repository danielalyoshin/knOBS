// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string>

#include "core/backend.h"
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
//
// Keys left out keep their defaults (core::Settings). Values are escaped as
// libobs's INI parser unescapes them (\\, \n, \r), and read with that parser
// (import::ObsIni).
namespace knobs::tray {

std::string FormatSettings(const core::Settings& settings);
core::Settings ParseSettings(std::string_view text);

// Defaults if `file` doesn't exist; an error if it can't be read.
Result<core::Settings> LoadSettings(const std::filesystem::path& file);
Status SaveSettings(const std::filesystem::path& file, const core::Settings& settings);

}  // namespace knobs::tray
