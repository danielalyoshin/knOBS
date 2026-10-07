// SPDX-License-Identifier: GPL-2.0-or-later
//
// Unit tests for the first run as data (src/tray/first_run.h): which page
// each state calls for on each door, what the pages say and offer, and how
// clicks and changes of state move the first run on.

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app_info.h"
#include "core/state.h"
#include "test_harness.h"
#include "tray/first_run.h"

namespace {

using namespace knobs;
using namespace knobs::tray;
using core::SetupNeed;
using core::State;
using Kind = FirstRunAction::Kind;
using Page = FirstRunPage;

constexpr char kMicId[] = "{0.0.1.00000000}.{audient-id4}";
constexpr char kHeadsetId[] = "{0.0.1.00000000}.{headset}";
constexpr char kCable16Id[] = "{0.0.0.00000000}.{cable-16ch}";
constexpr char kCable16[] = "CABLE In 16ch (VB-Audio Virtual Cable)";
constexpr char kCableInputId[] = "{0.0.0.00000000}.{cable-input}";
constexpr char kCableInput[] = "CABLE Input (VB-Audio Virtual Cable)";
constexpr char kSpeakersId[] = "{0.0.0.00000000}.{speakers}";
constexpr char kSpeakers[] = "Speakers (Realtek(R) Audio)";
constexpr char kHeadphonesId[] = "{0.0.0.00000000}.{headphones}";
constexpr char kHeadphones[] = "Headphones (Audient iD4)";

const std::vector<std::string> kFourFilters = {"Noise Suppression", "Noise Gate", "Compressor", "Limiter"};

import::MicCandidate Mic(std::string name, std::string device_id, std::vector<std::string> filters,
                         bool monitored = true) {
  import::MicCandidate mic;
  mic.name = std::move(name);
  mic.device_id = std::move(device_id);
  mic.filters = std::move(filters);
  mic.monitored = monitored;
  return mic;
}

// Running: Mic/Aux with four filters into CABLE In 16ch, as OBS monitors it.
core::Snapshot Running() {
  core::Snapshot snapshot;
  snapshot.state = State::kRunning;
  snapshot.chain = core::ChainSummary{"Mic/Aux", kFourFilters, kCable16};
  snapshot.mics = {Mic("Mic/Aux", kMicId, kFourFilters)};
  snapshot.picked_mic = 0;
  snapshot.chain_revision = 1;
  snapshot.obs_cable = {kCable16, kCable16Id};
  snapshot.outputs = {{kSpeakers, kSpeakersId}, {kCableInput, kCableInputId}, {kCable16, kCable16Id}};
  snapshot.inputs = {{"Microphone (Audient iD4)", kMicId}, {"Headset Microphone (USB)", kHeadsetId}};
  snapshot.default_input = kHeadsetId;
  snapshot.obs = {"C:\\Program Files\\obs-studio", {32, 2, 2}};
  return snapshot;
}

// Three mics, none picked.
core::Snapshot PickMic() {
  core::Snapshot snapshot = Running();
  snapshot.state = State::kNeedsSetup;
  snapshot.setup = SetupNeed::kPickMic;
  snapshot.chain.reset();
  snapshot.picked_mic.reset();
  snapshot.mics = {Mic("Desk Mic", "default", {}), Mic("Mic/Aux", kMicId, kFourFilters, false),
                   Mic("Podcast Mic", "{0.0.1.00000000}.{gone}", {"Gain"})};
  return snapshot;
}

// The first door's pages, from the welcome page.
FirstRun ObsUser(const core::Snapshot& snapshot) {
  FirstRun first_run({});
  first_run.Click(kButtonObsUser, 0, false, snapshot);
  return first_run;
}

std::vector<std::string> Texts(const std::vector<PageButton>& buttons) {
  std::vector<std::string> texts;
  for (const PageButton& button : buttons) texts.push_back(button.text);
  return texts;
}

const PageButton* FindButton(const PageView& view, int id) {
  const auto found = std::find_if(view.buttons.begin(), view.buttons.end(),
                                  [id](const PageButton& button) { return button.id == id; });
  return found == view.buttons.end() ? nullptr : &*found;
}

bool Contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// --- The welcome page ---------------------------------------------------------------

TEST(WelcomeSaysWhatWasFound) {
  const auto notes = [](const core::Snapshot& snapshot) {
    const PageView view = FirstRun({}).View(snapshot);
    CHECK(view.page == Page::kWelcome && view.command_links && view.buttons.size() == 2);
    return std::format("{} | {} | {}", view.buttons[0].text, view.buttons[1].text,
                       view.default_button == kButtonObsUser ? "obs" : "new");
  };
  core::Snapshot snapshot = Running();
  CHECK(notes(snapshot) ==
        "I set up my mic in OBS\nFound Mic/Aux with 4 filters. | I'm new to OBS\nSet up your mic's filters in OBS, "
        "step by step. | obs");
  snapshot.chain->filters.clear();
  snapshot.mics[0].filters.clear();
  CHECK(notes(snapshot).starts_with("I set up my mic in OBS\nMic/Aux has no filters yet. |"));
  CHECK(notes(snapshot).ends_with("| new"));
  CHECK(notes(PickMic()).starts_with("I set up my mic in OBS\nFound 3 mics: Desk Mic, Mic/Aux and Podcast Mic. |"));
  CHECK(notes(PickMic()).ends_with("| obs"));

  core::Snapshot missing;
  missing.state = State::kObsMissing;
  CHECK(Contains(notes(missing), "I'm new to OBS\nOBS Studio isn't installed."));
  CHECK(notes(missing).ends_with("| new"));
  core::Snapshot unsupported = missing;
  unsupported.state = State::kObsUnsupported;
  unsupported.obs = {"C:\\Program Files\\obs-studio", {33, 0, 0}};
  CHECK(Contains(notes(unsupported), "\nOBS 33.0.0 is installed, and "));
  CHECK(notes(unsupported).ends_with("| obs"));
  // Too old: no update will fix it, so not "yet".
  core::Snapshot too_old = unsupported;
  too_old.obs.version = {31, 1, 4};
  CHECK(Contains(notes(too_old), std::format("\nOBS 31.1.4 is installed, which is too old for {}.", kDisplayName)));
  CHECK(!Contains(notes(too_old), "yet"));
  core::Snapshot settings = missing;
  settings.state = State::kNeedsSetup;
  settings.setup = SetupNeed::kObsSettings;
  CHECK(Contains(notes(settings), "\nCan't read OBS's settings.") && notes(settings).ends_with("| new"));
  settings.setup = SetupNeed::kNoMic;
  CHECK(Contains(notes(settings), "\nOBS has no mic yet.") && notes(settings).ends_with("| new"));
  CHECK(Contains(notes(core::Snapshot{}), "\nLooking for OBS…"));

  const PageView view = FirstRun({}).View(Running());
  CHECK(view.icon == PageIcon::kKnob);
  CHECK(view.footer == std::format("{} isn't affiliated with or endorsed by the OBS Project.", kDisplayName));
}

// --- The first door -----------------------------------------------------------------

TEST(FirstDoorShowsOnlyWhatsNeeded) {
  // Everything's in place: straight to the last page.
  FirstRun first_run = ObsUser(Running());
  CHECK(first_run.progress().door == Door::kObsUser && first_run.Page(Running()) == Page::kDone);
  const PageView done = first_run.View(Running());
  CHECK(done.instruction == "Your mic is set up");
  CHECK(done.content.starts_with(
      "Mic/Aux › Noise Suppression › Noise Gate › Compressor › Limiter › CABLE In 16ch\n\n"
      "In Discord, Zoom and other apps, choose CABLE Output as your mic."));
  CHECK(Contains(done.content, "click ^ (Show hidden icons)"));
  CHECK(Contains(done.content, "It pauses while OBS is open"));
  CHECK(done.check == std::format("Start {} with Windows", kDisplayName) && done.checked);
  CHECK(done.default_button == kButtonDone);
  CHECK(!first_run.View(Running(), 0, false).checked);

  const FirstRunAction finish = first_run.Click(kButtonDone, 0, true, Running());
  CHECK(finish.kind == Kind::kFinish && finish.start_with_windows && first_run.progress().done);
  CHECK(first_run.Click(kButtonDone, 0, false, Running()).start_with_windows == false);

  // A first run offers Start with Windows checked, even while it's off. One
  // that was finished, opened again, offers it as it is, so that Done
  // doesn't turn it back on.
  const PageView first =
      FirstRun({.door = Door::kObsUser, .reached = Page::kDone}, std::nullopt, false).View(Running());
  CHECK(first.page == Page::kDone && first.checked);

  // Also on the way from a page a notification opened.
  const FirstRunProgress finished = {.done = true, .door = Door::kObsUser, .reached = Page::kDone};
  core::Snapshot warned = Running();
  warned.notes = {{true, "Filter \"ReaComp\" is a VST plugin."}};
  for (const bool on : {false, true}) {
    FirstRun again(finished, std::nullopt, on);
    PageView last = again.View(Running());
    CHECK(last.page == Page::kDone && last.checked == on);
    CHECK(again.Click(kButtonDone, 0, last.checked, Running()).start_with_windows == on);

    FirstRun warnings(finished, Page::kWarnings, on);
    CHECK(warnings.Page(warned) == Page::kWarnings);
    warnings.Click(kButtonNext, 0, false, warned);
    last = warnings.View(warned);
    CHECK(last.page == Page::kDone && last.checked == on);
  }

  // The last page says why the mic isn't running yet.
  core::Snapshot paused = Running();
  paused.state = State::kPausedForObs;
  paused.obs_running = true;
  CHECK(Contains(first_run.View(paused).content, "OBS is open now, so "));
  paused.settings.pause_for_obs = false;
  CHECK(!Contains(first_run.View(paused).content, "It pauses while OBS is open"));
  core::Snapshot missing = Running();
  missing.state = State::kMicMissing;
  missing.detail = "The recording device for \"Mic/Aux\" isn't connected.";
  CHECK(Contains(first_run.View(missing).content, "isn't connected. "));
}

TEST(SeveralMicsAskWhichOne) {
  core::Snapshot snapshot = PickMic();
  FirstRun first_run = ObsUser(snapshot);
  const PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kMic);
  CHECK(view.instruction == std::format("Which mic should {} run?", kDisplayName));
  CHECK(Texts(view.choices) ==
        (std::vector<std::string>{"Desk Mic: Default communications device (Headset Microphone (USB)), no filters",
                                  "Mic/Aux: Microphone (Audient iD4), 4 filters",
                                  "Podcast Mic: device not connected, 1 filter"}));
  // One with filters that OBS monitors, so sends to a cable.
  CHECK(view.default_choice == kChoiceFirst + 2);
  snapshot.mics[2].monitored = false;
  CHECK(ObsUser(snapshot).View(snapshot).default_choice == kChoiceFirst + 1);

