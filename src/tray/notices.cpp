// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/notices.h"

#include <algorithm>
#include <format>
#include <utility>

#include "app_info.h"
#include "tray/menu.h"
#include "util/win_strings.h"

namespace knobs::tray {
namespace {

using core::SetupNeed;
using core::State;
using Kind = Notice::Kind;

using audio::FindById;
using audio::kDefaultDevice;
using audio::SameId;

// What a balloon's title and text hold, less their terminators
// (NOTIFYICONDATAW::szInfoTitle and szInfo).
constexpr size_t kMaxTitle = 63;
constexpr size_t kMaxText = 255;
// About what Windows 11 shows of a notification's text: four lines. It clips
// the rest with no "…".
constexpr size_t kShownText = 160;

bool Connected(const std::vector<audio::AudioDevice>& devices, std::string_view id) {
  return FindById(devices, id) != nullptr;
}

const import::MicCandidate* PickedMic(const core::Snapshot& snapshot) {
  if (!snapshot.picked_mic || *snapshot.picked_mic >= snapshot.mics.size()) return nullptr;
  return &snapshot.mics[*snapshot.picked_mic];
}

// The cable gets no mic, and not because the user paused knobs or OBS is
// open: the mic or cable missing, or a problem that stops knobs.
bool Stopped(State state) {
  switch (state) {
    case State::kStarting:
    case State::kRunning:
    case State::kPausedByUser:
    case State::kPausedForObs:
      return false;
    default:
      return true;
  }
}

// A problem other than a missing device.
bool OtherProblem(State state) {
  return Stopped(state) && state != State::kMicMissing && state != State::kCableMissing;
}

// What a warning is about, so renaming its filter in OBS isn't a new warning.
const std::string& WarningKey(const import::ImportNote& note) {
  return note.key.empty() ? note.text : note.key;
}

// --- Copy ---------------------------------------------------------------------------

// Whether the picked mic's recording device is connected: if the mic is
// missing all the same, its chain stalled.
bool MicConnected(const core::Snapshot& snapshot) {
  const import::MicCandidate* picked = PickedMic(snapshot);
  if (!picked) return false;
  if (SameId(picked->device_id, kDefaultDevice)) return !snapshot.default_input.empty();
  return Connected(snapshot.inputs, picked->device_id);
}

Notice MicMissing(const core::Snapshot& snapshot) {
  Notice notice{.kind = Kind::kMicMissing, .title = "Mic missing", .problem = true, .sound = true};
  const std::string mic = snapshot.chain ? snapshot.chain->mic : "The mic";
  const import::MicCandidate* picked = PickedMic(snapshot);
  const bool is_default = picked && SameId(picked->device_id, kDefaultDevice);
  if (MicConnected(snapshot)) {
    // The core's watchdog saw no audio (docs/design.md, Devices and the
    // watchdog).
    notice.text = std::format("{} stopped sending audio. {} keeps trying to start it again.", mic, kDisplayName);
  } else if (is_default) {
    notice.text = std::format("There's no recording device. {} starts again when one is connected.", kDisplayName);
  } else {
    notice.text = std::format("The recording device for {} isn't connected. {} starts again when it's back.", mic,
                              kDisplayName);
  }
  return notice;
}

Notice CableMissing(const core::Snapshot& snapshot) {
  std::string cable = ShortCableName(snapshot);
  if (cable.empty()) cable = "The cable";
  return {.kind = Kind::kCableMissing,
          .title = "Cable missing",
          .text = std::format("{} isn't connected. {} starts again when it's back, or click to choose another "
                              "cable.",
                              cable, kDisplayName),
          .problem = true,
          .sound = true,
          .page = FirstRunPage::kCable};
}

// For a problem other than a missing device. `before` is the OBS version
// knobs knew before, if any.
std::optional<Notice> ProblemNotice(const core::Snapshot& snapshot, const runtime::ObsVersion& before) {
  Notice notice{.problem = true, .sound = true};
  switch (snapshot.state) {
    case State::kObsUnsupported: {
      const std::string version = snapshot.obs.version.ToString();
      notice.kind = Kind::kObsUnsupported;
      notice.page = FirstRunPage::kObsUnsupported;
      // Only an OBS newer than knobs supports is "not yet" supported.
      const bool newer = runtime::IsNewerThanSupportedObs(snapshot.obs.version);
      if (before != runtime::ObsVersion{} && before != snapshot.obs.version) {
        notice.title = before < snapshot.obs.version ? std::format("OBS was updated to {}", version)
                                                     : std::format("OBS changed to {}", version);
        notice.text = newer ? std::format("{} doesn't support it yet, so it stopped. Click for what you can do.",
                                          kDisplayName)
                            : std::format("It's too old for {}, so {} stopped. Click for what you can do.",
                                          kDisplayName, kDisplayName);
      } else {
        notice.title = "This version of OBS isn't supported";
        notice.text = newer ? std::format("{} doesn't support OBS {} yet, so it stopped. Click for what you can do.",
                                          kDisplayName, version)
                            : std::format("OBS {} is too old for {}, so it stopped. Click for what you can do.",
                                          version, kDisplayName);
      }
      return notice;
    }
    case State::kObsMissing:
      notice.kind = Kind::kObsMissing;
      notice.page = FirstRunPage::kFindObs;
      notice.title = "Can't find OBS Studio";
      notice.text = std::format("{0} runs your mic through OBS's filters, so it stopped. Click to show {0} where "
                                "OBS is.",
                                kDisplayName);
      return notice;
    case State::kNeedsSetup:
      notice.kind = Kind::kNeedsSetup;
      switch (snapshot.setup) {
        case SetupNeed::kObsSettings:
          notice.page = FirstRunPage::kObsSettings;
          notice.title = "Can't read OBS's settings";
          notice.text = std::format("{} reads your mic from them, so it stopped. Click for what to do.",
                                    kDisplayName);
          return notice;
        case SetupNeed::kNoMic:
          notice.page = FirstRunPage::kFilters;
          notice.title = "No mic in OBS";
          notice.text = std::format("OBS's scene collection has no mic now, so {} stopped. Click for what to do.",
                                    kDisplayName);
          return notice;
        case SetupNeed::kPickMic:
          notice.page = FirstRunPage::kMic;
          notice.title = std::format("OBS has {} mics", snapshot.mics.size());
          notice.text =
              snapshot.settings.mic.empty()
                  ? std::format("{} stopped until you choose the one it runs. Click to choose.", kDisplayName)
                  : std::format("{} is no longer in OBS, so {} stopped. Click to choose a mic.",
                                snapshot.settings.mic, kDisplayName);
          return notice;
        case SetupNeed::kCable:
          notice.page = FirstRunPage::kCable;
          notice.title = "No cable chosen";
          notice.text = std::format("OBS now monitors to its default device, usually speakers, so {} stopped. "
                                    "Click to choose a cable.",
                                    kDisplayName);
          return notice;
        case SetupNeed::kNone:
          break;
      }
      return std::nullopt;
    case State::kFailed:
      notice.kind = Kind::kFailed;
      notice.page = FirstRunPage::kProblem;
      notice.title = std::format("{} stopped after an error", kDisplayName);
      notice.text = snapshot.detail.empty() ? "Click for what to do." : snapshot.detail + " Click for what to do.";
      return notice;
    default:
      // A restart that's needed: the tray restarts by itself.
      return std::nullopt;
  }
}

// A text's length as a balloon holds it (kMaxText): in UTF-16 units.
size_t Length(std::string_view text) { return FromUtf8(text).size(); }

Notice ChainChanged(const core::Snapshot& snapshot, std::string_view mic_before) {
  const std::string& mic = snapshot.chain->mic;
  std::string text = std::format("{} now runs: {}", kDisplayName, ChainText(*snapshot.chain, false));
  // A chain too long to show whole: the short form, as the menu has it.
  if (Length(text) > kShownText) text = std::format("{} now runs: {}", kDisplayName, ChainText(*snapshot.chain, true));
  return {.kind = Kind::kChainChanged,
          .title = mic_before.empty() || mic == mic_before ? std::format("{} changed in OBS", mic)
                                                           : std::format("OBS's mic is now {}", mic),
          .text = std::move(text)};
}

// `text`, cut after a word or a line and ended with "…" to fit in `length`.
std::string Shortened(std::string_view text, size_t length) {
  std::wstring wide = FromUtf8(text);
  if (wide.size() <= length) return std::string(text);
  const size_t space = wide.find_last_of(L" \n", length - 1);
  wide.resize(space == std::wstring::npos ? length - 1 : space);
  while (!wide.empty() && std::wstring_view(L",.;:").find(wide.back()) != std::wstring_view::npos) wide.pop_back();
  return ToUtf8(wide) + "…";
}

Notice FilterWarnings(const core::Snapshot& snapshot, const std::vector<std::string>& added) {
  Notice notice{.kind = Kind::kFilterWarnings, .sound = true, .page = FirstRunPage::kWarnings};
  const std::string& mic = snapshot.chain->mic;
  notice.title = added.size() == 1 ? std::format("{} has a filter {} can't run", mic, kDisplayName)
                                   : std::format("{} has filters {} can't run", mic, kDisplayName);
  if (added.size() == 1 && Length(added.front()) <= kShownText) {
    notice.text = added.front();
    return notice;
  }
  // Windows shows only the start of a long text, so the first line says
  // what a click shows. The warnings follow, each on a line of its own: those
  // that fit whole, or as much of the first as fits.
  notice.text = added.size() == 1 ? std::string("Click to see it in full.")
                                  : std::format("{} warnings. Click to see them all.", added.size());
  for (size_t i = 0; i < added.size(); ++i) {
    std::string text = notice.text + "\n" + added[i];
    if (Length(text) > kMaxText) {
      if (i == 0) notice.text = Shortened(text, kMaxText);
      break;
    }
    notice.text = std::move(text);
  }
  return notice;
}

// Says what OBS in another session means for the mic, not just that it's
// open: knobs can't tell whether it monitors to the cable.
Notice OtherObs(const core::Snapshot& snapshot) {
  std::vector<core::OtherObs> others = snapshot.other_obs;
  const auto title = [&others](std::string_view cable) {
    return std::format("OBS in {} may send audio to {}", core::DescribeOtherObs(others), cable);
  };
  const bool yours = std::any_of(others.begin(), others.end(), [](const core::OtherObs& other) { return other.yours; });
  const auto text = [yours](std::string_view cable) {
    return std::format("Windows keeps it running while you use this {}. If it monitors to {}, apps here hear it with "
                       "your mic. Close it there or turn monitoring off.",
                       yours ? "session" : "account", cable);
  };
  const std::string cable = ShortCableName(snapshot);
  Notice notice{.kind = Kind::kOtherObs, .title = title(cable), .text = text("that cable")};
  if (!cable.empty() && Length(notice.title) <= kMaxTitle) return notice;
  // No cable yet, or a title too long to show whole: the text names the
  // cable, if Windows shows all of it.
  notice.title = title("your cable");
  if (!cable.empty()) {
    std::string named = text(cable);
    if (Length(named) <= kShownText) notice.text = std::move(named);
  }
  if (Length(notice.title) > kMaxTitle) {
    // An account's name too long for even that: as much of it as fits.
    const size_t over = Length(notice.title) - kMaxTitle;
    for (core::OtherObs& other : others) {
      if (Length(other.account) > over + 1) other.account = Shortened(other.account, Length(other.account) - over);
    }
    notice.title = title("your cable");
  }
  return notice;
}

std::optional<Notice> Restarted(core::RestartNeed why, const core::Snapshot& snapshot) {
  switch (why) {
    case core::RestartNeed::kObsUpdated:
      return Notice{.kind = Kind::kRestarted,
                    .title = std::format("OBS was updated to {}", snapshot.obs.version.ToString()),
                    .text = std::format("{} restarted to use it.", kDisplayName)};
    case core::RestartNeed::kAudioChanged:
      return Notice{.kind = Kind::kRestarted,
                    .title = "OBS's sample rate or channels changed",
                    .text = std::format("{} restarted to follow them, as OBS does.", kDisplayName)};
    case core::RestartNeed::kNone:
      break;
  }
  return std::nullopt;
}

}  // namespace

std::string_view RestartNeedName(core::RestartNeed need) {
  switch (need) {
    case core::RestartNeed::kObsUpdated:
      return "obs-updated";
    case core::RestartNeed::kAudioChanged:
      return "audio-changed";
    case core::RestartNeed::kNone:
      break;
  }
  return "";
}

std::optional<core::RestartNeed> RestartNeedNamed(std::string_view name) {
  for (const core::RestartNeed need : {core::RestartNeed::kObsUpdated, core::RestartNeed::kAudioChanged}) {
    if (RestartNeedName(need) == name) return need;
  }
  return std::nullopt;
}

Notifier::Notifier(Options options) : options_(std::move(options)) {}

Notifier::Update Notifier::Changed(const core::Snapshot& snapshot, bool quiet, Clock::time_point now) {
  Update update;
  const core::Snapshot before = std::exchange(snapshot_, snapshot);
  if (snapshot.state == State::kStarting) return update;
  const bool first = !started_;
  started_ = true;

  // A missing mic or cable, once it has been for the grace.
  Follow(mic_, snapshot.state == State::kMicMissing, MicConnected(snapshot), snapshot.state == State::kRunning, now);
  Follow(cable_, snapshot.state == State::kCableMissing, false, false, now);
  if (!mic_) Hide(Kind::kMicMissing, update);
  if (!cable_) Hide(Kind::kCableMissing, update);
  Due(mic_, Kind::kMicMissing, quiet, now, update);
  Due(cable_, Kind::kCableMissing, quiet, now, update);

  // OBS opened in another Windows session, which can send audio to the same
  // cable: knobs can't keep it out. Said last, below, and taken down when it
  // closes.
  const bool other_opened =
      std::any_of(snapshot.other_obs.begin(), snapshot.other_obs.end(), [this](const core::OtherObs& other) {
        return std::find(other_obs_.begin(), other_obs_.end(), other) == other_obs_.end();
      });
  if (snapshot.other_obs.empty()) Hide(Kind::kOtherObs, update);
  other_obs_ = snapshot.other_obs;

  // Other problems, as they begin.
  std::optional<std::pair<State, SetupNeed>> problem;
  if (OtherProblem(snapshot.state)) problem = std::pair{snapshot.state, snapshot.setup};
  if (problem != problem_) {
    for (const Kind kind : {Kind::kObsUnsupported, Kind::kObsMissing, Kind::kNeedsSetup, Kind::kFailed}) {
      Hide(kind, update);
    }
    problem_ = problem;
    if (problem) {
      if (auto notice = ProblemNotice(snapshot, obs_version_)) Show(std::move(*notice), quiet, update);
    }
  }

  // Changes in OBS, after the first import. A problem says more.
  if (snapshot.chain) {
    std::vector<std::string> warnings;
    std::vector<std::string> added;
    for (const import::ImportNote& note : snapshot.notes) {
      if (!note.warning) continue;
      const std::string& key = WarningKey(note);
      if (std::find(warnings_.begin(), warnings_.end(), key) == warnings_.end()) added.push_back(note.text);
      warnings.push_back(key);
    }
    // A mic picked in knobs, or a new cable, isn't news.
    const bool changed =
        chain_revision_ > 0 && snapshot.chain_revision != chain_revision_ && snapshot.settings == before.settings;
    if (!first && !update.show) {
      if (!added.empty()) {
        Show(FilterWarnings(snapshot, added), quiet, update);
      } else if (changed) {
        Show(ChainChanged(snapshot, mic_name_), quiet, update);
      }
    }
    chain_revision_ = snapshot.chain_revision;
    warnings_ = std::move(warnings);
    mic_name_ = snapshot.chain->mic;
  }

  if (first && !update.show && !Stopped(snapshot.state)) {
    if (auto notice = Restarted(options_.restarted_for, snapshot)) Show(std::move(*notice), quiet, update);
  }
  // OBS in another session, unless something above was said: that says more
  // about the mic, and the menu says this too.
  if (other_opened && !update.show) Show(OtherObs(snapshot), quiet, update);
  if (!snapshot.obs.root.empty()) obs_version_ = snapshot.obs.version;
  return update;
}

Notifier::Update Notifier::Tick(bool quiet, Clock::time_point now) {
  Update update;
  if (mic_ && mic_->running_since && now - *mic_->running_since >= options_.mic_back_after) {
    mic_.reset();
    Hide(Kind::kMicMissing, update);
  }
  Due(mic_, Kind::kMicMissing, quiet, now, update);
  Due(cable_, Kind::kCableMissing, quiet, now, update);
  return update;
}

std::optional<Notifier::Clock::time_point> Notifier::NextDeadline() const {
  std::optional<Clock::time_point> next;
  const auto consider = [&next](Clock::time_point time) {
    if (!next || time < *next) next = time;
  };
  if (mic_ && !mic_->due && snapshot_.state == State::kMicMissing) consider(mic_->since + options_.device_grace);
  if (mic_ && mic_->running_since) consider(*mic_->running_since + options_.mic_back_after);
  if (cable_ && !cable_->due && snapshot_.state == State::kCableMissing) {
    consider(cable_->since + options_.device_grace);
  }
  return next;
}

Badge Notifier::badge() const {
  switch (snapshot_.state) {
    case State::kStarting:
      return Badge::kNone;
    case State::kRunning:
      // Between stalls: still missing until it's back.
      return mic_ && mic_->due ? Badge::kAttention : Badge::kNone;
    case State::kPausedByUser:
    case State::kPausedForObs:
      return Badge::kPaused;
    case State::kMicMissing:
      return mic_ && mic_->due ? Badge::kAttention : Badge::kNone;
    case State::kCableMissing:
      return cable_ && cable_->due ? Badge::kAttention : Badge::kNone;
    default:
      return Badge::kAttention;
  }
}

void Notifier::Follow(std::optional<Missing>& missing, bool now_missing, bool stalled, bool running,
                      Clock::time_point now) const {
  if (now_missing) {
    if (!missing) missing = Missing{now};
    missing->stalled = stalled;
    missing->running_since.reset();
  } else if (missing && missing->stalled && running) {
    // A rebuilt chain that may stall again (Options::mic_back_after).
    if (!missing->running_since) missing->running_since = now;
    if (now - *missing->running_since >= options_.mic_back_after) missing.reset();
  } else {
    missing.reset();
  }
}

void Notifier::Due(std::optional<Missing>& missing, Notice::Kind kind, bool quiet, Clock::time_point now,
                   Update& update) {
  if (!missing || missing->due || now - missing->since < options_.device_grace) return;
  const State state = kind == Kind::kMicMissing ? State::kMicMissing : State::kCableMissing;
  if (snapshot_.state != state) return;
  missing->due = true;
  Show(kind == Kind::kMicMissing ? MicMissing(snapshot_) : CableMissing(snapshot_), quiet, update);
}

void Notifier::Show(Notice notice, bool quiet, Update& update) {
  if (quiet) return;
  shown_ = notice;
  update.show = std::move(notice);
  update.hide = false;
}

void Notifier::Hide(Notice::Kind kind, Update& update) {
  if (!shown_ || shown_->kind != kind) return;
  shown_.reset();
  if (!update.show) update.hide = true;
}

}  // namespace knobs::tray
