// SPDX-License-Identifier: GPL-2.0-or-later
//
// knobs-live: runs knobs's live audio path (a source through an OBS filter
// chain, and libobs's monitor into a virtual cable) and measures it.
//
// Only --run and --measure-mic open the mic, and they say so first. What the
// tool records from devices is reduced to a 1 ms energy envelope and a peak
// level as it arrives; no audio is kept or written anywhere.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "app_info.h"
#include "audio/audio_devices.h"
#include "audio/live_chain.h"
#include "common/console.h"
#include "common/envelope.h"
#include "common/obs_import.h"
#include "common/push_source.h"
#include "common/random.h"
#include "live/endpoints.h"
#include "runtime/obs_host.h"
#include "util/text_file.h"
#include "util/win_strings.h"

namespace {

using namespace knobs;
using namespace knobs::tools;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr uint64_t kBinNs = 1'000'000;            // Envelope resolution.
constexpr uint64_t kMaxDelayNs = 1'000'000'000;   // Latencies searched: 0 to 1 s.
constexpr int kWindows = 4;                       // Separate estimates per measurement.
constexpr double kMinCorrelation = 0.5;
constexpr int kMaxSeconds = 3600;
// Each quarter of a measurement needs some sound. Speech has pauses, and the
// test clicks start 0.3 s in and come 0.15 to 0.45 s apart.
constexpr int kMinMeasureSeconds = 8;

std::string Usage() {
  return std::format(R"(Usage: knobs-live <mode> [options]

Runs {0}'s live audio path with no OBS process: a source through an OBS
filter chain, monitored by libobs into a virtual cable. The chain is the mic
imported from OBS (--import), or the mic with one gain filter.

Modes (pick one):
  --list-devices           List recording and playback devices. Opens none.
  --measure-output <s>     Mic-free. Pushes a click pattern through the chain into
                           --output for s seconds (at least {1}) and measures how long
                           it takes to arrive on --listen.
  --measure-cable <s>      Mic-free baseline: the same clicks written straight into
                           --output by a stream set up like libobs's monitor, without
                           libobs. The difference from --measure-output is libobs's.
  --run <s>                Opens the mic. Runs it through the chain into --output for
                           s seconds (Ctrl+C stops), reporting the level that arrives
                           on --listen each second.
  --measure-mic <s>        Opens the mic. Measures mic-to-cable latency over s seconds
                           (at least {1}; 20 is better) by comparing the mic with
                           --listen. Keep talking or tapping the mic while it runs.

Options:
  --import                 Use the mic from OBS's active profile and scene collection,
                           with its filters and source-level state, at the profile's
                           sample rate and channels.
{2}
  --output <name|id>       Playback device to monitor to. Default: "CABLE Input", or
                           with --import, the OBS profile's monitoring device. A profile
                           left at "Default" monitors to the default playback device,
                           usually speakers, so that needs --output.
  --listen <name|id>       Recording side of that cable. Default: "CABLE Output".
  --mic <name|id>          The mic for the mic + gain chain, and the one --measure-mic
                           compares with. Default: "default", the default
                           communications device. With --import, the chain opens the
                           imported mic's device, so only --external takes --mic.
  --gain-db <x>            The gain filter's setting (default 0).
  --source <file.json>     Use this OBS source object (one entry of a scene
                           collection's "sources") instead of the mic + gain chain.
                           Keep --mic pointing at its device for --measure-mic.
  --external               With --measure-mic: start nothing, and measure whatever
                           already plays into --output, such as OBS monitoring the
                           same mic.
  --force                  Go ahead even if other apps use the cable.
  --obs-dir <folder>       Use this OBS install instead of searching for one.
  --verbose                Echo the libobs log, including debug lines.

Names match case-insensitively on any part; --list-devices shows them.
)",
                     kDisplayName, kMinMeasureSeconds, kImportUsage);
}

enum class Mode { kNone, kListDevices, kMeasureOutput, kMeasureCable, kRun, kMeasureMic };

struct Options {
  Mode mode = Mode::kNone;
  int seconds = 0;
  // Empty: the default, which depends on --import.
  std::string output;
  std::string listen = "CABLE Output";
  std::string mic;
  double gain_db = 0;
  bool import = false;
  ImportArgs import_args;
  std::optional<fs::path> source;
  bool external = false;
  bool force = false;
  std::optional<fs::path> obs_dir;
  bool verbose = false;
};

std::optional<Options> ParseArgs(int argc, wchar_t** argv) {
  Options options;
  auto set_mode = [&](Mode mode, const wchar_t* value) {
    if (options.mode != Mode::kNone) return false;
    options.mode = mode;
    if (!value) return true;
    wchar_t* end = nullptr;
    const long seconds = std::wcstol(value, &end, 10);
    const long min = mode == Mode::kRun ? 1 : kMinMeasureSeconds;
    if (end == value || *end != L'\0' || seconds < min || seconds > kMaxSeconds) return false;
    options.seconds = static_cast<int>(seconds);
    return true;
  };
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view arg = argv[i];
    const wchar_t* value = i + 1 < argc ? argv[i + 1] : nullptr;
    bool ok = true;
    bool takes_value = true;
    if (arg == L"--list-devices") {
      ok = set_mode(Mode::kListDevices, nullptr);
      takes_value = false;
    } else if (arg == L"--external") {
      options.external = true;
      takes_value = false;
    } else if (arg == L"--import") {
      options.import = true;
      takes_value = false;
    } else if (bool bad = false; ParseImportArg(arg, value, options.import_args, bad)) {
      ok = !bad;
    } else if (arg == L"--force") {
      options.force = true;
      takes_value = false;
    } else if (arg == L"--verbose") {
      options.verbose = true;
      takes_value = false;
    } else if (!value) {
      ok = false;
    } else if (arg == L"--measure-output") {
      ok = set_mode(Mode::kMeasureOutput, value);
    } else if (arg == L"--measure-cable") {
      ok = set_mode(Mode::kMeasureCable, value);
    } else if (arg == L"--run") {
      ok = set_mode(Mode::kRun, value);
    } else if (arg == L"--measure-mic") {
      ok = set_mode(Mode::kMeasureMic, value);
    } else if (arg == L"--output") {
      options.output = ToUtf8(std::wstring_view(value));
    } else if (arg == L"--listen") {
      options.listen = ToUtf8(std::wstring_view(value));
    } else if (arg == L"--mic") {
      options.mic = ToUtf8(std::wstring_view(value));
    } else if (arg == L"--gain-db") {
      wchar_t* end = nullptr;
      options.gain_db = std::wcstod(value, &end);
      ok = end != value && *end == L'\0' && std::isfinite(options.gain_db) && std::fabs(options.gain_db) <= 60;
    } else if (arg == L"--source") {
      options.source = fs::absolute(value);
    } else if (arg == L"--obs-dir") {
      options.obs_dir = fs::absolute(value);
    } else {
      ok = false;
    }
    if (!ok) return std::nullopt;
    if (takes_value) ++i;
  }
  if (options.mode == Mode::kNone) return std::nullopt;
  if (options.external && options.mode != Mode::kMeasureMic) return std::nullopt;
  // One chain, and the import options only with --import.
  if ((options.import && options.source) || (options.import_args.given() && !options.import)) return std::nullopt;
  // The imported chain opens its own mic, so --mic could only name another.
  if (options.import && !options.mic.empty() && !options.external) return std::nullopt;
  return options;
}