  const FirstRunAction picked = first_run.Click(kButtonNext, kChoiceFirst + 2, false, snapshot);
  CHECK(picked.kind == Kind::kApply && picked.settings.mic == "Podcast Mic");
  CHECK(first_run.progress().reached == Page::kFilters);
  // Until the core has it, the state still asks for a mic.
  CHECK(first_run.Page(snapshot) == Page::kMic);

  // Picked, the page is a question answered.
  snapshot = Running();
  snapshot.mics = PickMic().mics;
  snapshot.picked_mic = 2;
  snapshot.chain = core::ChainSummary{"Podcast Mic", {"Gain"}, kCable16};
  snapshot.settings.mic = "Podcast Mic";
  CHECK(first_run.Page(snapshot) == Page::kDone);
  CHECK(ObsUser(snapshot).Page(snapshot) == Page::kMic);
  CHECK(ObsUser(snapshot).View(snapshot).default_choice == kChoiceFirst + 2);
  FirstRun again = ObsUser(snapshot);
  CHECK(again.Click(kButtonNext, kChoiceFirst + 2, false, snapshot).kind == Kind::kNone);

  // A pick that's gone from OBS.
  snapshot = PickMic();
  snapshot.settings.mic = "Old Mic";
  CHECK(Contains(ObsUser(snapshot).View(snapshot).content, "The one chosen before, \"Old Mic\", is no longer"));
  // There's no default device.
  snapshot.default_input.clear();
  CHECK(ObsUser(snapshot).View(snapshot).choices[0].text.starts_with(
      "Desk Mic: Default communications device (none connected)"));
}

