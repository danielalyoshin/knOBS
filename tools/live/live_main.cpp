// SPDX-License-Identifier: GPL-2.0-or-later
//
// knobs-live: runs knOBS's live audio path (a source through an OBS filter
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
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "app_info.h"
#include "audio/audio_devices.h"
#include "audio/live_chain.h"
#include "common/console.h"
#include "common/envelope.h"
#include "common/push_source.h"
#include "live/endpoints.h"
#include "runtime/obs_host.h"
#include "util/win_strings.h"

namespace {

using namespace knobs;
using namespace knobs::tools;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr uint32_t kSampleRate = 48000;           // The session's default.
constexpr uint64_t kBinNs = 1'000'000;            // Envelope resolution.
constexpr uint64_t kMaxDelayNs = 1'000'000'000;   // Latencies searched: 0 to 1 s.
constexpr int kWindows = 4;                       // Separate estimates per measurement.
constexpr double kMinCorrelation = 0.5;
constexpr int kMaxSeconds = 3600;
// Each quarter of the window needs some sound, and speech has pauses.
constexpr int kMinMeasureSeconds = 8;

std::string Usage() {
  return std::format(R"(Usage: knobs-live <mode> [options]

Runs {}'s live audio path with no OBS process: a source through an OBS
filter chain, monitored by libobs into a virtual cable. Until import lands,
the chain is the mic with one gain filter.

Modes (pick one):
  --list-devices           List recording and playback devices. Opens none.
  --measure-output <s>     Mic-free. Pushes a click pattern through the chain into
                           --output for s seconds and measures how long it takes to
                           arrive on --listen.
  --measure-cable <s>      Mic-free baseline: the same clicks written straight into
                           --output by a stream set up like libobs's monitor, without
                           libobs. The difference from --measure-output is libobs's.
  --run <s>                Opens the mic. Runs it through the chain into --output for
                           s seconds (Ctrl+C stops), reporting the level that arrives
                           on --listen each second.
  --measure-mic <s>        Opens the mic. Measures mic-to-cable latency over s seconds
                           (at least {}; 20 is better) by comparing the mic with
                           --listen. Keep talking or tapping the mic while it runs.

Options:
  --output <name|id>       Playback device to monitor to. Default: "CABLE Input".
  --listen <name|id>       Recording side of that cable. Default: "CABLE Output".
  --mic <name|id>          Default: "default", the default communications device.
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
                     kDisplayName, kMinMeasureSeconds);
}

enum class Mode { kNone, kListDevices, kMeasureOutput, kMeasureCable, kRun, kMeasureMic };

struct Options {
  Mode mode = Mode::kNone;
  int seconds = 0;
  std::string output = "CABLE Input";
  std::string listen = "CABLE Output";
  std::string mic = "default";
  double gain_db = 0;
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
    const long min = mode == Mode::kMeasureMic ? kMinMeasureSeconds : 1;
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
      ok = *end == L'\0' && std::isfinite(options.gain_db) && std::fabs(options.gain_db) <= 60;
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
  return options;
}

// --- Helpers ------------------------------------------------------------------

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

std::string Db(float peak) {
  return peak > 0 ? std::format("{:.1f} dBFS", 20 * std::log10(peak)) : std::string("silence");
}

std::string Join(const std::vector<std::string>& names) {
  std::string text;
  for (const std::string& name : names) text += (text.empty() ? "" : ", ") + name;
  return text;
}

Result<std::string> ReadText(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return Error{std::format("Couldn't open {}.", ToUtf8(file))};
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
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
        sample = kClickLevel * window * Uniform();
        if (++click_frame_ == click_frames_) {
          gap_ = static_cast<size_t>((0.15f + 0.3f * (Uniform() * 0.5f + 0.5f)) * static_cast<float>(sample_rate_));
        }
      } else if (gap_ == 0 || --gap_ == 0) {
        click_frame_ = 0;
      }
      std::fill_n(interleaved + i * channels_, channels_, sample);
    }
  }

 private:
  static constexpr float kClickLevel = 0.25f;  // -12 dBFS

  float Uniform() {  // xorshift32, [-1, 1)
    state_ ^= state_ << 13;
    state_ ^= state_ >> 17;
    state_ ^= state_ << 5;
    return static_cast<float>(static_cast<int32_t>(state_)) / 2147483648.0f;
  }

  uint32_t sample_rate_;
  uint32_t channels_;
  size_t click_frames_;  // 4 ms
  size_t click_frame_;   // Where in the current click; click_frames_ between clicks.
  size_t gap_;           // Frames until the next click.
  uint32_t state_ = 0x636C6B21;
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
    // As win-wasapi stamps packets without device timing: the time of the
    // first frame, one packet before now.
    const uint64_t first_ns = NowNs() - packet_ns;
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
                   const std::string& json) {
  const runtime::ObsApi& api = host.api();
  RegisterPushSource(api);
  auto chain = audio::LiveChain::Start(api, host.session(), json, kPushSourceId);
  if (!chain) return Check(false, "chain", chain.error());
  ReportChain(audio::DescribeChain(api, (*chain)->source()));
  Report(Outcome::kNote, "measure",
         std::format("pushing clicks through the chain for {} s (a push source stands in for the mic)",
                     options.seconds));
  std::vector<float> left(kSampleRate / 100), right(kSampleRate / 100);
  const float* planes[] = {left.data(), right.data()};
  MeasureClicks(options, listen, kSampleRate, 2, std::format("from push to \"{}\"", listen.name),
                [&](const float* interleaved, uint32_t frames, uint64_t first_ns) {
                  for (uint32_t i = 0; i < frames; ++i) {
                    left[i] = interleaved[2 * i];
                    right[i] = interleaved[2 * i + 1];
                  }
                  PushAudio(api, (*chain)->source(), planes, 2, frames, kSampleRate, first_ns);
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

struct PeakMeter {
  std::mutex mutex;
  float peak = 0;

  float Take() {
    std::lock_guard lock(mutex);
    return std::exchange(peak, 0.0f);
  }
};

void OnChainAudio(void* param, obs_source_t*, const audio_data* audio, bool) {
  float peak = 0;
  for (size_t c = 0; c < MAX_AV_PLANES && audio->data[c]; ++c) {
    const auto* samples = reinterpret_cast<const float*>(audio->data[c]);
    for (uint32_t i = 0; i < audio->frames; ++i) peak = std::max(peak, std::fabs(samples[i]));
  }
  auto* meter = static_cast<PeakMeter*>(param);
  std::lock_guard lock(meter->mutex);
  meter->peak = std::max(meter->peak, peak);
}

// Opens the mic: runs it through the chain into the cable, and shows what
// the chain outputs next to what arrives on the cable's recording side.
void RunMic(runtime::ObsHost& host, const Options& options, const audio::AudioDevice& mic,
            const audio::AudioDevice& listen, const std::string& json) {
  const runtime::ObsApi& api = host.api();
  auto recorder = EndpointRecorder::Start(listen.id, Envelope(NowNs(), kBinNs, 0));
  if (!recorder) return Check(false, "listen", recorder.error());
  Report(Outcome::kNote, "mic",
         std::format("opening \"{}\" for {} s (Ctrl+C stops); other apps can record it from \"{}\"", mic.name,
                     options.seconds, listen.name));
  auto chain = audio::LiveChain::Start(api, host.session(), json);
  if (!chain) return Check(false, "chain", chain.error());
  ReportChain(audio::DescribeChain(api, (*chain)->source()));
  PeakMeter meter;
  api.obs_source_add_audio_capture_callback((*chain)->source(), OnChainAudio, &meter);

  bool arrived = false;
  for (int second = 1; second <= options.seconds && SleepFor(1s); ++second) {
    const float chain_peak = meter.Take();
    const float cable_peak = (*recorder)->TakePeak();
    arrived = arrived || cable_peak > 0;
    Report(Outcome::kNote, std::format("{:>4} s", second),
           std::format("chain out {:<14} cable {}", Db(chain_peak), Db(cable_peak)));
  }

  api.obs_source_remove_audio_capture_callback((*chain)->source(), OnChainAudio, &meter);
  (*chain).reset();
  const auto stopped = (*recorder)->Stop();
  if (!stopped) return Check(false, "listen", stopped.error());
  Check(arrived, "cable", arrived ? std::format("audio arrived on \"{}\"", listen.name)
                                  : std::format("nothing arrived on \"{}\"", listen.name));
}

// Opens the mic: records it and the cable's recording side side by side and
// measures how much later the cable has the same sound. Works the same
// whether knOBS or OBS feeds the cable.
void MeasureMic(runtime::ObsHost& host, const Options& options, const audio::AudioDevice& mic,
                const audio::AudioDevice& listen, const std::string& json) {
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
  std::optional<Result<std::unique_ptr<audio::LiveChain>>> chain;
  if (!options.external) {
    chain = audio::LiveChain::Start(api, host.session(), json);
    if (!*chain) return Check(false, "chain", chain->error());
    ReportChain(audio::DescribeChain(api, (**chain)->source()));
  }

  for (int second = 1; second <= options.seconds && SleepFor(1s); ++second) {
    Report(Outcome::kNote, std::format("{:>4} s", second),
           std::format("mic {:<14} cable {}", Db((*mic_recorder)->TakePeak()), Db((*cable_recorder)->TakePeak())));
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

void RunMode(runtime::ObsHost& host, const Options& options, const std::vector<audio::AudioDevice>& mics,
             const std::vector<audio::AudioDevice>& outputs) {
  const runtime::ObsApi& api = host.api();
  auto output = audio::FindDevice(outputs, options.output, "playback device");
  auto listen = audio::FindDevice(mics, options.listen, "recording device");
  if (!output || !listen) {
    return Check(false, "devices",
                 (!output ? output.error() : listen.error()) + " Is VB-Cable installed? --list-devices shows what's there.");
  }
  std::optional<audio::AudioDevice> mic;
  if (options.mode == Mode::kRun || options.mode == Mode::kMeasureMic) {
    auto found = audio::FindDevice(mics, options.mic, "recording device");
    if (!found) return Check(false, "devices", found.error());
    mic = *found;
  }
  Report(Outcome::kOk, "devices",
         std::format("{}monitoring to \"{}\", listening on \"{}\"", mic ? std::format("mic \"{}\", ", mic->name) : "",
                     output->name, listen->name));
  if (!CheckCable(options, *output, *listen)) return;

  std::string json;
  if (options.source) {
    auto text = ReadText(*options.source);
    if (!text) return Check(false, "chain", text.error());
    json = std::move(*text);
  } else {
    json = audio::MicWithGainSourceJson(mic ? mic->id : "default", options.gain_db);
  }
  if (!options.external) {
    const auto set = audio::SetMonitoringDevice(api, *output);
    if (!set) return Check(false, "monitoring", set.error());
  }

  switch (options.mode) {
    case Mode::kMeasureOutput:
      return MeasureOutput(host, options, *listen, json);
    case Mode::kMeasureCable:
      return MeasureCable(options, *output, *listen);
    case Mode::kRun:
      return RunMic(host, options, *mic, *listen, json);
    case Mode::kMeasureMic:
      return MeasureMic(host, options, *mic, *listen, json);
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
  runtime::HostOptions host_options;
  host_options.obs_dir = options.obs_dir;
  host_options.log_prefix = L"live ";
  host_options.verbose = options.verbose;
  auto host = runtime::ObsHost::Start(host_options);
  if (!host) {
    Check(false, "start libobs", host.error());
    return kExitFail;
  }
  Check(true, "start libobs", std::format("OBS {}, no video", (*host)->install().version.ToString()));

  const auto mics = audio::ListMicDevices((*host)->api());
  const auto outputs = audio::ListMonitoringDevices((*host)->api());
  if (options.mode == Mode::kListDevices) {
    PrintDevices("Recording devices (--mic, --listen):", mics);
    PrintDevices("Playback devices (--output):", outputs);
  } else {
    RunMode(**host, options, mics, outputs);
  }

  const long leaks = (*host)->Shutdown();
  Check(leaks == 0, "shutdown",
        leaks == 0 ? "0 leaked allocations" : std::format("{} libobs allocations leaked", leaks));
  Report(Outcome::kNote, "libobs log",
         std::format("{} ({} warnings/errors)", ToUtf8((*host)->log().path()), (*host)->log().problem_count()));
  for (const std::string& line : (*host)->log().RecentProblems()) Print(std::format("{:26}{}\n", "", line));
  Print(AnyFailed() ? "FAIL\n" : "PASS\n");
  return AnyFailed() ? kExitFail : kExitPass;
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
