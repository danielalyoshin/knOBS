// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "audio/audio_devices.h"
#include "core/backend.h"
#include "core/state.h"

// The tray menu as data (plan.md, Tray and first run): what it says and
// offers for a snapshot of the core. tray_app.cpp turns it into a native
// menu. Text is UTF-8 menu text: "&" marks the key that picks an item, so a
// name's own "&" is doubled.
namespace knobs::tray {

// Menu item IDs. Mic and cable picks are numbered from kMicFirst and
// kCableFirst, in the order of Menu::mics and Menu::cables.
enum MenuId : unsigned {
  kIdStatus = 1,
  kIdPause,
  kIdResume,
  kIdMicMenu,
  kIdMicSameAsObs,
  kIdCableMenu,
  kIdCableSameAsObs,
  kIdOtherDevicesMenu,
  kIdReimport,
  kIdPauseForObs,
  kIdStartWithWindows,
  kIdSetup,
  kIdOpenLogs,
  kIdAbout,
  kIdQuit,
  // Fixes, shown in bold after the status line.
  kIdFinishSetup,
  kIdChooseMic,
  kIdChooseCable,
  kIdFindObs,
  kIdRestart,
  kIdTryAgain,
  kIdOtherObs,  // A line under the status: OBS open in another account.
  kIdMicFirst = 100,
  kIdCableFirst = 200,
  kIdCableLast = 299,
};

struct MenuItem {
  enum class Kind { kCommand, kSeparator, kSubmenu };

  Kind kind = Kind::kCommand;
  unsigned id = 0;  // Submenus have one too, to find them by.
  std::string text;
  bool enabled = true;
  bool checked = false;
  bool radio = false;  // Checked with a bullet: one of several choices.
  bool bold = false;   // The fix for what needs the user.
  std::vector<MenuItem> items;  // A submenu's.
};

struct Menu {
  std::vector<MenuItem> items;
  // What each pick sets: a mic's name for Settings::mic, and a playback
  // device for Settings::cable.
  std::vector<std::string> mics;
  std::vector<audio::AudioDevice> cables;
};

// The single line that says what knobs is doing, in the chain's short form:
// "Mic/Aux › 4 filters › CABLE In 16ch", "Paused: …", "Can't find OBS
// Studio". The menu's first line and the tooltip.
std::string StatusLine(const core::Snapshot& snapshot);

// "CABLE In 16ch (VB-Audio Virtual Cable)" -> "CABLE In 16ch": a playback
// device's name without the trailing name of its driver, as Windows' Sound
// settings shows it on its first line.
std::string ShortDeviceName(std::string_view name);

// The chain as the tray says it, with the cable's short name: in full, or in
// the short form the menu and tooltip use.
std::string ChainText(const core::ChainSummary& chain, bool short_form);

// The cable the mic goes to, by its short name, or "" until one is known.
std::string ShortCableName(const core::Snapshot& snapshot);

// Whether a playback device is a virtual cable (VB-Cable, VoiceMeeter, VB's
// Hi-Fi Cable, Virtual Audio Cable), going by its name.
bool IsVirtualCable(std::string_view name);

// The recording side of a virtual cable's playback device, which other apps
// choose as their mic, by its short name: CABLE Input and CABLE In 16ch go
// to CABLE Output, CABLE-A Input to CABLE-A Output, VoiceMeeter Input to
// VoiceMeeter Output. Empty when it isn't known.
std::string CableRecordingSide(std::string_view name);

Menu BuildMenu(const core::Snapshot& snapshot, const core::Settings& settings, bool start_with_windows);

// The menu item with `id`, anywhere in `items`, or null.
const MenuItem* FindItem(const std::vector<MenuItem>& items, unsigned id);

// The settings a mic or cable pick asks for, or nullopt if `id` isn't one.
std::optional<core::Settings> SettingsForPick(const Menu& menu, unsigned id, const core::Settings& settings);

}  // namespace knobs::tray