TEST(NoFiltersOffersTheSteps) {
  core::Snapshot snapshot = Running();
  snapshot.chain->filters.clear();
  snapshot.mics[0].filters.clear();
  FirstRun first_run = ObsUser(snapshot);
  PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kFilters && view.instruction == "Mic/Aux has no filters yet");
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Continue without filters", "Show the steps"}));
  CHECK(view.default_button == kButtonShowSteps);

  CHECK(first_run.Click(kButtonShowSteps, 0, false, snapshot).kind == Kind::kNone);
  CHECK(first_run.Page(snapshot) == Page::kSteps);
  first_run.Click(kButtonBack, 0, false, snapshot);
  CHECK(first_run.Page(snapshot) == Page::kFilters);
  first_run.Click(kButtonSkipFilters, 0, false, snapshot);
  CHECK(first_run.Page(snapshot) == Page::kDone);
  CHECK(first_run.View(snapshot).content.starts_with("Mic/Aux › CABLE In 16ch\n\n"));

  // With OBS open, what it saved may be behind.
  snapshot.obs_running = true;
  CHECK(Contains(ObsUser(snapshot).View(snapshot).footer, "reads its settings again when OBS closes"));

  // No mic at all: OBS has to have one first.
  core::Snapshot none = Running();
  none.state = State::kNeedsSetup;
  none.setup = SetupNeed::kNoMic;
  none.chain.reset();
  none.mics.clear();
  none.picked_mic.reset();
  first_run = FirstRun({.door = Door::kObsUser, .reached = Page::kDone});
  view = first_run.View(none);
  CHECK(view.page == Page::kFilters && view.instruction == "OBS has no mic yet");
  CHECK(Contains(view.content, "open OBS's Settings › Audio, set Mic/Auxiliary Audio to your mic"));
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Open OBS", "Show the steps"}));
}