// --- Helpers ------------------------------------------------------------------

// The source to run, as an OBS source object.
struct Chain {
  std::string json;
  bool load_callbacks = false;  // See audio::LoadOptions.
};

std::atomic<bool> g_interrupted = false;

BOOL WINAPI OnConsoleCtrl(DWORD type) {
  if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
  g_interrupted = true;
  return TRUE;
}

// Sleeps for `duration` unless Ctrl+C comes first. Returns false if it did.
bool SleepFor(std::chrono::steady_clock::duration duration) {
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (!g_interrupted) {
    const auto left = deadline - std::chrono::steady_clock::now();
    if (left <= 0s) return true;
    std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(left, 50ms));
  }
  return false;
}

std::string Join(const std::vector<std::string>& names) {
  std::string text;
  for (const std::string& name : names) text += (text.empty() ? "" : ", ") + name;
  return text;
}

size_t Bins(uint64_t duration_ns) { return static_cast<size_t>(duration_ns / kBinNs); }

// Estimates the delay separately in kWindows equal parts of the reference
// bins [first_bin, end_bin), and reports the median.
void ReportLatency(std::string_view what, const Envelope& reference, const Envelope& delayed, size_t first_bin,
                   size_t end_bin) {
  std::vector<DelayEstimate> estimates;
  for (int w = 0; w < kWindows; ++w) {
    estimates.push_back(EstimateDelay(reference, delayed, kMaxDelayNs, first_bin + (end_bin - first_bin) * w / kWindows,
                                      first_bin + (end_bin - first_bin) * (w + 1) / kWindows));
  }
  std::string each;
  double worst = 1;
  std::vector<double> delays;
  for (const DelayEstimate& e : estimates) {
    each += std::format("{}{:.1f}", each.empty() ? "" : ", ", e.delay_ms);
    worst = std::min(worst, e.correlation);
    delays.push_back(e.delay_ms);
  }
  std::sort(delays.begin(), delays.end());
  const double median = (delays[(kWindows - 1) / 2] + delays[kWindows / 2]) / 2;
  const bool ok = worst >= kMinCorrelation;
  Check(ok, "latency",
        ok ? std::format("{:.1f} ms {} (per quarter: {} ms; correlation >= {:.2f})", median, what, each, worst)
           : std::format("no clear match (correlation {:.2f}; per quarter: {} ms). Was there enough sound?", worst,
                         each));
}

