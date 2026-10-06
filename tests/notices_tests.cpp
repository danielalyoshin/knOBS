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
#include <vector>

#include "app_info.h"
#include "core/controller.h"
#include "core/state.h"
#include "test_harness.h"
#include "tray/badge.h"
#include "tray/fake_backend.h"
#include "tray/notices.h"

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