TEST(CableWhenOBSMonitorsToTheDefaultDevice) {
  core::Snapshot snapshot = Running();
  snapshot.state = State::kNeedsSetup;
  snapshot.setup = SetupNeed::kCable;
  snapshot.chain->cable.clear();
  snapshot.obs_cable = {"", "default"};
  FirstRun first_run = ObsUser(snapshot);
  const PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kCable && !view.waiting);
  CHECK(Contains(view.content, "default playback device, which is usually your speakers"));
  CHECK(Texts(view.choices) == (std::vector<std::string>{"CABLE Input", "CABLE In 16ch"}));
  CHECK(view.default_choice == kChoiceFirst);
  const FirstRunAction picked = first_run.Click(kButtonNext, kChoiceFirst + 1, false, snapshot);
  CHECK(picked.kind == Kind::kApply && picked.settings.cable == kCable16Id && picked.settings.cable_name == kCable16);
  CHECK(first_run.progress().reached == Page::kWarnings);
  // Still needed until the core has the cable.
  CHECK(first_run.Page(snapshot) == Page::kCable);
  core::Snapshot running = Running();
  running.settings = picked.settings;
  CHECK(first_run.Page(running) == Page::kDone);
}

TEST(CableAppearsAndTheFirstRunMovesOn) {
  core::Snapshot none = Running();
  none.state = State::kNeedsSetup;
  none.setup = SetupNeed::kCable;
  none.chain->cable.clear();
  none.obs_cable = {"", "default"};
  none.outputs = {{kSpeakers, kSpeakersId}};
  FirstRun first_run = ObsUser(none);
  const PageView view = first_run.View(none);
  CHECK(view.page == Page::kCable && view.waiting && view.instruction == "Install a virtual cable");
  CHECK(Contains(view.content, "<a href=\"https://vb-audio.com/Cable/\">VB-Cable</a>"));
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back"}));

  // VB-Cable installs: its one input appears, and that's the cable.
  core::Snapshot installed = none;
  installed.outputs.push_back({kCableInput, kCableInputId});
  const FirstRunAction picked = first_run.Changed(none, installed);
  CHECK(picked.kind == Kind::kApply && picked.settings.cable == kCableInputId);
  installed.settings = picked.settings;
  installed.state = State::kRunning;
  CHECK(first_run.Page(installed) == Page::kDone);
  // Back goes to it.
  first_run.Click(kButtonBack, 0, false, installed);
  CHECK(first_run.Page(installed) == Page::kCable);

  // Two at once: the choice is the user's.
  first_run = ObsUser(none);
  core::Snapshot two = none;
  two.outputs.push_back({kCableInput, kCableInputId});
  two.outputs.push_back({kCable16, kCable16Id});
  CHECK(first_run.Changed(none, two).kind == Kind::kNone);
  CHECK(first_run.Page(two) == Page::kCable && !first_run.View(two).waiting);
}

TEST(CableThatIsntACable) {
  core::Snapshot snapshot = Running();
  snapshot.obs_cable = {kHeadphones, kHeadphonesId};
  snapshot.outputs.push_back(snapshot.obs_cable);
  snapshot.chain->cable = kHeadphones;
  FirstRun first_run = ObsUser(snapshot);
  const PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kCable);
  CHECK(Contains(view.content, "OBS monitors your mic to Headphones, which isn't a virtual cable."));
  CHECK(Texts(view.choices) ==
        (std::vector<std::string>{"CABLE Input", "CABLE In 16ch", "Headphones (OBS's monitoring device)"}));
  CHECK(view.default_choice == kChoiceFirst + 2);
  // Keeping it is an answer, and no change.
  CHECK(first_run.Click(kButtonNext, kChoiceFirst + 2, false, snapshot).kind == Kind::kNone);
  CHECK(first_run.Page(snapshot) == Page::kDone);
  CHECK(first_run.View(snapshot).content.starts_with("Mic/Aux › Noise Suppression › Noise Gate › Compressor › "
                                                     "Limiter › Headphones\n\nIn Discord, Zoom and other apps, "
                                                     "choose the recording side of Headphones as your mic."));

  // A cable of knobs's own is no question.
  snapshot.settings.cable = kCable16Id;
  CHECK(ObsUser(snapshot).Page(snapshot) == Page::kDone);

  // No cable installed, but a device to keep.
  snapshot = Running();
  snapshot.obs_cable = {kHeadphones, kHeadphonesId};
  snapshot.outputs = {{kSpeakers, kSpeakersId}, snapshot.obs_cable};
  first_run = ObsUser(snapshot);
  CHECK(Texts(first_run.View(snapshot).buttons) == (std::vector<std::string>{"Back", "Keep Headphones"}));
  CHECK(first_run.Click(kButtonKeepDevice, 0, false, snapshot).kind == Kind::kNone);
  CHECK(first_run.Page(snapshot) == Page::kDone);
}

