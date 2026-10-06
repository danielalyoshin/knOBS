// SPDX-License-Identifier: GPL-2.0-or-later
//
// Unit tests for the tray's notifications and badges: the Notifier on the
// fake core, which is the real core::Controller on knobs-tray's made-up OBS
// and devices (tools/tray/fake_backend.h), with a fake clock. No OBS, no
// libobs and no audio devices.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app_info.h"
#include "core/controller.h"
#include "core/state.h"
#include "test_harness.h"
#include "tray/badge.h"
#include "tray/fake_backend.h"
#include "tray/notices.h"
#include "util/win_strings.h"

namespace {

using namespace knobs;
using namespace knobs::tray;
using namespace std::chrono_literals;
using core::State;
using tools::FakeWorld;
using Clock = Notifier::Clock;

// The core on a fake world and a fake clock, and a Notifier hearing it.
// `events` lists what the notification area was asked to do: each
// notification's title as it's shown, and "-" as one is taken down.
struct Fixture {
  std::shared_ptr<tools::SharedWorld> world;
  tools::FakeBackend backend;
  std::vector<core::Snapshot> pending;
  core::Controller controller;
  Notifier notifier;
  Clock::time_point now{};
  bool quiet = false;
  std::vector<std::string> events;
  // The last notification shown, or one with no title if none was.
  Notice last;

  explicit Fixture(FakeWorld start = tools::DefaultWorld(), core::Settings settings = {},
                   Notifier::Options options = {})
      : world(std::make_shared<tools::SharedWorld>(std::move(start))),
        backend(world, [](std::string_view) {}),
        controller(backend, std::move(settings), [this](const core::Snapshot& s) { pending.push_back(s); },
                   {.clock = [this] { return now; }}),
        notifier(options) {}

  State state() const { return controller.snapshot().state; }

  void Apply(const Notifier::Update& update) {
    if (update.show) {
      events.push_back(update.show->title);
      last = *update.show;
    } else if (update.hide) {
      events.push_back("-");
    }
  }
  // Passes what the core published to the notifier.
  void Feed() {
    for (const core::Snapshot& snapshot : pending) Apply(notifier.Changed(snapshot, quiet, now));
    pending.clear();
  }
  // Passes only the last, as the tray does when the core publishes faster
  // than it takes them (TrayApp::OnSnapshot).
  void FeedLast() {
    if (!pending.empty()) Apply(notifier.Changed(pending.back(), quiet, now));
    pending.clear();
  }

