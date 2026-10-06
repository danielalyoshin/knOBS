// SPDX-License-Identifier: GPL-2.0-or-later
//
// knobs-live: runs knobs's live audio path (a source through an OBS filter
// chain, and libobs's monitor into a virtual cable) and measures it.
//
// Only --run and --measure-mic open the mic, and they say so first. What the
// tool records from devices is reduced to a 1 ms energy envelope and a peak
// level as it arrives; no audio is kept or written anywhere.

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <format>
#include <functional>
#include <numbers>
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
  --measure-restart <s>    Mic-free. Pushes a steady tone and clicks through the chain
                           into --output for s seconds, restarting libobs's monitor
                           every --restart-every seconds as {0} does in silence, and
                           measures each restart's gap and the latency between them
                           on --listen.
  --measure-drift <s>      Mic-free. Times --output's sample clock against
                           QueryPerformanceCounter for s seconds, playing silence into
                           it, and reports how fast or slow it runs (ppm). Run it on a
                           cable and on an interface's playback side at the same time.
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
  --restart-every <s>      With --measure-restart: seconds between restarts (default 5).
  --push-ppm <x>           With --measure-restart: push x ppm faster (+) or slower (-)
                           than real time, as a mic whose clock runs fast or slow
                           against the cable's.
  --force                  Go ahead even if other apps use the cable.
  --obs-dir <folder>       Use this OBS install instead of searching for one.
  --verbose                Echo the libobs log, including debug lines.

Names match case-insensitively on any part; --list-devices shows them.
)",
                     kDisplayName, kMinMeasureSeconds, kImportUsage);
}

enum class Mode {
  kNone,
  kListDevices,
  kMeasureOutput,
  kMeasureCable,
  kMeasureRestart,
  kMeasureDrift,
  kRun,
  kMeasureMic
};