TEST(CableMissingAlwaysAsks) {
  core::Snapshot snapshot = Running();
  snapshot.state = State::kCableMissing;
  snapshot.outputs = {{kSpeakers, kSpeakersId}, {kCableInput, kCableInputId}};
  FirstRun first_run({.door = Door::kObsUser, .reached = Page::kDone});
  const PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kCable && view.content.starts_with("CABLE In 16ch isn't connected. Choose another cable."));
  CHECK(Texts(view.choices) == (std::vector<std::string>{"CABLE Input"}));
  const FirstRunAction picked = first_run.Click(kButtonNext, kChoiceFirst, false, snapshot);
  CHECK(picked.kind == Kind::kApply && picked.settings.cable == kCableInputId);
  // Back to OBS's own: no pick of knobs's.
  snapshot.settings = picked.settings;
  snapshot.outputs.push_back({kCable16, kCable16Id});
  first_run = FirstRun({.door = Door::kObsUser, .reached = Page::kDone}, Page::kCable);
  const PageView again = first_run.View(snapshot);
  CHECK(again.choices.back().text == "CABLE In 16ch (OBS's monitoring device)");
  const FirstRunAction obs = first_run.Click(kButtonNext, kChoiceFirst + 1, false, snapshot);
  CHECK(obs.kind == Kind::kApply && obs.settings.cable.empty() && obs.settings.cable_name.empty());
}

TEST(WarningsShowWhatChangesExpectations) {
  core::Snapshot snapshot = Running();
  snapshot.notes = {{true, "Filter \"VST\" is a VST plugin."},
                    {false, "Filter \"Gate\" is off in OBS, and stays off."},
                    {false, "The mic is on push-to-talk in OBS.", true}};
  FirstRun first_run = ObsUser(snapshot);
  PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kWarnings && view.icon == PageIcon::kWarning && view.instruction == "Before you start");
  CHECK(view.content == "• Filter \"VST\" is a VST plugin.\n\n• The mic is on push-to-talk in OBS.");
  first_run.Click(kButtonNext, 0, false, snapshot);
  CHECK(first_run.Page(snapshot) == Page::kDone && first_run.progress().reached == Page::kDone);

  snapshot.notes.erase(snapshot.notes.begin());
  view = ObsUser(snapshot).View(snapshot);
  CHECK(view.page == Page::kWarnings && view.icon == PageIcon::kInformation);
  snapshot.notes.pop_back();
  CHECK(ObsUser(snapshot).Page(snapshot) == Page::kDone);
}

TEST(ProblemsWithObsComeFirst) {
  core::Snapshot snapshot;
  CHECK(ObsUser(snapshot).Page(snapshot) == Page::kLooking && ObsUser(snapshot).View(snapshot).waiting);
  CHECK(ObsUser(snapshot).View(snapshot).content ==
        "Copying the parts of OBS needed to run, this may take a few seconds the first time.");

  snapshot.state = State::kObsMissing;
  snapshot.detail = "OBS Studio isn't installed.";
  FirstRun first_run = ObsUser(snapshot);
  PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kFindObs && view.content.starts_with("OBS Studio isn't installed.\n\nIf OBS is installed"));
  CHECK(Contains(view.content, "<a href=\"https://obsproject.com/download\">get OBS Studio</a>"));
  CHECK(view.default_button == kButtonChooseObs);
  CHECK(first_run.Click(kButtonChooseObs, 0, false, snapshot).kind == Kind::kChooseObs);
  CHECK(first_run.Click(kButtonTryAgain, 0, false, snapshot).kind == Kind::kReimport);

  snapshot.state = State::kObsUnsupported;
  snapshot.obs = {"C:\\Program Files\\obs-studio", {33, 0, 0}};
  view = ObsUser(snapshot).View(snapshot);
  CHECK(view.page == Page::kObsUnsupported && Contains(view.content, "supports OBS 33.0.0."));

  snapshot.state = State::kNeedsSetup;
  snapshot.setup = SetupNeed::kObsSettings;
  snapshot.obs = {"C:\\Program Files\\obs-studio", {32, 2, 2}};
  first_run = ObsUser(snapshot);
  view = first_run.View(snapshot);
  CHECK(view.page == Page::kObsSettings);
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Try again", "Choose folder…", "Open OBS"}));
  CHECK(view.default_button == kButtonOpenObs);
  CHECK(first_run.Click(kButtonOpenObs, 0, false, snapshot).kind == Kind::kOpenObs);
  CHECK(first_run.Click(kButtonChooseObsSettings, 0, false, snapshot).kind == Kind::kChooseObsSettings);
  // Settings put there some other way, or a folder that's back.
  CHECK(first_run.Click(kButtonTryAgain, 0, false, snapshot).kind == Kind::kReimport);
  snapshot.obs_running = true;
  view = first_run.View(snapshot);
  CHECK(!FindButton(view, kButtonOpenObs)->enabled && view.default_button == kButtonChooseObsSettings);

  snapshot = Running();
  snapshot.state = State::kRestartNeeded;
  first_run = ObsUser(snapshot);
  view = first_run.View(snapshot);
  CHECK(view.page == Page::kProblem && view.instruction == std::format("{} needs to restart", kDisplayName));
  CHECK(first_run.Click(kButtonRestart, 0, false, snapshot).kind == Kind::kRestart);
  snapshot.state = State::kFailed;
  CHECK(ObsUser(snapshot).View(snapshot).instruction == "Stopped after an error");
  // Even a page that was answered waits for them.
  CHECK(FirstRun({.door = Door::kObsUser, .reached = Page::kDone}, Page::kCable).Page(snapshot) == Page::kProblem);
}

