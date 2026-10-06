// SPDX-License-Identifier: GPL-2.0-or-later
//
// Unit tests for the tray app's parts that don't need a window: the menu
// for each state, the settings file, the Run entry (on a scratch registry
// key), the single-instance lock, and starting OBS (with a stand-in).

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "app_info.h"
#include "core/state.h"
#include "test_harness.h"
#include "tray/autostart.h"
#include "tray/menu.h"
#include "tray/open_obs.h"
#include "tray/settings_file.h"
#include "tray/single_instance.h"
#include "util/win_strings.h"

namespace knobs::test {

// Started as obs64.exe: notes its working directory next to itself.
bool RunAsStandInObs() {
  wchar_t path[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
  const std::filesystem::path exe = path;
  if (AsciiLower(ToUtf8(exe.filename())) != "obs64.exe") return false;
  std::ofstream(exe.parent_path() / L"working-directory.txt", std::ios::binary)
      << ToUtf8(std::filesystem::current_path());
  return true;
}

}  // namespace knobs::test

namespace {

using namespace knobs;
using namespace knobs::tray;
using namespace std::chrono_literals;
using core::SetupNeed;
using core::State;
namespace fs = std::filesystem;

constexpr char kCable16Id[] = "{0.0.0.00000000}.{cable-16ch}";
constexpr char kCable16[] = "CABLE In 16ch (VB-Audio Virtual Cable)";
constexpr char kCableInputId[] = "{0.0.0.00000000}.{cable-input}";
constexpr char kCableInput[] = "CABLE Input (VB-Audio Virtual Cable)";
constexpr char kSpeakersId[] = "{0.0.0.00000000}.{speakers}";
constexpr char kSpeakers[] = "Speakers (Realtek(R) Audio)";

import::MicCandidate Mic(std::string name) {
  import::MicCandidate mic;
  mic.name = std::move(name);
  mic.device_id = "default";
  return mic;
}

// Running: Mic/Aux with four filters into CABLE In 16ch, as OBS monitors it.
core::Snapshot Running() {
  core::Snapshot snapshot;
  snapshot.state = State::kRunning;
  snapshot.chain = core::ChainSummary{"Mic/Aux", {"3-Band EQ", "Expander", "Compressor", "Limiter"}, kCable16};
  snapshot.mics = {Mic("Mic/Aux")};
  snapshot.picked_mic = 0;
  snapshot.chain_revision = 1;
  snapshot.obs_cable = {kCable16, kCable16Id};
  snapshot.outputs = {{kSpeakers, kSpeakersId}, {kCableInput, kCableInputId}, {kCable16, kCable16Id}};
  return snapshot;
}

std::vector<std::string> Texts(const std::vector<MenuItem>& items) {
  std::vector<std::string> texts;
  for (const MenuItem& item : items) texts.push_back(item.kind == MenuItem::Kind::kSeparator ? "-" : item.text);
  return texts;
}

// The bold item, which has to come right after the status line.
const MenuItem* Fix(const Menu& menu) {
  for (size_t i = 0; i < menu.items.size(); ++i) {
    if (menu.items[i].bold) return i == 1 ? &menu.items[i] : nullptr;
  }
  return nullptr;
}

// --- Status line --------------------------------------------------------------------

TEST(StatusLineFollowsTheState) {
  core::Snapshot snapshot = Running();
  CHECK(StatusLine(snapshot) == "Mic/Aux › 4 filters › CABLE In 16ch");
  snapshot.state = State::kPausedByUser;
  CHECK(StatusLine(snapshot) == "Paused: Mic/Aux › 4 filters › CABLE In 16ch");
  snapshot.state = State::kPausedForObs;
  CHECK(StatusLine(snapshot) == "Paused while OBS is open: Mic/Aux › 4 filters › CABLE In 16ch");
  snapshot.state = State::kMicMissing;
  CHECK(StatusLine(snapshot) == "Mic missing: Mic/Aux › 4 filters › CABLE In 16ch");
  snapshot.state = State::kCableMissing;
  CHECK(StatusLine(snapshot) == "Cable missing: Mic/Aux › 4 filters › CABLE In 16ch");
  snapshot.state = State::kNeedsSetup;
  snapshot.setup = SetupNeed::kCable;
  snapshot.chain->cable.clear();
  CHECK(StatusLine(snapshot) == "No cable chosen: Mic/Aux › 4 filters");
  snapshot.setup = SetupNeed::kPickMic;
  snapshot.chain.reset();
  snapshot.mics = {Mic("Mic/Aux"), Mic("Mic/Aux 2"), Mic("Podcast Mic")};
  CHECK(StatusLine(snapshot) == "OBS has 3 mics");
  snapshot.setup = SetupNeed::kNoMic;
  CHECK(StatusLine(snapshot) == "No mic in OBS yet");
  snapshot.setup = SetupNeed::kObsSettings;
  CHECK(StatusLine(snapshot) == "Can't read OBS's settings");
  snapshot.state = State::kObsMissing;
  CHECK(StatusLine(snapshot) == "Can't find OBS Studio");
  snapshot.state = State::kObsUnsupported;
  CHECK(StatusLine(snapshot) == "This version of OBS isn't supported");
  snapshot.state = State::kRestartNeeded;
  CHECK(StatusLine(snapshot) == std::format("{} needs to restart", kDisplayName));
  snapshot.state = State::kFailed;
  CHECK(StatusLine(snapshot) == "Stopped after an error");
  CHECK(StatusLine(core::Snapshot{}) == "Starting…");
}

TEST(DeviceNamesLoseTheirDriver) {
  CHECK(ShortDeviceName(kCable16) == "CABLE In 16ch");
  CHECK(ShortDeviceName(kSpeakers) == "Speakers");
  CHECK(ShortDeviceName("Line 1 (Virtual Audio Cable) ") == "Line 1");
  CHECK(ShortDeviceName("Mic (front) (USB Audio)") == "Mic (front)");
  CHECK(ShortDeviceName("CABLE Input") == "CABLE Input");
  CHECK(ShortDeviceName("(Unnamed)") == "(Unnamed)");
  CHECK(ShortDeviceName("Odd)") == "Odd)");
}

TEST(VirtualCablesAreToldApart) {
  CHECK(IsVirtualCable(kCable16));
  CHECK(IsVirtualCable("CABLE-A Input (VB-Audio Cable A)"));
  CHECK(IsVirtualCable("Voicemeeter Input (VB-Audio Voicemeeter VAIO)"));
  CHECK(IsVirtualCable("Line 1 (Virtual Audio Cable)"));
  CHECK(!IsVirtualCable(kSpeakers));
  CHECK(!IsVirtualCable("Headphones (Audient iD4)"));
}

TEST(CablesHaveARecordingSide) {
  CHECK(CableRecordingSide(kCableInput) == "CABLE Output");
  CHECK(CableRecordingSide(kCable16) == "CABLE Output");
  CHECK(CableRecordingSide("CABLE-A Input (VB-Audio Cable A)") == "CABLE-A Output");
  CHECK(CableRecordingSide("VoiceMeeter Input (VB-Audio VoiceMeeter VAIO)") == "VoiceMeeter Output");
  CHECK(CableRecordingSide("VoiceMeeter Aux Input (VB-Audio VoiceMeeter AUX VAIO)") == "VoiceMeeter Aux Output");
  // Virtual Audio Cable names both sides alike, and other devices have none.
  CHECK(CableRecordingSide("Line 1 (Virtual Audio Cable)").empty());
  CHECK(CableRecordingSide(kSpeakers).empty());
  CHECK(CableRecordingSide("CABLE Input").empty());
}

// --- Menu ---------------------------------------------------------------------------

TEST(MenuListsTheDesignedItems) {
  const Menu menu = BuildMenu(Running(), {}, true);
  CHECK(Texts(menu.items) ==
        (std::vector<std::string>{"Mic/Aux › 4 filters › CABLE In 16ch", "-", "&Pause", "&Mic", "&Cable",
                                  "Re-&import from OBS", "-", "Pause while &OBS is open", "Start with &Windows", "-",
                                  "&Setup…", "Open &log folder", "&About", "-", "&Quit"}));
  CHECK(!menu.items[0].enabled && menu.items[0].id == kIdStatus);
  CHECK(!Fix(menu));
  CHECK(FindItem(menu.items, kIdPauseForObs)->checked && FindItem(menu.items, kIdStartWithWindows)->checked);
  CHECK(!FindItem(BuildMenu(Running(), {.pause_for_obs = false}, false).items, kIdPauseForObs)->checked);
  CHECK(!FindItem(BuildMenu(Running(), {}, false).items, kIdStartWithWindows)->checked);

  core::Snapshot paused = Running();
  paused.state = State::kPausedByUser;
  paused.paused_by_user = true;
  const Menu paused_menu = BuildMenu(paused, {}, false);
  CHECK(FindItem(paused_menu.items, kIdResume) && !FindItem(paused_menu.items, kIdPause));
}

TEST(MenuOffersTheFixInBold) {
  const auto fix = [](core::Snapshot snapshot) {
    const Menu menu = BuildMenu(snapshot, {}, false);
    const MenuItem* item = Fix(menu);
    return item ? std::format("{} {}", item->id, item->text) : std::string("none");
  };
  core::Snapshot snapshot = Running();
  for (const State state : {State::kRunning, State::kPausedByUser, State::kPausedForObs, State::kMicMissing,
                            State::kObsUnsupported, State::kStarting}) {
    snapshot.state = state;
    CHECK(fix(snapshot) == "none");
  }
  snapshot.state = State::kObsMissing;
  CHECK(fix(snapshot) == std::format("{} &Find OBS…", +kIdFindObs));
  snapshot.state = State::kCableMissing;
  CHECK(fix(snapshot) == std::format("{} C&hoose a cable…", +kIdChooseCable));
  snapshot.state = State::kNeedsSetup;
  snapshot.setup = SetupNeed::kCable;
  CHECK(fix(snapshot) == std::format("{} C&hoose a cable…", +kIdChooseCable));
  snapshot.setup = SetupNeed::kPickMic;
  CHECK(fix(snapshot) == std::format("{} C&hoose a mic…", +kIdChooseMic));
  snapshot.setup = SetupNeed::kNoMic;
  CHECK(fix(snapshot) == std::format("{} &Finish setup…", +kIdFinishSetup));
  snapshot.setup = SetupNeed::kObsSettings;
  CHECK(fix(snapshot) == std::format("{} &Finish setup…", +kIdFinishSetup));
  snapshot.state = State::kRestartNeeded;
  CHECK(fix(snapshot) == std::format("{} Res&tart {}", +kIdRestart, kDisplayName));
  snapshot.state = State::kFailed;
  CHECK(fix(snapshot) == std::format("{} &Try again", +kIdTryAgain));
}

TEST(MenuMicStartsWithSameAsObs) {
  Menu menu = BuildMenu(Running(), {}, false);
  const MenuItem* mic = FindItem(menu.items, kIdMicMenu);
  CHECK(mic && mic->enabled &&
        Texts(mic->items) == (std::vector<std::string>{"&Same as OBS (Mic/Aux)", "-", "Mic/Aux"}));
  CHECK(mic->items[0].checked && mic->items[0].radio && !mic->items[2].checked);
  // Picked by name, it no longer follows OBS.
  menu = BuildMenu(Running(), {.mic = "Mic/Aux"}, false);
  mic = FindItem(menu.items, kIdMicMenu);
  CHECK(!mic->items[0].checked && mic->items[2].checked);

  // Several mics and none picked: nothing to be the same as.
  core::Snapshot several = Running();
  several.state = State::kNeedsSetup;
  several.setup = SetupNeed::kPickMic;
  several.mics = {Mic("Mic/Aux"), Mic("Mic/Aux 2"), Mic("Ben & Jo")};
  several.picked_mic.reset();
  menu = BuildMenu(several, {}, false);
  mic = FindItem(menu.items, kIdMicMenu);
  CHECK(Texts(mic->items) == (std::vector<std::string>{"Mic/Aux", "Mic/Aux 2", "Ben && Jo"}));
  CHECK(std::none_of(mic->items.begin(), mic->items.end(), [](const MenuItem& item) { return item.checked; }));
  CHECK(menu.mics == (std::vector<std::string>{"Mic/Aux", "Mic/Aux 2", "Ben & Jo"}));
  const auto picked = SettingsForPick(menu, kIdMicFirst + 2, {});
  CHECK(picked && picked->mic == "Ben & Jo");

  // A pick that's gone from the collection stays visible.
  menu = BuildMenu(several, {.mic = "Old Mic"}, false);
  mic = FindItem(menu.items, kIdMicMenu);
  CHECK(mic->items.back().text == "Old Mic (not in OBS)" && mic->items.back().checked && !mic->items.back().enabled);

  // Nothing imported yet: nothing to choose.
  core::Snapshot missing;
  missing.state = State::kObsMissing;
  CHECK(!FindItem(BuildMenu(missing, {}, false).items, kIdMicMenu)->enabled);
  CHECK(!FindItem(BuildMenu(core::Snapshot{}, {}, false).items, kIdCableMenu)->enabled);
}

TEST(MenuCableListsCablesFirst) {
  Menu menu = BuildMenu(Running(), {}, false);
  const MenuItem* cable = FindItem(menu.items, kIdCableMenu);
  CHECK(Texts(cable->items) == (std::vector<std::string>{"&Same as OBS (CABLE In 16ch)", "-", kCableInput, kCable16,
                                                         "-", "&Other devices"}));
  CHECK(cable->items[0].checked && !cable->items[2].checked && !cable->items[3].checked);
  CHECK(Texts(cable->items[5].items) == (std::vector<std::string>{kSpeakers}));
  CHECK(menu.cables.size() == 3 && menu.cables[2].id == kSpeakersId);
  const auto picked = SettingsForPick(menu, cable->items[2].id, {.mic = "Mic/Aux"});
  CHECK(picked && picked->cable == kCableInputId && picked->cable_name == kCableInput && picked->mic == "Mic/Aux");
  const auto same = SettingsForPick(menu, kIdCableSameAsObs, *picked);
  CHECK(same && same->cable.empty() && same->cable_name.empty());
  CHECK(!SettingsForPick(menu, kIdQuit, {}));

  // A pick of knobs's own, even the device OBS uses.
  menu = BuildMenu(Running(), {.cable = kCable16Id, .cable_name = kCable16}, false);
  cable = FindItem(menu.items, kIdCableMenu);
  CHECK(!cable->items[0].checked && cable->items[3].checked);

  // A pick that isn't connected stays visible.
  core::Snapshot unplugged = Running();
  unplugged.state = State::kCableMissing;
  menu = BuildMenu(unplugged, {.cable = "{0.0.0.00000000}.{cable-a}", .cable_name = "CABLE-A Input (VB-Audio Cable A)"},
                   false);
  cable = FindItem(menu.items, kIdCableMenu);
  const MenuItem& gone = cable->items[4];
  CHECK(gone.text == "CABLE-A Input (VB-Audio Cable A) (not connected)" && gone.checked && !gone.enabled);

  // And so does OBS's.
  unplugged.outputs.pop_back();
  menu = BuildMenu(unplugged, {}, false);
  cable = FindItem(menu.items, kIdCableMenu);
  CHECK(cable->items[0].text == "&Same as OBS (CABLE In 16ch, not connected)" && cable->items[0].checked &&
        cable->items[0].enabled);
}

TEST(MenuCableWhenOBSDoesntMonitorToACable) {
  // To the default device: knobs needs a cable, and OBS has none to offer.
  core::Snapshot snapshot = Running();
  snapshot.state = State::kNeedsSetup;
  snapshot.setup = SetupNeed::kCable;
  snapshot.obs_cable = {"Default", "default"};
  Menu menu = BuildMenu(snapshot, {}, false);
  CHECK(Texts(FindItem(menu.items, kIdCableMenu)->items) ==
        (std::vector<std::string>{kCableInput, kCable16, "-", "&Other devices"}));

  // To headphones: offered only while it's the choice.
  snapshot = Running();
  snapshot.obs_cable = {"Headphones (Audient iD4)", "{0.0.0.00000000}.{headphones}"};
  snapshot.outputs.push_back(snapshot.obs_cable);
  menu = BuildMenu(snapshot, {}, false);
  CHECK(FindItem(menu.items, kIdCableMenu)->items[0].text == "&Same as OBS (Headphones)");
  menu = BuildMenu(snapshot, {.cable = kCable16Id}, false);
  CHECK(!FindItem(menu.items, kIdCableSameAsObs));

  // No cable installed.
  snapshot = Running();
  snapshot.obs_cable = {"Default", "default"};
  snapshot.outputs = {{kSpeakers, kSpeakersId}};
  menu = BuildMenu(snapshot, {}, false);
  const MenuItem* cable = FindItem(menu.items, kIdCableMenu);
  CHECK(cable->items[0].text == "No virtual cable found" && !cable->items[0].enabled);
}

TEST(MenuEscapesAmpersands) {
  core::Snapshot snapshot = Running();
  snapshot.chain->mic = "Ben & Jo";
  CHECK(BuildMenu(snapshot, {}, false).items[0].text == "Ben && Jo › 4 filters › CABLE In 16ch");
}

// --- Settings file ------------------------------------------------------------------

TEST(SettingsRoundTrip) {
  SavedSettings saved;
  core::Settings& settings = saved.core;
  settings.obs_dir = fs::path(L"D:\\Games\\obs-studio");
  settings.obs_config = fs::path(L"D:\\Games\\obs-studio\\config\\Ö");
  settings.mic = "Mic\\Aux\nnew line";
  settings.cable = kCable16Id;
  settings.cable_name = kCable16;
  settings.pause_for_obs = false;
  saved.first_run = {.door = Door::kObsUser, .reached = FirstRunPage::kCable};
  const std::string text = FormatSettings(saved);
  CHECK(text.find("Install=D:\\\\Games\\\\obs-studio\n") != std::string::npos);
  CHECK(text.find("Mic=Mic\\\\Aux\\nnew line\n") != std::string::npos);
  CHECK(text.find("[Setup]\nDone=false\nDoor=obs\nPage=cable\n") != std::string::npos);
  CHECK(ParseSettings(text) == saved);

  CHECK(ParseSettings("") == SavedSettings{});
  CHECK(FormatSettings({}) == "[OBS]\nPauseWhileOpen=true\n\n[Audio]\n\n[Setup]\nDone=false\n");
  CHECK(ParseSettings(FormatSettings({})) == SavedSettings{});
  CHECK(!ParseSettings("[OBS]\nPauseWhileOpen=0\n").core.pause_for_obs);
  CHECK(ParseSettings("[OBS]\nPauseWhileOpen=maybe\n").core.pause_for_obs);
  // A name without its cable means nothing.
  CHECK(ParseSettings("[Audio]\nCableName=CABLE Input\n").core.cable_name.empty());

  // Finished, the first run keeps nothing else. Unfinished, it resumes only
  // at the first door's pages from the mic on.
  saved.first_run = {.done = true, .door = Door::kObsUser, .reached = FirstRunPage::kWarnings};
  CHECK(FormatSettings(saved).ends_with("[Setup]\nDone=true\n"));
  CHECK(ParseSettings(FormatSettings(saved)).first_run == FirstRunProgress{.done = true});
  CHECK((ParseSettings("[Setup]\nDoor=new\nPage=steps\n").first_run ==
         FirstRunProgress{.door = Door::kNewToObs, .reached = FirstRunPage::kMic}));
  CHECK(ParseSettings("[Setup]\nDoor=elsewhere\nPage=done\n").first_run ==
        FirstRunProgress{.reached = FirstRunPage::kDone});

  const fs::path folder = fs::temp_directory_path() / std::format(L"knobs-tests-{}-settings", GetCurrentProcessId());
  const fs::path file = folder / L"settings.ini";
  std::error_code ec;
  fs::remove_all(folder, ec);
  const auto missing = LoadSettings(file);
  CHECK(missing && *missing == SavedSettings{});
  CHECK(SaveSettings(file, saved).ok());
  const auto loaded = LoadSettings(file);
  CHECK(loaded && loaded->core == saved.core && loaded->first_run.done);
  settings.mic.clear();
  CHECK(SaveSettings(file, saved).ok());
  CHECK(LoadSettings(file)->core.mic.empty());
  CHECK(!fs::exists(folder / L"settings.ini.tmp"));
  fs::remove_all(folder, ec);
}

// --- Start with Windows -------------------------------------------------------------

TEST(StartWithWindowsUsesTheRunKey) {
  const std::wstring scratch = std::format(L"Software\\knobs-tests-{}", GetCurrentProcessId());
  RunEntry entry;
  entry.name = L"knobs";
  entry.command = L"\"C:\\Program Files\\knobs\\knobs.exe\" --startup";
  entry.run_key = scratch + L"\\Run";
  entry.approved_key = scratch + L"\\StartupApproved\\Run";

  CHECK(!StartsWithWindows(entry));
  CHECK(SetStartWithWindows(entry, true).ok());
  CHECK(StartsWithWindows(entry));
  // Another copy's entry isn't this one's.
  RunEntry moved = entry;
  moved.command = L"\"D:\\knobs\\knobs.exe\" --startup";
  CHECK(!StartsWithWindows(moved));
  RunEntry cased = entry;
  cased.command = L"\"c:\\program files\\knobs\\KNOBS.EXE\" --startup";
  CHECK(StartsWithWindows(cased));

  // Turned off in Task Manager, then on again in knobs.
  const BYTE off[12] = {3, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
  CHECK(RegSetKeyValueW(HKEY_CURRENT_USER, entry.approved_key.c_str(), entry.name.c_str(), REG_BINARY, off,
                        sizeof(off)) == ERROR_SUCCESS);
  CHECK(!StartsWithWindows(entry));
  CHECK(SetStartWithWindows(entry, true).ok());
  CHECK(StartsWithWindows(entry));
  const BYTE on[12] = {2};
  CHECK(RegSetKeyValueW(HKEY_CURRENT_USER, entry.approved_key.c_str(), entry.name.c_str(), REG_BINARY, on,
                        sizeof(on)) == ERROR_SUCCESS);
  CHECK(StartsWithWindows(entry));

  CHECK(SetStartWithWindows(entry, false).ok());
  CHECK(!StartsWithWindows(entry));
  CHECK(SetStartWithWindows(entry, false).ok());
  CHECK(RegDeleteTreeW(HKEY_CURRENT_USER, scratch.c_str()) == ERROR_SUCCESS);
}

// --- Open OBS -----------------------------------------------------------------------

TEST(OpenObsStartsInItsBinFolder) {
  // An install whose obs64.exe is this binary, which then only notes where
  // it was started (RunAsStandInObs).
  const fs::path root = fs::temp_directory_path() / std::format(L"knobs-tests-{}-obs", GetCurrentProcessId());
  const fs::path bin = root / L"bin" / L"64bit";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(bin);
  wchar_t self[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
  CHECK(fs::copy_file(self, bin / L"obs64.exe", ec));

  CHECK(OpenObs(root).ok());
  const fs::path note = bin / L"working-directory.txt";
  std::string started_in;
  for (int i = 0; i < 200 && started_in.empty(); ++i) {
    std::this_thread::sleep_for(25ms);
    std::ifstream file(note, std::ios::binary);
    started_in.assign(std::istreambuf_iterator<char>(file), {});
  }
  CHECK(!started_in.empty() && fs::equivalent(fs::path(FromUtf8(started_in)), bin, ec));
  CHECK(!OpenObs(root / L"elsewhere").ok());
  CHECK(!OpenObs({}).ok());

  // Gone once the stand-in has exited.
  for (int i = 0; i < 200 && fs::exists(root); ++i) {
    fs::remove_all(root, ec);
    if (fs::exists(root)) std::this_thread::sleep_for(25ms);
  }
  CHECK(!fs::exists(root));
}

// --- Single instance ----------------------------------------------------------------

TEST(OneCopyAtATime) {
  const std::wstring name = std::format(L"knobs-tests-{}-instance", GetCurrentProcessId());
  const auto try_elsewhere = [&name](std::chrono::milliseconds wait) {
    return std::async(std::launch::async, [&name, wait] {
      auto lock = SingleInstance::Acquire(name, wait);
      return lock.ok() && *lock != nullptr;
    });
  };
  auto first = SingleInstance::Acquire(name, 0ms);
  CHECK(first.ok() && *first);
  CHECK(!try_elsewhere(0ms).get());
  // A restart waits for the running copy to quit.
  auto waiting = try_elsewhere(5s);
  std::this_thread::sleep_for(100ms);
  first->reset();
  CHECK(waiting.get());
}

}  // namespace