// --- Click measurements --------------------------------------------------------

// Short noise bursts at irregular intervals: easy to spot in an energy
// envelope, and never periodic, so the envelopes line up at one delay only.
class ClickTrain {
 public:
  ClickTrain(uint32_t sample_rate, uint32_t channels)
      : sample_rate_(sample_rate), channels_(channels), click_frames_(sample_rate * 4 / 1000),
        click_frame_(click_frames_), gap_(sample_rate * 3 / 10) {}

  // The next `frames` frames, interleaved.
  void Next(float* interleaved, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
      float sample = 0;
      if (click_frame_ < click_frames_) {
        const float window =
            0.5f - 0.5f * std::cos(6.2831853f * static_cast<float>(click_frame_) / static_cast<float>(click_frames_));
        sample = kClickLevel * window * random_.Uniform();
        if (++click_frame_ == click_frames_) {
          gap_ = static_cast<size_t>((0.15f + 0.3f * (random_.Uniform() * 0.5f + 0.5f)) *
                                     static_cast<float>(sample_rate_));
        }
      } else if (gap_ == 0 || --gap_ == 0) {
        click_frame_ = 0;
      }
      std::fill_n(interleaved + i * channels_, channels_, sample);
    }
  }

 private:
  static constexpr float kClickLevel = 0.25f;  // -12 dBFS

  uint32_t sample_rate_;
  uint32_t channels_;
  size_t click_frames_;  // 4 ms
  size_t click_frame_;   // Where in the current click; click_frames_ between clicks.
  size_t gap_;           // Frames until the next click.
  XorShift32 random_{0x636C6B21};
};

// Waits until NowNs() reaches `deadline_ns`, with a high-resolution timer.
void WaitUntil(HANDLE timer, uint64_t deadline_ns) {
  const uint64_t now = NowNs();
  if (deadline_ns <= now) return;
  LARGE_INTEGER due;
  due.QuadPart = -static_cast<LONGLONG>((deadline_ns - now) / 100);  // Relative, 100 ns units.
  if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
    WaitForSingleObject(timer, INFINITE);
  } else {
    std::this_thread::sleep_for(std::chrono::nanoseconds(deadline_ns - now));
  }
}

// Receives one 10 ms packet of clicks, interleaved, and the time of its first
// frame.
using PacketSink = std::function<void(const float* interleaved, uint32_t frames, uint64_t first_ns)>;