TEST(APickedFolderThatStoppedWorkingIsTheUsersToChange) {
  // OBS gone from the folder picked for it: pick another, or look for OBS
  // as with no pick. Nothing is dropped until the user says.
  core::Snapshot snapshot;
  snapshot.state = State::kObsMissing;
  snapshot.detail = "D:\\OBS isn't a complete OBS Studio install: bin\\64bit\\obs.dll is missing.";
  snapshot.settings.obs_dir = "D:\\OBS";
  FirstRun first_run = ObsUser(snapshot);
  PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kFindObs && Contains(view.content, "OBS isn't in the folder chosen for it."));
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Try again", "Look for OBS", "Choose folder…"}));
  CHECK(view.default_button == kButtonChooseObs);
  FirstRunAction action = first_run.Click(kButtonLookForObs, 0, false, snapshot);
  CHECK(action.kind == Kind::kApply && !action.settings.obs_dir);
  snapshot.settings.obs_dir.reset();
  view = ObsUser(snapshot).View(snapshot);
  CHECK(!FindButton(view, kButtonLookForObs));

  // The same for OBS's settings: pick another folder, or use OBS's own.
  // Opening OBS wouldn't fill a folder elsewhere, so it isn't offered.
  snapshot.state = State::kNeedsSetup;
  snapshot.setup = SetupNeed::kObsSettings;
  snapshot.obs = {"C:\\Program Files\\obs-studio", {32, 2, 2}};
  snapshot.detail = "OBS has no settings in E:\\Portable\\config\\obs-studio.";
  snapshot.settings.obs_config = "E:\\Portable\\config";
  snapshot.obs_writes_config = false;
  first_run = ObsUser(snapshot);
  view = first_run.View(snapshot);
  CHECK(view.page == Page::kObsSettings && Contains(view.content, "aren't in the folder chosen for them."));
  CHECK(Texts(view.buttons) ==
        (std::vector<std::string>{"Back", "Try again", "Use OBS's own folder", "Choose folder…"}));
  CHECK(view.default_button == kButtonChooseObsSettings && view.footer.empty());
  // The same folder again, such as on a drive plugged back in.
  CHECK(first_run.Click(kButtonTryAgain, 0, false, snapshot).kind == Kind::kReimport);
  action = first_run.Click(kButtonOwnObsSettings, 0, false, snapshot);
  CHECK(action.kind == Kind::kApply && !action.settings.obs_config);
  // OBS's own folder picked, however it's spelled (the core says so): as
  // with no pick, where opening OBS once does fill it.
  snapshot.settings.obs_config = "c:\\users\\YOU\\appdata\\roaming\\";
  snapshot.obs_writes_config = true;
  view = ObsUser(snapshot).View(snapshot);
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Try again", "Choose folder…", "Open OBS"}));
  CHECK(view.default_button == kButtonOpenObs && Contains(view.content, "If your OBS keeps its settings"));
  snapshot.settings.obs_config.reset();
  view = ObsUser(snapshot).View(snapshot);
  CHECK(!FindButton(view, kButtonOwnObsSettings) && FindButton(view, kButtonOpenObs));
}