struct Options {
  Mode mode = Mode::kNone;
  int seconds = 0;
  // Empty: the default, which depends on --import.
  std::string output;
  std::string listen = "CABLE Output";
  std::string mic;
  double gain_db = 0;
  int restart_every = 5;
  double push_ppm = 0;
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
    } else if (arg == L"--measure-restart") {
      ok = set_mode(Mode::kMeasureRestart, value);
    } else if (arg == L"--restart-every") {
      wchar_t* end = nullptr;
      const long seconds = std::wcstol(value, &end, 10);
      ok = end != value && *end == L'\0' && seconds >= 1 && seconds <= 600;
      options.restart_every = static_cast<int>(seconds);
    } else if (arg == L"--push-ppm") {
      wchar_t* end = nullptr;
      options.push_ppm = std::wcstod(value, &end);
      ok = end != value && *end == L'\0' && std::isfinite(options.push_ppm) && std::fabs(options.push_ppm) <= 50'000;
    } else if (arg == L"--measure-drift") {
      ok = set_mode(Mode::kMeasureDrift, value);
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
  const bool restarts = options.mode == Mode::kMeasureRestart;
  if (!restarts && (options.restart_every != 5 || options.push_ppm != 0)) return std::nullopt;
  if (restarts && options.restart_every * 2 > options.seconds) return std::nullopt;
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

// --- Monitor restarts -------------------------------------------------------------

// A steady 1 kHz tone, quiet next to the clicks: a gap in what reaches the
// cable shows as a run of silence.
class Tone {
 public:
  static constexpr float kLevel = 0.0316f;  // -30 dBFS peak

  explicit Tone(uint32_t sample_rate) : step_(2 * std::numbers::pi * 1000 / sample_rate) {}

  void AddTo(float* interleaved, size_t frames, uint32_t channels) {
    for (size_t i = 0; i < frames; ++i) {
      const float sample = kLevel * static_cast<float>(std::sin(phase_));
      phase_ += step_;
      if (phase_ >= 2 * std::numbers::pi) phase_ -= 2 * std::numbers::pi;
      for (uint32_t c = 0; c < channels; ++c) interleaved[i * channels + c] += sample;
    }
  }

 private:
  double step_;
  double phase_ = 0;
};

struct Gap {
  uint64_t start_ns = 0;
  double ms = 0;  // Whole envelope bins of silence, so up to 2 ms short.
};

// Runs of bins in [first_bin, end_bin) recorded with less than a tenth of
// the tone's level: nothing reached the cable there. Bins the recording
// skipped extend a run but don't make one.
std::vector<Gap> FindGaps(const Envelope& recorded, size_t first_bin, size_t end_bin) {
  constexpr double kSilent = Tone::kLevel / std::numbers::sqrt2 / 10;
  const std::vector<double> amplitude = recorded.Amplitude();
  end_bin = std::min(end_bin, amplitude.size());
  const auto silent = [&](size_t i) { return recorded.Filled(i) && amplitude[i] < kSilent; };
  std::vector<Gap> gaps;
  for (size_t i = first_bin; i < end_bin;) {
    if (!silent(i)) {
      ++i;
      continue;
    }
    const size_t begin = i;
    while (i < end_bin && (silent(i) || !recorded.Filled(i))) ++i;
    gaps.push_back({recorded.origin_ns() + begin * recorded.bin_ns(),
                    static_cast<double>((i - begin) * recorded.bin_ns()) / 1e6});
  }
  return gaps;
}

// Mic-free: the push source stands in for the mic, with a tone under the
// clicks, and libobs's monitor plays it into the cable while another thread
// restarts the monitor on a schedule, as the core does in silence
// (core::Controller::Timing::monitor_restart_every). Measures what each
// restart leaves on the cable: the gap in the tone, and the latency before
// and after it from the clicks. --push-ppm runs the push off real time, as a
// mic whose clock is fast or slow against the cable's.
void MeasureRestart(runtime::ObsHost& host, const Options& options, const audio::AudioDevice& listen,
                    const Chain& source) {
  const runtime::ObsApi& api = host.api();
  const uint32_t rate = host.session().options().samples_per_sec;
  RegisterPushSource(api);
  auto chain = audio::LiveChain::Start(api, host.session(), source.json,
                                       {.type_id = kPushSourceId, .load_callbacks = source.load_callbacks});
  if (!chain) return Check(false, "chain", chain.error());
  ReportChain(audio::DescribeChain(api, (*chain)->source()));
  Report(Outcome::kNote, "measure",
         std::format("pushing a tone and clicks through the chain for {} s{}, restarting the monitor every {} s",
                     options.seconds,
                     options.push_ppm != 0 ? std::format(" at {:+g} ppm off real time", options.push_ppm) : "",
                     options.restart_every));

  const uint64_t origin = NowNs();
  const size_t bins = Bins((options.seconds + 2) * 1'000'000'000ull);
  Envelope reference(origin, kBinNs, bins);
  auto recorder = EndpointRecorder::Start(listen.id, Envelope(origin, kBinNs, bins));
  if (!recorder) return Check(false, "listen", recorder.error());

  // A fast mic delivers its 10 ms packets more often than every 10 ms.
  const double period_ns = 10e6 / (1 + options.push_ppm / 1e6);
  const uint64_t start = NowNs() + 100'000'000;
  const auto slot = [&](size_t n) { return start + static_cast<uint64_t>(static_cast<double>(n) * period_ns); };
  const uint64_t every_ns = uint64_t{static_cast<unsigned>(options.restart_every)} * 1'000'000'000;
  const size_t restart_count = static_cast<size_t>(options.seconds / options.restart_every) - 1;

  // The restarts, from a thread of their own as the core's: when each began
  // and how long libobs took.
  struct Restart {
    uint64_t at_ns = 0;
    double took_ms = 0;
  };
  std::vector<Restart> restarts(restart_count);
  std::thread restarter([&] {
    // libobs's monitor opens its device with COM on the calling thread.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (size_t k = 0; k < restart_count && !g_interrupted; ++k) {
      WaitUntil(timer, start + (k + 1) * every_ns);
      restarts[k].at_ns = NowNs();
      (*chain)->RestartMonitor();
      restarts[k].took_ms = static_cast<double>(NowNs() - restarts[k].at_ns) / 1e6;
    }
    if (timer) CloseHandle(timer);
    if (SUCCEEDED(com)) CoUninitialize();
  });

  ClickTrain clicks(rate, 2);
  Tone tone(rate);
  const uint32_t frames = rate / 100;
  std::vector<float> packet(size_t{frames} * 2);
  std::vector<float> left(frames), right(frames);
  const float* planes[] = {left.data(), right.data()};
  const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  const size_t packets = static_cast<size_t>(options.seconds * 1e9 / period_ns);
  const int priority = GetThreadPriority(GetCurrentThread());
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  uint64_t end = start;
  for (size_t n = 0; n < packets && !g_interrupted; ++n) {
    // As MeasureClicks: a packet comes once its last frame is in, stamped
    // with its first.
    WaitUntil(timer, slot(n + 1));
    clicks.Next(packet.data(), frames);
    tone.AddTo(packet.data(), frames, 2);
    for (uint32_t i = 0; i < frames; ++i) {
      left[i] = packet[2 * i];
      right[i] = packet[2 * i + 1];
    }
    reference.Add(slot(n), rate, 2, packet.data(), frames);
    PushAudio(api, (*chain)->source(), planes, 2, frames, rate, slot(n));
    end = slot(n + 1);
  }
  SetThreadPriority(GetCurrentThread(), priority);
  if (timer) CloseHandle(timer);
  restarter.join();

  SleepFor(std::chrono::nanoseconds(kMaxDelayNs));  // Let the last clicks arrive.
  const auto stopped = (*recorder)->Stop();
  if (!stopped) return Check(false, "listen", stopped.error());
  if (g_interrupted) return Report(Outcome::kNote, "restarts", "interrupted");
  const Envelope& recorded = (*recorder)->envelope();

  // The latency from the clicks, in each half of the stretch before the
  // first restart and after each one: a backlog the monitor builds shows as
  // a rise within a stretch.
  std::vector<uint64_t> bounds = {start};
  for (const Restart& restart : restarts) bounds.push_back(restart.at_ns);
  bounds.push_back(end);
  std::string each;
  double worst = 1;
  for (size_t k = 0; k + 1 < bounds.size(); ++k) {
    const size_t first = Bins(bounds[k] - origin) + 200;
    const size_t last = Bins(bounds[k + 1] - origin);
    const size_t middle = (first + last) / 2;
    const DelayEstimate early = EstimateDelay(reference, recorded, kMaxDelayNs, first, middle);
    const DelayEstimate late = EstimateDelay(reference, recorded, kMaxDelayNs, middle, last);
    each += std::format("{}{:.1f}/{:.1f}", each.empty() ? "" : ", ", early.delay_ms, late.delay_ms);
    worst = std::min({worst, early.correlation, late.correlation});
  }
  Check(worst >= kMinCorrelation, "latency",
        std::format("{} ms, in each half of the stretch from the start and after each restart (correlation >= "
                    "{:.2f})",
                    each, worst));

  // A restart's gap reaches the cable within half a second of it. Any other
  // silence is the monitor's stream running dry, or a hiccup.
  const std::vector<Gap> gaps = FindGaps(recorded, Bins(start - origin) + 500, Bins(end - origin));
  std::vector<double> restart_gaps(restarts.size(), 0);
  std::vector<Gap> other;
  for (const Gap& gap : gaps) {
    bool matched = false;
    for (size_t k = 0; k < restarts.size() && !matched; ++k) {
      if (gap.start_ns >= restarts[k].at_ns && gap.start_ns < restarts[k].at_ns + 500'000'000) {
        restart_gaps[k] += gap.ms;
        matched = true;
      }
    }
    if (!matched) other.push_back(gap);
  }
  std::string took, lost;
  for (size_t k = 0; k < restarts.size(); ++k) {
    took += std::format("{}{:.1f}", took.empty() ? "" : ", ", restarts[k].took_ms);
    lost += std::format("{}{:.0f}", lost.empty() ? "" : ", ", restart_gaps[k]);
  }
  Report(Outcome::kNote, "restart took", std::format("{} ms", took));
  std::vector<double> sorted = restart_gaps;
  std::sort(sorted.begin(), sorted.end());
  Check(!sorted.empty(), "restart gaps",
        sorted.empty() ? std::string("no restarts")
                       : std::format("{} ms (median {:.0f}, {:.0f} to {:.0f})", lost, sorted[sorted.size() / 2],
                                     sorted.front(), sorted.back()));
  std::string others;
  for (const Gap& gap : other) {
    others += std::format("{}{:.0f} ms at {:.2f} s", others.empty() ? "" : ", ", gap.ms,
                          static_cast<double>(gap.start_ns - start) / 1e9);
  }
  Report(other.empty() ? Outcome::kOk : Outcome::kWarn, "other gaps", other.empty() ? std::string("none") : others);
}

// --- Clock drift -----------------------------------------------------------------

struct ClockFit {
  double ppm = 0;          // How much faster than its nominal rate the clock runs.
  double residual_us = 0;  // RMS distance of the readings from the line.
};

// A least-squares line through the device's time against QPC time, over
// readings [begin, end).
ClockFit FitClock(const std::vector<EndpointClock::Reading>& readings, size_t begin, size_t end) {
  if (end - begin < 3) return {};
  // Relative to the first reading and centered, so doubles keep their
  // precision.
  const EndpointClock::Reading& first = readings[begin];
  const auto x = [&](size_t i) { return static_cast<double>(readings[i].qpc_ns - first.qpc_ns) / 1e9; };
  const auto y = [&](size_t i) { return readings[i].device_s - first.device_s; };
  const double n = static_cast<double>(end - begin);
  double mean_x = 0, mean_y = 0;
  for (size_t i = begin; i < end; ++i) {
    mean_x += x(i);
    mean_y += y(i);
  }
  mean_x /= n;
  mean_y /= n;
  double sxx = 0, sxy = 0;
  for (size_t i = begin; i < end; ++i) {
    sxx += (x(i) - mean_x) * (x(i) - mean_x);
    sxy += (x(i) - mean_x) * (y(i) - mean_y);
  }
  if (sxx <= 0) return {};
  const double slope = sxy / sxx;
  double residual = 0;
  for (size_t i = begin; i < end; ++i) {
    const double e = y(i) - mean_y - slope * (x(i) - mean_x);
    residual += e * e;
  }
  return {(slope - 1) * 1e6, std::sqrt(residual / n) * 1e6};
}

// Mic-free: times the output's sample clock against QueryPerformanceCounter,
// the clock libobs and WASAPI timestamps use, playing silence into it.
// Positive drift means the device consumes more samples per second than its
// nominal rate.
void MeasureDrift(const Options& options, const audio::AudioDevice& output) {
  auto clock = EndpointClock::Open(output.id);
  if (!clock) return Check(false, "clock", clock.error());
  Report(Outcome::kNote, "measure",
         std::format("timing \"{}\" ({} Hz) against QueryPerformanceCounter for {} s, playing silence into it",
                     output.name, (*clock)->sample_rate(), options.seconds));
  std::vector<EndpointClock::Reading> readings;
  const auto start = std::chrono::steady_clock::now();
  const auto end = start + std::chrono::seconds(options.seconds);
  const auto report_every = std::chrono::seconds(std::max(10, options.seconds / 10));
  auto next_report = start + report_every;
  for (auto now = start; now < end && !g_interrupted; now = std::chrono::steady_clock::now()) {
    const auto reading = (*clock)->Poll();
    if (!reading) return Check(false, "clock", reading.error());
    // The position moves once per engine period, so polls repeat readings.
    if (readings.empty() || reading->qpc_ns != readings.back().qpc_ns) readings.push_back(*reading);
    if (now >= next_report) {
      const ClockFit fit = FitClock(readings, 0, readings.size());
      Report(Outcome::kNote, std::format("{:>4} s", std::chrono::duration_cast<std::chrono::seconds>(now - start).count()),
             std::format("{:+.2f} ppm so far", fit.ppm));
      next_report += report_every;
    }
    std::this_thread::sleep_for(10ms);
  }
  if (g_interrupted) return Report(Outcome::kNote, "drift", "interrupted");
  const ClockFit fit = FitClock(readings, 0, readings.size());
  std::string quarters;
  for (int q = 0; q < kWindows; ++q) {
    const ClockFit part = FitClock(readings, readings.size() * q / kWindows, readings.size() * (q + 1) / kWindows);
    quarters += std::format("{}{:+.2f}", quarters.empty() ? "" : ", ", part.ppm);
  }
  const double hz = (*clock)->sample_rate() * (1 + fit.ppm / 1e6);
  Check(readings.size() >= 3, "drift",
        std::format("{:+.2f} ppm against QPC, {:+.1f} ms per hour ({:.3f} Hz); per quarter: {} ppm; {} readings "
                    "within {:.1f} us of the line",
                    fit.ppm, fit.ppm * 3.6, hz, quarters, readings.size(), fit.residual_us));
  if ((*clock)->low_polls() > 0) {
    Report(Outcome::kWarn, "clock",
           std::format("{} polls found less than 10 ms queued; the stream may have run dry, which skews the "
                       "result",
                       (*clock)->low_polls()));
  }
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
    const bool clicks = options.mode == Mode::kMeasureOutput || options.mode == Mode::kMeasureCable ||
                        options.mode == Mode::kMeasureRestart;
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
  if (output && options.mode == Mode::kMeasureDrift) {
    // Silence: whoever else uses the device hears nothing of it.
    Report(Outcome::kOk, "devices", std::format("timing \"{}\"", output->name));
    return MeasureDrift(options, *output);
  }
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
    case Mode::kMeasureRestart:
      return MeasureRestart(host, options, *listen, chain);
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