// Delivers the click pattern to `sink` in 10 ms packets on a real-time
// schedule, as a capture device would, for options.seconds, while recording
// `listen`. Then reports how much later the clicks showed up there.
void MeasureClicks(const Options& options, const audio::AudioDevice& listen, uint32_t sample_rate,
                   uint32_t channels, std::string_view what, const PacketSink& sink) {
  const uint64_t origin = NowNs();
  const size_t bins = Bins((options.seconds + 2) * 1'000'000'000ull);
  Envelope reference(origin, kBinNs, bins);
  auto recorder = EndpointRecorder::Start(listen.id, Envelope(origin, kBinNs, bins));
  if (!recorder) return Check(false, "listen", recorder.error());

  ClickTrain clicks(sample_rate, channels);
  const uint32_t packet_frames = sample_rate / 100;
  std::vector<float> packet(size_t{packet_frames} * channels);
  const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  const uint64_t packet_ns = 10'000'000;
  const size_t packets = size_t{static_cast<unsigned>(options.seconds)} * 100;
  const int priority = GetThreadPriority(GetCurrentThread());
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  const uint64_t start = NowNs();
  for (size_t n = 0; n < packets && !g_interrupted; ++n) {
    // A device delivers a packet once its last frame is in.
    WaitUntil(timer, start + (n + 1) * packet_ns);
    clicks.Next(packet.data(), packet_frames);
    // The time of the packet's first frame, one packet before its slot ends.
    // That's how win-wasapi stamps packets without device timing, when on
    // time. Packets sent late to catch up keep their own slots.
    const uint64_t first_ns = start + n * packet_ns;
    reference.Add(first_ns, sample_rate, channels, packet.data(), packet_frames);
    sink(packet.data(), packet_frames, first_ns);
  }
  SetThreadPriority(GetCurrentThread(), priority);
  if (timer) CloseHandle(timer);

  SleepFor(std::chrono::nanoseconds(kMaxDelayNs));  // Let the last clicks arrive.
  const auto stopped = (*recorder)->Stop();
  if (!stopped) return Check(false, "listen", stopped.error());
  if (g_interrupted) return Report(Outcome::kNote, "latency", "interrupted");
  ReportLatency(what, reference, (*recorder)->envelope(), Bins(start - origin),
                Bins(start + packets * packet_ns - origin));
}

// Mic-free: the push source stands in for the mic and the chain's monitor
// plays the clicks into the cable. Measures from each packet's timestamp to
// when the cable's recording side has it: the chain, the monitor, the
// playback buffer and the cable, i.e. everything after the capture device.
void MeasureOutput(runtime::ObsHost& host, const Options& options, const audio::AudioDevice& listen,
                   const Chain& source) {
  const runtime::ObsApi& api = host.api();
  const uint32_t rate = host.session().options().samples_per_sec;
  RegisterPushSource(api);
  auto chain = audio::LiveChain::Start(api, host.session(), source.json,
                                       {.type_id = kPushSourceId, .load_callbacks = source.load_callbacks});
  if (!chain) return Check(false, "chain", chain.error());
  ReportChain(audio::DescribeChain(api, (*chain)->source()));
  Report(Outcome::kNote, "measure",
         std::format("pushing clicks through the chain for {} s (a push source stands in for the mic)",
                     options.seconds));
  std::vector<float> left(rate / 100), right(rate / 100);
  const float* planes[] = {left.data(), right.data()};
  MeasureClicks(options, listen, rate, 2, std::format("from push to \"{}\"", listen.name),
                [&](const float* interleaved, uint32_t frames, uint64_t first_ns) {
                  for (uint32_t i = 0; i < frames; ++i) {
                    left[i] = interleaved[2 * i];
                    right[i] = interleaved[2 * i + 1];
                  }
                  PushAudio(api, (*chain)->source(), planes, 2, frames, rate, first_ns);
                });
}

// Mic-free baseline for MeasureOutput: the same clicks written straight into
// the cable by a stream set up like libobs's monitor, with no libobs in the
// path. What MeasureOutput measures on top of this is what libobs adds.
void MeasureCable(const Options& options, const audio::AudioDevice& output, const audio::AudioDevice& listen) {
  auto player = EndpointPlayer::Open(output.id);
  if (!player) return Check(false, "play", player.error());
  Report(Outcome::kNote, "measure",
         std::format("writing clicks straight into \"{}\" ({} Hz, {} ch) for {} s, bypassing libobs", output.name,
                     (*player)->sample_rate(), (*player)->channels(), options.seconds));
  std::optional<std::string> failure;
  MeasureClicks(options, listen, (*player)->sample_rate(), (*player)->channels(),
                std::format("from write to \"{}\"", listen.name),
                [&](const float* interleaved, uint32_t frames, uint64_t) {
                  const auto written = (*player)->Write(interleaved, frames);
                  if (!written && !failure) failure = written.error();
                });
  if (failure) Check(false, "play", *failure);
}

void OnChainAudio(void* param, obs_source_t*, const audio_data* audio, bool) {
  auto* meter = static_cast<PeakHold*>(param);
  for (size_t c = 0; c < MAX_AV_PLANES && audio->data[c]; ++c) {
    meter->Add(reinterpret_cast<const float*>(audio->data[c]), audio->frames);
  }
}

// Opens the mic: runs it through the chain into the cable, and shows what
// the chain outputs next to what arrives on the cable's recording side.
void RunMic(runtime::ObsHost& host, const Options& options, const audio::AudioDevice& mic,
            const audio::AudioDevice& listen, const Chain& source) {
  const runtime::ObsApi& api = host.api();
  auto recorder = EndpointRecorder::Start(listen.id, Envelope(NowNs(), kBinNs, 0));
  if (!recorder) return Check(false, "listen", recorder.error());
  Report(Outcome::kNote, "mic",
         std::format("opening \"{}\" for {} s (Ctrl+C stops); other apps can record it from \"{}\"", mic.name,
                     options.seconds, listen.name));
  auto chain = audio::LiveChain::Start(api, host.session(), source.json, {.load_callbacks = source.load_callbacks});
  if (!chain) return Check(false, "chain", chain.error());
  ReportChain(audio::DescribeChain(api, (*chain)->source()));
  PeakHold meter;
  api.obs_source_add_audio_capture_callback((*chain)->source(), OnChainAudio, &meter);

  bool arrived = false;
  for (int second = 1; second <= options.seconds && SleepFor(1s); ++second) {
    const float chain_peak = meter.Take();
    const float cable_peak = (*recorder)->TakePeak();
    arrived = arrived || cable_peak > 0;
    Report(Outcome::kNote, std::format("{:>4} s", second),
           std::format("chain out {:<14} cable {}", FormatPeak(chain_peak), FormatPeak(cable_peak)));
  }

  api.obs_source_remove_audio_capture_callback((*chain)->source(), OnChainAudio, &meter);
  (*chain).reset();
  const auto stopped = (*recorder)->Stop();
  if (!stopped) return Check(false, "listen", stopped.error());
  if (!arrived && g_interrupted) return Report(Outcome::kNote, "cable", "interrupted");
  // With --force, another app's audio on the cable counts too.
  Check(arrived, "cable",
        arrived ? std::format("audio arrived on \"{}\"{}", listen.name, options.force ? " (--force: maybe not ours)" : "")
                : std::format("nothing arrived on \"{}\"", listen.name));
}

// Opens the mic: records it and the cable's recording side side by side and
// measures how much later the cable has the same sound. Works the same
// whether knobs or OBS feeds the cable.
void MeasureMic(runtime::ObsHost& host, const Options& options, const audio::AudioDevice& mic,
                const audio::AudioDevice& listen, const Chain& source) {
  const runtime::ObsApi& api = host.api();
  const uint64_t origin = NowNs();
  const uint64_t duration_ns = uint64_t{static_cast<unsigned>(options.seconds)} * 1'000'000'000;
  const size_t bins = Bins(duration_ns + 1'000'000'000);
  Report(Outcome::kNote, "mic",
         std::format("opening \"{}\" for {} s: talk, clap or tap the mic now", mic.name, options.seconds));
  auto mic_recorder = EndpointRecorder::Start(mic.id, Envelope(origin, kBinNs, bins));
  if (!mic_recorder) return Check(false, "mic", mic_recorder.error());
  auto cable_recorder = EndpointRecorder::Start(listen.id, Envelope(origin, kBinNs, bins));
  if (!cable_recorder) return Check(false, "listen", cable_recorder.error());
  std::unique_ptr<audio::LiveChain> chain;
  if (!options.external) {
    auto started = audio::LiveChain::Start(api, host.session(), source.json, {.load_callbacks = source.load_callbacks});
    if (!started) return Check(false, "chain", started.error());
    chain = std::move(*started);
    ReportChain(audio::DescribeChain(api, chain->source()));
  }

  for (int second = 1; second <= options.seconds && SleepFor(1s); ++second) {
    Report(Outcome::kNote, std::format("{:>4} s", second),
           std::format("mic {:<14} cable {}", FormatPeak((*mic_recorder)->TakePeak()),
                       FormatPeak((*cable_recorder)->TakePeak())));
  }
  chain.reset();
  const auto mic_stopped = (*mic_recorder)->Stop();
  const auto cable_stopped = (*cable_recorder)->Stop();
  if (!mic_stopped || !cable_stopped) {
    return Check(false, "record", !mic_stopped ? mic_stopped.error() : cable_stopped.error());
  }
  if (g_interrupted) return Report(Outcome::kNote, "latency", "interrupted");
  // The cable recording runs kMaxDelayNs past the last mic window.
  ReportLatency(std::format("from \"{}\" to \"{}\"{}", mic.name, listen.name, options.external ? " (external)" : ""),
                (*mic_recorder)->envelope(), (*cable_recorder)->envelope(), 0,
                Bins(duration_ns - kMaxDelayNs));
}

// Checks who else uses the cable. Returns false if the mode shouldn't go on.
bool CheckCable(const Options& options, const audio::AudioDevice& output, const audio::AudioDevice& listen) {
  auto players = OtherActiveSessions(output.id, EndpointFlow::kPlayback);
  auto listeners = OtherActiveSessions(listen.id, EndpointFlow::kRecording);
  if (!players || !listeners) {
    Check(false, "cable check", !players ? players.error() : listeners.error());
    return false;
  }
  if (options.external) {
    if (players->empty()) {
      Check(false, "cable check",
            std::format("nothing is playing into \"{}\". Start OBS with the mic monitored to it first.", output.name));
      return false;
    }
    Report(Outcome::kNote, "cable check", std::format("playing into \"{}\": {}", output.name, Join(*players)));
  } else if (!players->empty()) {
    const std::string text = std::format("{} already playing into \"{}\"", Join(*players), output.name);
    if (!options.force) {
      Check(false, "cable check", text + ". Two sources on one cable double the audio and spoil measurements. "
                                         "Close it, or pass --force.");
      return false;
    }
    Report(Outcome::kNote, "cable check", text + " (--force)");
  }
  if (!listeners->empty()) {
    const std::string text = std::format("{} listening on \"{}\"", Join(*listeners), listen.name);
    const bool clicks = options.mode == Mode::kMeasureOutput || options.mode == Mode::kMeasureCable;
    if (clicks && !options.force) {
      Check(false, "cable check", text + ", and would hear the test clicks. Close it, or pass --force.");
      return false;
    }
    Report(Outcome::kNote, "cable check", text);
  }
  return true;
}

void RunMode(runtime::ObsHost& host, const Options& options, const std::optional<import::ActiveObsConfig>& config,
             const std::vector<audio::AudioDevice>& mics, const std::vector<audio::AudioDevice>& outputs) {
  const runtime::ObsApi& api = host.api();
  Chain chain;
  std::string output_query = options.output.empty() ? "CABLE Input" : options.output;
  std::string mic_query = options.mic.empty() ? "default" : options.mic;
  if (config) {
    auto imported = ImportMic(api, *config, options.import_args);
    if (!imported) return Check(false, "import", imported.error());
    chain = {std::move(imported->source_json), imported->load_callbacks()};
    if (options.output.empty()) {
      // OBS's default, which follows the default playback device: speakers,
      // most likely, where the mic would feed back and clicks wouldn't
      // reach --listen.
      if (config->audio.monitoring_device_id == "default") {
        return Check(false, "devices",
                     "The OBS profile monitors to \"Default\", the default playback device, which is usually "
                     "speakers. Pass --output with the virtual cable, or --output default if that's where it "
                     "should go.");
      }
      output_query = config->audio.monitoring_device_id;
    }
    if (options.mic.empty()) mic_query = imported->mic.device_id;
    if (!audio::FindDevice(mics, imported->mic.device_id, "recording device")) {
      Report(Outcome::kWarn, "mic", std::format("the mic's device ({}) isn't connected", imported->mic.device_id));
    }
  }
  auto output = audio::FindDevice(outputs, output_query, "playback device");
  auto listen = audio::FindDevice(mics, options.listen, "recording device");
  if (!output || !listen) {
    return Check(false, "devices",
                 (!output ? output.error() : listen.error()) + " Is VB-Cable installed? --list-devices shows what's there.");
  }
  std::optional<audio::AudioDevice> mic;
  if (options.mode == Mode::kRun || options.mode == Mode::kMeasureMic) {
    auto found = audio::FindDevice(mics, mic_query, "recording device");
    if (!found) return Check(false, "devices", found.error());
    mic = *found;
  }
  Report(Outcome::kOk, "devices",
         std::format("{}monitoring to \"{}\", listening on \"{}\"", mic ? std::format("mic \"{}\", ", mic->name) : "",
                     output->name, listen->name));
  if (!CheckCable(options, *output, *listen)) return;

  if (options.source) {
    auto text = ReadText(*options.source);
    if (!text) return Check(false, "chain", text.error());
    chain.json = std::move(*text);
  } else if (!config) {
    chain.json = audio::MicWithGainSourceJson(mic ? mic->id : "default", options.gain_db);
  }
  if (!options.external) {
    const auto set = audio::SetMonitoringDevice(api, *output);
    if (!set) return Check(false, "monitoring", set.error());
  }

  switch (options.mode) {
    case Mode::kMeasureOutput:
      return MeasureOutput(host, options, *listen, chain);
    case Mode::kMeasureCable:
      return MeasureCable(options, *output, *listen);
    case Mode::kRun:
      return RunMic(host, options, *mic, *listen, chain);
    case Mode::kMeasureMic:
      return MeasureMic(host, options, *mic, *listen, chain);
    default:
      return;
  }
}

void PrintDevices(std::string_view heading, const std::vector<audio::AudioDevice>& devices) {
  Print(std::format("{}\n", heading));
  for (const audio::AudioDevice& device : devices) Print(std::format("  {:<46} {}\n", device.name, device.id));
}

int Run(const Options& options) {
  Print(std::format("{} live audio\n", kDisplayName));
  auto started = StartTool({.obs_dir = options.obs_dir,
                            .log_prefix = L"live ",
                            .verbose = options.verbose,
                            .import = options.import,
                            .import_args = options.import_args});
  if (const int* code = std::get_if<int>(&started)) return *code;
  auto& [host, config] = std::get<StartedTool>(started);

  const auto mics = audio::ListMicDevices(host->api());
  const auto outputs = audio::ListMonitoringDevices(host->api());
  if (options.mode == Mode::kListDevices) {
    PrintDevices("Recording devices (--mic, --listen):", mics);
    PrintDevices("Playback devices (--output):", outputs);
  } else {
    RunMode(*host, options, config, mics, outputs);
  }
  return FinishRun(*host);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // Plain LoadLibrary calls skip PATH and the working directory; see
  // ObsRuntime::Load.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == std::wstring_view(L"--help") || argv[i] == std::wstring_view(L"-h")) {
      Print(Usage());
      return kExitPass;
    }
  }
  const auto options = ParseArgs(argc, argv);
  if (!options) {
    Print(Usage());
    return kExitUsage;
  }
  return Run(*options);
}