TEST(AnUnsupportedObsOffersAnotherFolder) {
  // OBS updated past what knobs supports: install a supported one, choose
  // one installed elsewhere, or wait for knobs.
  core::Snapshot snapshot;
  snapshot.state = State::kObsUnsupported;
  snapshot.obs = {"C:\\Program Files\\obs-studio", {33, 0, 0}};
  snapshot.detail = "OBS 33.0.0 isn't supported.";
  FirstRun first_run = ObsUser(snapshot);
  PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kObsUnsupported);
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Try again", "Choose folder…"}));
  CHECK(view.default_button == kButtonTryAgain);
  CHECK(Contains(view.content, "If a supported OBS is installed somewhere else, choose that folder."));
  CHECK(view.content.ends_with(std::format("Or wait for a {} update that supports OBS 33.0.0.", kDisplayName)));
  CHECK(first_run.Click(kButtonChooseObs, 0, false, snapshot).kind == Kind::kChooseObs);
  CHECK(first_run.Click(kButtonTryAgain, 0, false, snapshot).kind == Kind::kReimport);

  // A folder picked for OBS that holds an older one stays picked, with the
  // ways back. No knobs update supports an older OBS.
  snapshot.settings.obs_dir = "D:\\OBS 31";
  snapshot.obs = {"D:\\OBS 31", {31, 1, 4}};
  snapshot.detail = "OBS 31.1.4 isn't supported.";
  first_run = ObsUser(snapshot);
  view = first_run.View(snapshot);
  CHECK(view.page == Page::kObsUnsupported && Contains(view.content, "the folder chosen for it, D:\\OBS 31."));
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Try again", "Look for OBS", "Choose folder…"}));
  CHECK(view.default_button == kButtonChooseObs && !Contains(view.content, "wait"));
  const FirstRunAction action = first_run.Click(kButtonLookForObs, 0, false, snapshot);
  CHECK(action.kind == Kind::kApply && !action.settings.obs_dir);
  CHECK(first_run.Click(kButtonChooseObs, 0, false, snapshot).kind == Kind::kChooseObs);
}

// --- The second door ----------------------------------------------------------------

TEST(SecondDoorListsTheStepsInOBSWords) {
  core::Snapshot snapshot = Running();
  snapshot.chain->filters.clear();
  snapshot.mics[0].filters.clear();
  FirstRun first_run({});
  first_run.Click(kButtonNewToObs, 0, false, snapshot);
  PageView view = first_run.View(snapshot);
  CHECK(view.page == Page::kSteps && view.instruction == "Set up your mic in OBS");
  // As OBS 32.2.2 names them (docs/design.md, Tray and first run).
  CHECK(Contains(view.content, "\n1. Open OBS.\n"));
  CHECK(Contains(view.content, "\n2. In the Audio Mixer, click Mic/Aux and choose Filters.\n"));
  CHECK(Contains(view.content, "\n3. Under Audio Filters, click + and add filters as needed.\n"));
  CHECK(Contains(view.content, "\n4. Click each filter and adjust it by ear, starting from OBS's defaults.\n"));
  CHECK(Contains(view.content, "\n5. Close OBS. "));
  CHECK(Contains(view.footer, std::format("<a href=\"{}\">", kSetupGuideUrl)));
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Open OBS", "Next"}));
  CHECK(FindButton(view, kButtonOpenObs)->enabled && !FindButton(view, kButtonNext)->enabled);
  CHECK(view.default_button == kButtonOpenObs);
  CHECK(first_run.Click(kButtonOpenObs, 0, false, snapshot).kind == Kind::kOpenObs);

  // OBS open: knobs waits for it to close.
  core::Snapshot open = snapshot;
  open.state = State::kPausedForObs;
  open.obs_running = true;
  view = first_run.View(open);
  CHECK(!FindButton(view, kButtonOpenObs)->enabled && view.footer.starts_with("OBS is open."));
  // Closed with no filters added: still here.
  CHECK(first_run.Changed(open, snapshot).kind == Kind::kNone && first_run.Page(snapshot) == Page::kSteps);
  // Closed with filters: on to the first door's pages.
  core::Snapshot tuned = Running();
  first_run.Changed(open, tuned);
  CHECK(first_run.progress().door == Door::kObsUser && first_run.Page(tuned) == Page::kDone);
  first_run.Click(kButtonBack, 0, false, tuned);
  CHECK(first_run.Page(tuned) == Page::kSteps);
  // Filters there already, and OBS closed: Next goes on.
  view = first_run.View(tuned);
  CHECK(FindButton(view, kButtonNext)->enabled);
  first_run.Click(kButtonNext, 0, false, tuned);
  CHECK(first_run.Page(tuned) == Page::kDone);
  first_run.Click(kButtonBack, 0, false, tuned);
  first_run.Click(kButtonBack, 0, false, tuned);
  CHECK(first_run.Page(tuned) == Page::kWelcome);
}