  void Start(bool obs_running = false) {
    controller.Start(obs_running, now);
    Feed();
  }
  // Moves the clock on by `duration`, ticking the core and the notifier at
  // whatever falls due on the way.
  void Advance(Clock::duration duration) {
    const Clock::time_point end = now + duration;
    for (int ticks = 0; ticks < 10'000; ++ticks) {
      std::optional<Clock::time_point> next = controller.NextDeadline();
      if (const auto notice = notifier.NextDeadline(); notice && (!next || *notice < *next)) next = notice;
      if (!next || *next > end) break;
      now = std::max(now, *next);
      controller.Tick(now);
      Feed();
      Apply(notifier.Tick(quiet, now));
    }
    now = end;
  }
  // Changes the world's devices, and lets the core's notifications settle.
  void Devices(const std::function<void(FakeWorld&)>& change) {
    world->Change(change);
    controller.DevicesChanged(now);
    Feed();
    Advance(500ms);
  }
  void ObsOpens() {
    controller.SetObsRunning(true, now);
    Feed();
  }
  // OBS closes, after `change` was made in it.
  void ObsCloses(const std::function<void(FakeWorld&)>& change = {}) {
    if (change) world->Change(change);
    controller.SetObsRunning(false, now);
    Feed();
  }
  void Reimport() {
    controller.Reimport(now);
    Feed();
  }
};

const std::vector<std::string> kNone;

// --- Quiet --------------------------------------------------------------------------

TEST(NoticesStayQuietOnAColdBoot) {
  Fixture f;
  f.Start();
  CHECK(f.state() == State::kRunning && f.events == kNone && f.notifier.badge() == Badge::kNone);

  // Started while OBS is open: paused, and nothing to say.
  Fixture open;
  open.Start(true);
  CHECK(open.state() == State::kPausedForObs && open.events == kNone && open.notifier.badge() == Badge::kPaused);
  open.ObsCloses();
  CHECK(open.state() == State::kRunning && open.events == kNone && open.notifier.badge() == Badge::kNone);

  // Warnings found at start were the first run's to show.
  FakeWorld warned = tools::DefaultWorld();
  tools::ToggleVstFilter(warned);
  Fixture warnings(warned);
  warnings.Start();
  CHECK(warnings.events == kNone);
}

TEST(NoticesStayQuietWhenObsOpensAndClosesUnchanged) {
  Fixture f;
  f.Start();
  for (int i = 0; i < 3; ++i) {
    f.ObsOpens();
    CHECK(f.state() == State::kPausedForObs && f.notifier.badge() == Badge::kPaused);
    f.Advance(10s);
    f.ObsCloses();
    CHECK(f.state() == State::kRunning && f.notifier.badge() == Badge::kNone);
    f.Advance(10s);
  }
  f.Reimport();
  CHECK(f.events == kNone);
}

TEST(NoticesWaitForTheFirstRun) {
  Fixture f;
  f.quiet = true;
  f.Start();
  f.Devices(tools::ToggleCables);
  f.Advance(10s);
  CHECK(f.state() == State::kCableMissing && f.events == kNone);
  // The icon still says so.
  CHECK(f.notifier.badge() == Badge::kAttention);
  // Not said later either.
  f.quiet = false;
  f.Advance(1min);
  CHECK(f.events == kNone);
  f.Devices(tools::ToggleCables);
  CHECK(f.state() == State::kRunning && f.events == kNone && f.notifier.badge() == Badge::kNone);
}

// --- Missing devices ----------------------------------------------------------------

TEST(NoticesSayAMicIsMissingAfterFiveSeconds) {
  Fixture f;
  f.Start();
  f.Devices(tools::ToggleMics);
  CHECK(f.state() == State::kMicMissing && f.events == kNone && f.notifier.badge() == Badge::kNone);
  f.Advance(4900ms);
  CHECK(f.events == kNone);
  f.Advance(200ms);
  CHECK(f.events == std::vector<std::string>{"Mic missing"} && f.notifier.badge() == Badge::kAttention);
  CHECK(f.last.text ==
        std::format("The recording device for Mic/Aux isn't connected. {} starts again when it's back.", kDisplayName));
  CHECK(f.last.problem && f.last.sound && !f.last.page && f.notifier.shown() == f.last);
  f.Advance(1min);
  CHECK(f.events.size() == 1);

  // Back: taken down.
  f.Devices(tools::ToggleMics);
  CHECK(f.state() == State::kRunning && f.notifier.badge() == Badge::kNone);
  CHECK((f.events == std::vector<std::string>{"Mic missing", "-"}) && !f.notifier.shown());
}

TEST(NoticesStayQuietThroughAPowerCycle) {
  Fixture f;
  f.Start();
  f.Devices(tools::ToggleMics);
  f.Advance(2s);
  f.Devices(tools::ToggleMics);
  CHECK(f.state() == State::kRunning);
  f.Advance(1min);
  CHECK(f.events == kNone && f.notifier.badge() == Badge::kNone);

  // An interface with the mic and the device knobs sends to: the cable goes
  // too.
  f.Devices([](FakeWorld& world) {
    tools::ToggleMics(world);
    tools::ToggleCables(world);
  });
  CHECK(f.state() == State::kCableMissing);
  f.Advance(3s);
  f.Devices([](FakeWorld& world) {
    tools::ToggleMics(world);
    tools::ToggleCables(world);
  });
  f.Advance(1min);
  CHECK(f.state() == State::kRunning && f.events == kNone);
}

TEST(NoticesSayAStalledMicOnce) {
  Fixture f;
  f.Start();
  f.world->Change([](FakeWorld& world) { world.stalled = true; });
  // The core sees the stall in 3 s, rebuilds the chain after 2 s, sees it
  // stall again, and waits 5 s: the mic has been missing for 5 s by then.
  f.Advance(9s);
  CHECK(f.state() == State::kMicMissing && f.events == std::vector<std::string>{"Mic missing"});
  CHECK(f.last.text == std::format("Mic/Aux stopped sending audio. {} keeps trying to start it again.", kDisplayName));
  // Rebuilt and stalled again, more than once: still missing, said once.
  f.Advance(2min);
  CHECK(f.events.size() == 1);
  CHECK(f.notifier.badge() == Badge::kAttention);

  f.world->Change([](FakeWorld& world) { world.stalled = false; });
  f.Advance(2min);
  CHECK(f.state() == State::kRunning && f.notifier.badge() == Badge::kNone);
  CHECK((f.events == std::vector<std::string>{"Mic missing", "-"}));
}

TEST(NoticesSayACableIsMissing) {
  Fixture f;
  f.Start();
  f.Devices(tools::ToggleCables);
  CHECK(f.state() == State::kCableMissing && f.events == kNone);
  f.Advance(5s);
  CHECK(f.events == std::vector<std::string>{"Cable missing"} && f.notifier.badge() == Badge::kAttention);
  CHECK(f.last.text == std::format("CABLE In 16ch isn't connected. {} starts again when it's back, or click to "
                                    "choose another cable.",
                                    kDisplayName));
  CHECK(f.last.page == FirstRunPage::kCable && f.last.problem);

  // Nothing's missing while paused.
  f.ObsOpens();
  CHECK((f.events == std::vector<std::string>{"Cable missing", "-"}) && f.notifier.badge() == Badge::kPaused);
  f.Advance(1min);
  f.Devices(tools::ToggleCables);
  f.ObsCloses();
  CHECK(f.state() == State::kRunning && f.events.size() == 2);
}

// --- Changes in OBS -----------------------------------------------------------------

TEST(NoticesSayTheChainChangedInObs) {
  Fixture f;
  f.Start();
  f.ObsOpens();
  f.ObsCloses(tools::EditChain);
  CHECK(f.state() == State::kRunning && f.events == std::vector<std::string>{"Mic/Aux changed in OBS"});
  CHECK(f.last.text == std::format("{} now runs: Mic/Aux › Noise Suppression › 3-Band EQ › Expander › Compressor "
                                    "› Limiter › CABLE In 16ch",
                                    kDisplayName));
  CHECK(!f.last.problem && !f.last.sound && !f.last.page);

  // Unchanged: nothing more. Changed back: said again.
  f.ObsOpens();
  f.ObsCloses();
  f.Reimport();
  CHECK(f.events.size() == 1);
  f.ObsOpens();
  f.ObsCloses(tools::EditChain);
  CHECK(f.events.size() == 2 && f.last.text.find("Mic/Aux › 3-Band EQ") != std::string::npos);

  // A new mic in OBS.
  f.ObsOpens();
  f.ObsCloses([](FakeWorld& world) {
    world.collection.front().mic.name = "Podcast Mic";
    world.collection.front().chain_key += "|renamed";
  });
  CHECK(f.last.title == "OBS's mic is now Podcast Mic");
}

TEST(NoticesStayQuietForAChangeMadeInKnobs) {
  // Several mics: picking another in knobs changes the chain.
  FakeWorld world = tools::DefaultWorld();
  world.collection.push_back(world.collection.front());
  world.collection.back().mic.name = "Podcast Mic";
  world.collection.back().chain_key = "podcast";
  Fixture f(world, {.mic = "Mic/Aux"});
  f.Start();
  core::Settings settings = f.controller.snapshot().settings;
  settings.mic = "Podcast Mic";
  f.controller.Apply(settings, f.now);
  f.Feed();
  CHECK(f.state() == State::kRunning && f.controller.snapshot().chain->mic == "Podcast Mic");
  CHECK(f.events == kNone);
}

TEST(NoticesSayAFilterKnobsCantRun) {
  Fixture f;
  f.Start();
  f.ObsOpens();
  f.ObsCloses(tools::ToggleVstFilter);
  CHECK(f.state() == State::kRunning);
  CHECK(f.events == std::vector<std::string>{std::format("Mic/Aux has a filter {} can't run", kDisplayName)});
  CHECK(f.last.text == f.controller.snapshot().notes.front().text && f.last.text.starts_with("Filter \"ReaComp\""));
  CHECK(f.last.page == FirstRunPage::kWarnings && f.last.sound && !f.last.problem);
  CHECK(f.notifier.badge() == Badge::kNone);

  // Said once.
  f.ObsOpens();
  f.ObsCloses();
  CHECK(f.events.size() == 1);
  // The filter taken out along with a change to the chain: the change. Put
  // back with another: the warning, which says more.
  f.ObsOpens();
  f.ObsCloses([](FakeWorld& world) {
    tools::ToggleVstFilter(world);
    tools::EditChain(world);
  });
  CHECK(f.events.size() == 2 && f.last.title == "Mic/Aux changed in OBS");
  f.ObsOpens();
  f.ObsCloses([](FakeWorld& world) {
    tools::ToggleVstFilter(world);
    tools::EditChain(world);
  });
  CHECK(f.events.size() == 3 && f.last.kind == Notice::Kind::kFilterWarnings);

  // The filter renamed in OBS: its warning's text changes, but it isn't new.
  f.ObsOpens();
  f.ObsCloses([](FakeWorld& world) {
    import::ImportNote& warning = world.collection.front().notes.front();
    warning.text.replace(warning.text.find("ReaComp"), 7, "Comp");
  });
  CHECK(f.controller.snapshot().notes.front().text.starts_with("Filter \"Comp\""));
  CHECK(f.events.size() == 3);
}

// The notification for `warnings`, new on Mic/Aux when OBS closes.
Notice WarningsNotice(const std::vector<std::string>& warnings) {
  Fixture f;
  f.Start();
  f.ObsOpens();
  f.ObsCloses([&warnings](FakeWorld& world) {
    for (const std::string& warning : warnings) world.collection.front().notes.push_back({true, warning});
  });
  return f.last;
}

TEST(NoticesFitWarningsInTheBalloon) {
  // A balloon's text holds 255 UTF-16 units (NOTIFYICONDATAW::szInfo).
  const auto fits = [](const Notice& notice) { return FromUtf8(notice.text).size() <= 255; };
  // The text is `start`, then as much of `rest` as fits, cut after a word
  // and ended with "…".
  const auto cut_at_word = [](const Notice& notice, const std::string& start, const std::string& rest) {
    if (!notice.text.starts_with(start) || !notice.text.ends_with("…")) return false;
    const std::string shown = notice.text.substr(start.size(), notice.text.size() - start.size() - 3);
    return !shown.empty() && rest.starts_with(shown) && std::string_view(" ,.;:").find(rest[shown.size()]) !=
                                                             std::string_view::npos;
  };
  const auto ducking = [](std::string_view sidechain) {
    return std::format("Compressor \"Duck\" turns the mic down under \"{0}\" in OBS. {1} loads only the mic, so it "
                       "keeps only this compressor's output gain (+3.0 dB), and the mic sounds as it does in OBS "
                       "while nothing plays on \"{0}\".",
                       sidechain, kDisplayName);
  };
  const auto unknown = [](std::string_view name, std::string_view type) {
    return std::format("Filter \"{}\" ({}) isn't one {} has. It passes audio through untouched.", name, type,
                       kDisplayName);
  };
  const std::string vst = std::format("Filter \"ReaComp\" is a VST plugin (reacomp-standalone.dll). {} can't run "
                                      "VST plugins yet, so it leaves the filter out.",
                                      kDisplayName);

  // One that Windows shows whole: just the warning.
  Notice notice = WarningsNotice({vst});
  CHECK(notice.title == std::format("Mic/Aux has a filter {} can't run", kDisplayName) && notice.text == vst);
  // Several: the first line says how many and what a click shows, then each
  // warning on a line of its own, those the balloon holds whole.
  const std::string a = unknown("A", "a");
  const std::string b = unknown("B", "b");
  notice = WarningsNotice({a, b});
  CHECK(notice.title == std::format("Mic/Aux has filters {} can't run", kDisplayName));
  CHECK(notice.text == "2 warnings. Click to see them all.\n" + a + "\n" + b);
  const std::string deep = unknown("DeepFilterNet", "deepfilternet_noise_suppression_filter");
  notice = WarningsNotice({vst, deep, ducking("Discord")});
  CHECK(notice.text == "3 warnings. Click to see them all.\n" + vst);
  // One too long to show whole: the first line says a click shows it all.
  const std::string long_ducking = ducking("Discord");
  notice = WarningsNotice({long_ducking});
  CHECK(notice.text == "Click to see it in full.\n" + long_ducking && fits(notice));
  // And too long for the balloon: as much of it as fits.
  const std::string longer_ducking = ducking("Discord Desktop Audio");
  notice = WarningsNotice({longer_ducking});
  CHECK(fits(notice) && cut_at_word(notice, "Click to see it in full.\n", longer_ducking));
  // Measured as the balloon holds them: these fit whole, though their UTF-8
  // doesn't.
  const std::string cyrillic = unknown("Шумоподавление для микрофона", "rnnoise_ru");
  const std::string cyrillic_2 = unknown("Подавление эха и шумов", "echo_ru");
  const std::string both = "2 warnings. Click to see them all.\n" + cyrillic + "\n" + cyrillic_2;
  CHECK(both.size() > 255);
  notice = WarningsNotice({cyrillic, cyrillic_2});
  CHECK(notice.text == both && fits(notice));
}

TEST(NoticesKeepWarningsThatFitWhole) {
  // The balloon's limit falls in the first word of the second warning: the
  // first, which fits, is kept whole.
  const std::string start = "2 warnings. Click to see them all.\n";
  std::string first = std::format("Filter \"A\" isn't one {} has.", kDisplayName);
  while (start.size() + first.size() + 1 < 245) first += " More.";
  const std::string second = "Compressor \"Duck\" turns the mic down under \"Discord\" in OBS.";
  CHECK(start.size() + first.size() + 1 <= 254 && start.size() + first.size() + 1 + second.size() > 255);
  const Notice notice = WarningsNotice({first, second});
  CHECK(notice.text == start + first);
}

TEST(NoticesShortenALongChain) {
  // A chain too long to show whole says it in the short form, as the menu does.
  Fixture f;
  f.Start();
  f.ObsOpens();
  f.ObsCloses([](FakeWorld& world) {
    import::ImportedMic& mic = world.collection.front();
    for (int i = 1; i <= 12; ++i) mic.filters.push_back(std::format("Parametric EQ band {}", i));
    mic.mic.filters = mic.filters;
    mic.chain_key += "|long";
  });
  CHECK(f.last.kind == Notice::Kind::kChainChanged);
  CHECK(f.last.text == std::format("{} now runs: Mic/Aux › 16 filters › CABLE In 16ch", kDisplayName));
}

// OBS in other sessions: Alex's, Sam's, and one whose account's name can't
// be read.
const core::OtherObs kAlex{.session = 2, .account = "Alex"};
const core::OtherObs kSam{.session = 3, .account = "Sam"};
core::OtherObs Unnamed(uint32_t session) { return {.session = session}; }

TEST(NoticesSayObsIsOpenInAnotherAccount) {
  // It can send audio to the same cable, which knobs can't keep out, so it's
  // said as it opens and taken down as it closes. knobs runs on.
  Fixture f;
  f.Start();
  f.controller.SetOtherObs({kAlex}, f.now);
  f.Feed();
  CHECK(f.state() == State::kRunning);
  // What it means for the mic, not just that it's open, all of it within
  // what Windows shows.
  CHECK(f.events == std::vector<std::string>{"OBS in Alex's account may send audio to CABLE In 16ch"});
  CHECK(f.last.text ==
        "Windows keeps it running while you use this account. If it monitors to that cable, apps here hear it with "
        "your mic. Close it there or turn monitoring off.");
  CHECK(FromUtf8(f.last.text).size() <= 160);
  CHECK(!f.last.problem && !f.last.page && f.notifier.badge() == Badge::kNone);
  // Another account too: said again. Then both close.
  f.controller.SetOtherObs({kAlex, kSam}, f.now);
  f.Feed();
  CHECK(f.events.size() == 2 && f.last.title == "OBS in 2 other Windows accounts may send audio to CABLE In 16ch");
  f.controller.SetOtherObs({}, f.now);
  f.Feed();
  CHECK(f.events.size() == 3 && f.events.back() == "-");
  // Already open when knobs starts, seen right after: said too.
  Fixture g;
  g.Start();
  g.controller.SetOtherObs({Unnamed(2)}, g.now);
  g.Feed();
  CHECK(g.events == std::vector<std::string>{"OBS in another Windows account may send audio to CABLE In 16ch"});
}

TEST(NoticesSayObsInEachNewSession) {
  // Accounts whose names can't be read are told apart by their sessions.
  Fixture f;
  f.Start();
  f.controller.SetOtherObs({Unnamed(2)}, f.now);
  f.Feed();
  f.controller.SetOtherObs({Unnamed(2), Unnamed(3)}, f.now);
  f.Feed();
  CHECK(f.events.size() == 2 && f.last.title == "OBS in 2 other Windows accounts may send audio to CABLE In 16ch");
  // An account signed in twice is one account, but the OBS in its second
  // session is news.
  Fixture g;
  g.Start();
  g.controller.SetOtherObs({kAlex}, g.now);
  g.Feed();
  g.controller.SetOtherObs({kAlex, {.session = 4, .account = "Alex"}}, g.now);
  g.Feed();
  CHECK(g.events.size() == 2 && g.last.title == "OBS in Alex's account may send audio to CABLE In 16ch");
}

TEST(NoticesSayObsInYourOtherSession) {
  // This account, signed in a second time.
  Fixture f;
  f.Start();
  f.controller.SetOtherObs({{.session = 2, .account = "Daniel", .yours = true}}, f.now);
  f.Feed();
  CHECK(f.events == std::vector<std::string>{"OBS in your other session may send audio to CABLE In 16ch"});
  CHECK(f.last.text ==
        "Windows keeps it running while you use this session. If it monitors to that cable, apps here hear it with "
        "your mic. Close it there or turn monitoring off.");
}

TEST(NoticesSayWhatComesWithObsInAnotherAccount) {
  // OBS here closes with a filter knobs can't run, and OBS opens in another
  // account, in one poll: the tray gets one snapshot with both. The warning,
  // which plays a sound, says more.
  Fixture f;
  f.Start();
  f.ObsOpens();
  f.world->Change(tools::ToggleVstFilter);
  f.controller.SetObsRunning(false, f.now);
  f.controller.SetOtherObs({kAlex}, f.now);
  f.FeedLast();
  CHECK(f.events == std::vector<std::string>{std::format("Mic/Aux has a filter {} can't run", kDisplayName)});
  // Likewise a change to the chain.
  Fixture g;
  g.Start();
  g.ObsOpens();
  g.world->Change(tools::EditChain);
  g.controller.SetObsRunning(false, g.now);
  g.controller.SetOtherObs({kAlex}, g.now);
  g.FeedLast();
  CHECK(g.events == std::vector<std::string>{"Mic/Aux changed in OBS"});
}

TEST(NoticesSayObsInAnotherAccountBeforeACableIsChosen) {
  FakeWorld world = tools::DefaultWorld();
  world.config.audio.monitoring_device_id = "default";
  world.config.audio.monitoring_device_name.clear();
  Fixture f(world);
  f.Start();
  CHECK(f.state() == State::kNeedsSetup && f.controller.snapshot().chain);
  f.controller.SetOtherObs({kAlex}, f.now);
  f.Feed();
  CHECK(f.last.title == "OBS in Alex's account may send audio to your cable");
  CHECK(f.last.text.find("If it monitors to that cable,") != std::string::npos);
}

TEST(NoticesFitObsInAnotherAccountInTheTitle) {
  // A title holds 63 UTF-16 units (NOTIFYICONDATAW::szInfoTitle), and
  // Windows shows about 160 of a text.
  const auto fits = [](const Notice& notice) {
    return FromUtf8(notice.title).size() <= 63 && FromUtf8(notice.text).size() <= 160;
  };
  const auto sends_to = [](std::string_view cable) {
    return std::format("Windows keeps it running while you use this account. If it monitors to {}, apps here hear "
                       "it with your mic. Close it there or turn monitoring off.",
                       cable);
  };
  // OBS sends the mic to `name`.
  const auto with_cable = [](std::string name) {
    FakeWorld world = tools::DefaultWorld();
    world.devices.outputs.push_back({name, "{0.0.0.00000000}.{long-cable}"});
    world.config.audio.monitoring_device_id = "{0.0.0.00000000}.{long-cable}";
    world.config.audio.monitoring_device_name = std::move(name);
    return world;
  };

  // The cable's name would make the title too long: the text names it.
  Fixture f(with_cable("CABLE-A In 16ch (VB-Audio Cable A)"));
  f.Start();
  f.controller.SetOtherObs({kAlex, kSam}, f.now);
  f.Feed();
  CHECK(fits(f.last) && f.last.title == "OBS in 2 other Windows accounts may send audio to your cable");
  CHECK(f.last.text == sends_to("CABLE-A In 16ch"));
  // Too long for the text too: "that cable", as the menu names it.
  Fixture g(with_cable("Voicemeeter Input (VB-Audio VoiceMeeter VAIO)"));
  g.Start();
  g.controller.SetOtherObs({Unnamed(2)}, g.now);
  g.Feed();
  CHECK(fits(g.last) && g.last.title == "OBS in another Windows account may send audio to your cable");
  CHECK(g.last.text == sends_to("that cable"));
  // An account's name too long for the title: as much of it as fits.
  Fixture h;
  h.Start();
  h.controller.SetOtherObs({{.session = 2, .account = "Maximilian Alexander"}}, h.now);
  h.Feed();
  CHECK(fits(h.last) && h.last.title == "OBS in Maximilian…'s account may send audio to your cable");
  CHECK(h.last.text == sends_to("CABLE In 16ch"));
}

// --- OBS updates --------------------------------------------------------------------

TEST(NoticesSayKnobsRestartedForAnObsUpdate) {
  Fixture f;
  f.Start();
  f.ObsOpens();
  std::string version;
  f.ObsCloses([&version](FakeWorld& world) { version = tools::UpdateObs(world); });
  CHECK(version == "32.2.3" && f.state() == State::kRestartNeeded);
  CHECK(f.controller.snapshot().restart == core::RestartNeed::kObsUpdated);
  // The tray restarts by itself, so nothing's said. The icon says it's
  // stopped, in case it can't.
  CHECK(f.events == kNone && f.notifier.badge() == Badge::kAttention);

  // The new copy says why.
  Fixture restarted(f.world->Get(), {}, {.restarted_for = core::RestartNeed::kObsUpdated});
  restarted.Start();
  CHECK(restarted.state() == State::kRunning);
  CHECK(restarted.events == std::vector<std::string>{"OBS was updated to 32.2.3"});
  CHECK(restarted.last.text == std::format("{} restarted to use it.", kDisplayName) && !restarted.last.page);
  CHECK(!restarted.last.sound && !restarted.last.problem);
  // Once.
  restarted.ObsOpens();
  restarted.ObsCloses();
  CHECK(restarted.events.size() == 1);

  // For new audio settings in OBS.
  Fixture format(tools::DefaultWorld(), {}, {.restarted_for = core::RestartNeed::kAudioChanged});
  format.Start();
  CHECK(format.events == std::vector<std::string>{"OBS's sample rate or channels changed"});
  CHECK(format.last.text == std::format("{} restarted to follow them, as OBS does.", kDisplayName));
}

TEST(NoticesSayKnobsStoppedForAnUnsupportedObs) {
  Fixture f;
  f.Start();
  f.ObsOpens();
  f.ObsCloses([](FakeWorld& world) {
    tools::UpdateObs(world);
    tools::UpdateObs(world);
  });
  CHECK(f.state() == State::kObsUnsupported);
  CHECK(f.events == std::vector<std::string>{"OBS was updated to 33.0.0"});
  CHECK(f.last.text ==
        std::format("{} doesn't support it yet, so it stopped. Click for what you can do.", kDisplayName));
  CHECK(f.last.page == FirstRunPage::kObsUnsupported && f.last.problem && f.notifier.badge() == Badge::kAttention);
  // Said once.
  f.ObsOpens();
  f.ObsCloses();
  CHECK(f.events.size() == 1);

  // Found at start: a problem says so even then.
  Fixture start(f.world->Get());
  start.Start();
  CHECK(start.events == std::vector<std::string>{"This version of OBS isn't supported"});
  CHECK(start.last.text ==
        std::format("{} doesn't support OBS 33.0.0 yet, so it stopped. Click for what you can do.", kDisplayName));

  // Too old, at start and when OBS changes to it: not "yet", since no update
  // will support it.
  FakeWorld old_world = tools::DefaultWorld();
  old_world.obs = {core::ObsFound::kUnsupported, {"C:\\Program Files\\obs-studio", {31, 1, 4}},
                   runtime::UnsupportedObsMessage("31.1.4")};
  Fixture old(old_world);
  old.Start();
  CHECK(old.events == std::vector<std::string>{"This version of OBS isn't supported"});
  CHECK(old.last.text ==
        std::format("OBS 31.1.4 is too old for {}, so it stopped. Click for what you can do.", kDisplayName));
  Fixture changed;
  changed.Start();
  changed.ObsOpens();
  changed.ObsCloses([](FakeWorld& world) {
    world.obs = {core::ObsFound::kUnsupported, {world.obs.install.root, {31, 1, 4}},
                 runtime::UnsupportedObsMessage("31.1.4")};
  });
  CHECK(changed.events == std::vector<std::string>{"OBS changed to 31.1.4"});
  CHECK(changed.last.text ==
        std::format("It's too old for {0}, so {0} stopped. Click for what you can do.", kDisplayName));
}

// --- Other problems -----------------------------------------------------------------

TEST(NoticesSayWhatStopsKnobs) {
  Fixture f;
  f.Start();
  const auto after = [&f](const std::function<void(FakeWorld&)>& change) {
    f.ObsOpens();
    f.ObsCloses(change);
    return f.last;
  };
  Notice notice;

  // A second mic in OBS: knobs doesn't know which to run.
  notice = after([](FakeWorld& world) {
    world.collection.push_back(world.collection.front());
    world.collection.back().mic.name = "Mic/Aux 2";
  });
  CHECK(f.state() == State::kNeedsSetup && notice.title == "OBS has 2 mics" && notice.page == FirstRunPage::kMic);
  CHECK(notice.text == std::format("{} stopped until you choose the one it runs. Click to choose.", kDisplayName));
  // Settled: taken down.
  f.controller.Apply({.mic = "Mic/Aux"}, f.now);
  f.Feed();
  CHECK(f.state() == State::kRunning && f.events.back() == "-");

  // The picked mic gone.
  notice = after([](FakeWorld& world) { world.collection.front().mic.name = "Voice"; });
  CHECK(notice.title == "OBS has 2 mics" &&
        notice.text == std::format("Mic/Aux is no longer in OBS, so {} stopped. Click to choose a mic.", kDisplayName));
  f.controller.Apply({}, f.now);
  f.world->Change([](FakeWorld& world) { world.collection.pop_back(); });
  f.Reimport();
  CHECK(f.state() == State::kRunning);

  notice = after([](FakeWorld& world) { world.collection.clear(); });
  CHECK(notice.title == "No mic in OBS" && notice.page == FirstRunPage::kFilters);
  notice = after([](FakeWorld& world) {
    world.collection = tools::DefaultWorld().collection;
    world.config.audio.monitoring_device_id = "default";
  });
  CHECK(notice.title == "No cable chosen" && notice.page == FirstRunPage::kCable);
  CHECK(notice.text == std::format("OBS now monitors to its default device, usually speakers, so {} stopped. Click "
                                    "to choose a cable.",
                                    kDisplayName));
  notice = after([](FakeWorld& world) {
    world.config = tools::DefaultWorld().config;
    world.config_error = "OBS's settings are gone.";
  });
  CHECK(notice.title == "Can't read OBS's settings" && notice.page == FirstRunPage::kObsSettings);
  notice = after([](FakeWorld& world) {
    world.config_error.reset();
    world.obs = {core::ObsFound::kMissing, {}, "OBS Studio isn't installed."};
  });
  CHECK(notice.title == "Can't find OBS Studio" && notice.page == FirstRunPage::kFindObs);
  CHECK(notice.problem && f.notifier.badge() == Badge::kAttention);

  // Running again: taken down, and nothing said.
  const size_t events = f.events.size();
  after([](FakeWorld& world) { world.obs = tools::DefaultWorld().obs; });
  CHECK(f.state() == State::kRunning && f.events.size() == events + 1 && f.events.back() == "-");

  // A chain that won't start.
  f.world->Change([](FakeWorld& world) { world.chain_error = "libobs couldn't load the mic's source."; });
  f.Reimport();
  CHECK(f.state() == State::kRunning);  // Unchanged, so not reloaded.
  notice = after(tools::EditChain);
  CHECK(f.state() == State::kFailed && notice.title == std::format("{} stopped after an error", kDisplayName));
  CHECK(notice.text == "libobs couldn't load the mic's source. Click for what to do." &&
        notice.page == FirstRunPage::kProblem);
}

TEST(RestartReasonsHaveNames) {
  for (const core::RestartNeed need : {core::RestartNeed::kObsUpdated, core::RestartNeed::kAudioChanged}) {
    CHECK(RestartNeedNamed(RestartNeedName(need)) == need);
  }
  CHECK(RestartNeedName(core::RestartNeed::kObsUpdated) == "obs-updated");
  CHECK(!RestartNeedNamed("") && !RestartNeedNamed("OBS-UPDATED"));
}

// --- Badges -------------------------------------------------------------------------

uint32_t At(const IconPixels& icon, int x, int y) { return icon.bgra[static_cast<size_t>(y) * icon.size + x]; }
uint32_t Alpha(uint32_t pixel) { return pixel >> 24; }

TEST(BadgesSitInTheCorner) {
  for (const int size : {16, 20, 24, 32}) {
    IconPixels icon{size, std::vector<uint32_t>(static_cast<size_t>(size) * size, 0xff808080)};
    CHECK(AddBadge(icon, Badge::kNone).bgra == icon.bgra);
    for (const Badge badge : {Badge::kPaused, Badge::kAttention}) {
      const IconPixels badged = AddBadge(icon, badge);
      // The icon's top left is untouched; the badge's middle is opaque and
      // something else; the ring around the badge is clear.
      CHECK(At(badged, 0, 0) == 0xff808080 && At(badged, size / 3, size / 3) == 0xff808080);
      const float radius = std::round(size * 9.0f / 16) / 2;
      const float center = size - radius;
      const float gap = std::max(1.0f, std::round(size / 16.0f));
      const int middle = static_cast<int>(center);
      CHECK(Alpha(At(badged, middle, middle)) == 255 && At(badged, middle, middle) != 0xff808080);
      CHECK(Alpha(At(badged, static_cast<int>(center - radius - gap / 2), middle)) < 128);
    }
    // Windows 11's caution and neutral colors, beside the mark.
    const auto fill = [&](Badge badge) {
      const IconPixels badged = AddBadge(icon, badge);
      const float radius = std::round(size * 9.0f / 16) / 2;
      const float center = size - radius;
      return At(badged, static_cast<int>(center - 0.6f * radius), static_cast<int>(center + 0.3f * radius)) & 0xffffff;
    };
    CHECK(fill(Badge::kAttention) == 0xfce100 && fill(Badge::kPaused) == 0x9d9d9d);
  }
}

}  // namespace