TEST(SecondDoorWithoutOBSOrAMic) {
  core::Snapshot missing;
  missing.state = State::kObsMissing;
  PageView view = FirstRun({}, Page::kSteps).View(missing);
  CHECK(Contains(view.content, "\n1. Install OBS Studio 32.2 or a later 32.x release from <a "
                               "href=\"https://obsproject.com/download\">obsproject.com</a>, and open it.\n"));
  CHECK(Contains(view.content, "\n2. In Settings › Audio, set Mic/Auxiliary Audio to your mic. Then, in the Audio "
                               "Mixer, click Mic/Aux and choose Filters.\n"));
  CHECK(Texts(view.buttons) == (std::vector<std::string>{"Back", "Next"}));

  core::Snapshot unsupported = missing;
  unsupported.state = State::kObsUnsupported;
  unsupported.obs = {"C:\\Program Files\\obs-studio", {33, 0, 0}};
  view = FirstRun({}, Page::kSteps).View(unsupported);
  CHECK(Contains(view.content, "OBS's releases on GitHub</a>, and open it."));
  CHECK(Contains(view.content, " doesn't support OBS 33.0.0 yet."));
  unsupported.obs.version = {31, 1, 4};
  view = FirstRun({}, Page::kSteps).View(unsupported);
  CHECK(Contains(view.content, std::format("and open it. OBS 31.1.4 is too old for {}.", kDisplayName)));
  CHECK(!Contains(view.content, "yet"));

  // Several mics, none picked: whichever one they mean.
  CHECK(Contains(FirstRun({}, Page::kSteps).View(PickMic()).content, "click your mic and choose Filters"));
}

// --- Resuming, Back and pages asked for -----------------------------------------

TEST(ResumesWhereItStopped) {
  core::Snapshot snapshot = Running();
  snapshot.mics = PickMic().mics;
  snapshot.picked_mic = 1;
  snapshot.settings.mic = "Mic/Aux";
  snapshot.notes = {{false, "The mic is muted in OBS.", true}};
  // The mic was answered; the warnings weren't.
  CHECK(FirstRun({.door = Door::kObsUser, .reached = Page::kWarnings}).Page(snapshot) == Page::kWarnings);
  CHECK(FirstRun({.door = Door::kObsUser, .reached = Page::kMic}).Page(snapshot) == Page::kMic);
  CHECK(FirstRun({.door = Door::kNewToObs}).Page(snapshot) == Page::kSteps);
  // An answered page the state calls for again.
  snapshot.state = State::kNeedsSetup;
  snapshot.setup = SetupNeed::kPickMic;
  CHECK(FirstRun({.door = Door::kObsUser, .reached = Page::kDone}).Page(snapshot) == Page::kMic);
  // From the start.
  CHECK(FirstRun({.done = true}).Page(snapshot) == Page::kWelcome);
}

TEST(BackRetracesThePages) {
  core::Snapshot snapshot = PickMic();
  snapshot.obs_cable = {"", "default"};
  FirstRun first_run = ObsUser(snapshot);
  CHECK(first_run.Page(snapshot) == Page::kMic);
  first_run.Click(kButtonNext, kChoiceFirst + 1, false, snapshot);
  // The core picks Mic/Aux, and needs a cable.
  snapshot.state = State::kNeedsSetup;
  snapshot.setup = SetupNeed::kCable;
  snapshot.picked_mic = 1;
  snapshot.chain = core::ChainSummary{"Mic/Aux", kFourFilters, ""};
  snapshot.settings.mic = "Mic/Aux";
  CHECK(first_run.Page(snapshot) == Page::kCable);
  first_run.Click(kButtonBack, 0, false, snapshot);
  CHECK(first_run.Page(snapshot) == Page::kMic && first_run.View(snapshot).default_choice == kChoiceFirst + 1);
  first_run.Click(kButtonBack, 0, false, snapshot);
  CHECK(first_run.Page(snapshot) == Page::kWelcome && first_run.progress().door == Door::kNone);
  // With nothing to go back to, Back is the welcome page.
  first_run = FirstRun({.door = Door::kObsUser, .reached = Page::kCable});
  first_run.Click(kButtonBack, 0, false, snapshot);
  CHECK(first_run.Page(snapshot) == Page::kWelcome);
}

TEST(AnyPageCanBeAskedFor) {
  for (const Page page : {Page::kWelcome, Page::kSteps, Page::kLooking, Page::kFindObs, Page::kObsUnsupported,
                          Page::kObsSettings, Page::kProblem, Page::kMic, Page::kFilters, Page::kCable,
                          Page::kWarnings, Page::kDone}) {
    CHECK(PageNamed(PageName(page)) == page);
  }
  CHECK(!PageNamed("elsewhere"));
  // A page the state doesn't call for is still shown when asked for.
  FirstRun first_run({}, Page::kCable);
  CHECK(first_run.Page(Running()) == Page::kCable);
  CHECK(Contains(first_run.View(Running()).content, " sends your mic to CABLE In 16ch."));
  first_run.Click(kButtonNext, kChoiceFirst + 1, false, Running());
  CHECK(first_run.Page(Running()) == Page::kDone);
  CHECK(FirstRun({}, Page::kMic).Page(Running()) == Page::kMic);
  // One that doesn't apply gives way.
  CHECK(FirstRun({}, Page::kWarnings).Page(Running()) == Page::kDone);
  CHECK(FirstRun({}, Page::kFindObs).Page(Running()) == Page::kDone);
}

}  // namespace
