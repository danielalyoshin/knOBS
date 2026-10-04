// SPDX-License-Identifier: GPL-2.0-or-later
//
// knobs-harness: runs audio through an OBS filter chain offline and checks
// that the output is bit-identical from run to run.
//
// No audio device is opened. A push source (tools/common/push_source.h)
// stands in for the mic: it outputs the input in fixed-size chunks with
// synthetic, gapless timestamps, and libobs runs the chain on this thread
// before each push returns. An audio capture callback on the source, the hook
// libobs's monitor uses, collects what the filters output. Each run loads the
// chain afresh, so no filter state carries over.

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app_info.h"
#include "audio/live_chain.h"
#include "common/console.h"
#include "common/push_source.h"
#include "common/sha256.h"
#include "common/text_file.h"
#include "common/wav.h"
#include "harness/test_signal.h"
#include "runtime/obs_host.h"
#include "runtime/obs_install.h"
#include "util/win_strings.h"

namespace {

using namespace knobs;
using namespace knobs::tools;
namespace fs = std::filesystem;

constexpr uint32_t kSampleRate = 48000;
// Pushed after the input so filters that hold audio back flush it (noise
// suppression works in 10 ms segments).
constexpr uint32_t kTailFrames = kSampleRate;
// Where the synthetic timestamps start. Only their spacing matters.
constexpr uint64_t kFirstTimestampNs = 1'000'000'000;
constexpr int kMaxRuns = 100;

// Every audio filter obs-filters 32.2 registers, once, mostly at its
// defaults. Two noise suppressors in a row is odd for a real chain, but it
// covers both methods.
constexpr char kCoverageChain[] = R"({
  "id": "knobs_push_source", "name": "Harness",
  "filters": [
    {"id": "noise_suppress_filter", "versioned_id": "noise_suppress_filter_v2", "name": "RNNoise",
     "settings": {"method": "rnnoise"}},
    {"id": "noise_suppress_filter", "versioned_id": "noise_suppress_filter_v2", "name": "Speex",
     "settings": {"method": "speex"}},
    {"id": "invert_polarity_filter", "versioned_id": "invert_polarity_filter", "name": "Invert Polarity"},
    {"id": "basic_eq_filter", "versioned_id": "basic_eq_filter", "name": "3-Band EQ",
     "settings": {"low": 2.0, "mid": -1.5, "high": 3.0}},
    {"id": "noise_gate_filter", "versioned_id": "noise_gate_filter", "name": "Noise Gate"},
    {"id": "expander_filter", "versioned_id": "expander_filter", "name": "Expander"},
    {"id": "upward_compressor_filter", "versioned_id": "upward_compressor_filter", "name": "Upward Compressor"},
    {"id": "compressor_filter", "versioned_id": "compressor_filter", "name": "Compressor"},
    {"id": "gain_filter", "versioned_id": "gain_filter", "name": "Gain", "settings": {"db": 6.0}},
    {"id": "limiter_filter", "versioned_id": "limiter_filter", "name": "Limiter"}
  ]
})";

std::string Usage() {
  return std::format(R"(Usage: knobs-harness [options]

Runs audio through an OBS filter chain offline, with the installed OBS's own
filter code and no audio devices, and checks that every run's output is
bit-identical.

  --in <file.wav>          48 kHz 32-bit float, mono or stereo. Default: a built-in
                           12 s test signal.
  --source <file.json>     The chain, as an OBS source object (one entry of a scene
                           collection's "sources"). Its filters and the source-level
                           state libobs applies before them (balance, Mono) are used;
                           its source type isn't.
  --chain coverage|gain    A built-in chain instead: every obs-filters audio filter
                           (default), or {}'s M1 chain, one gain filter at 0 dB.
  --out <file.wav>         Write the output: 32-bit float, stereo, 48 kHz. It's what
                           the filters output, before the source's volume, which
                           libobs applies afterwards (in the monitor and the mix).
  --save-input <file.wav>  Write the input, e.g. to reuse the built-in signal.
  --chunk <frames>         Frames per push (default 480: 10 ms, WASAPI's usual packet).
  --runs <n>               Runs to compare (default 3, at most {}).
  --obs-dir <folder>       Use this OBS install instead of searching for one.
  --verbose                Echo the libobs log, including debug lines.

After the input, {} s of silence is pushed so buffered filters flush, and the
output keeps it.
)",
                     kDisplayName, kMaxRuns, kTailFrames / kSampleRate);
}

struct Options {
  std::optional<fs::path> in;
  std::optional<fs::path> source;
  std::string chain = "coverage";
  std::optional<fs::path> out;
  std::optional<fs::path> save_input;
  uint32_t chunk = 480;
  int runs = 3;
  std::optional<fs::path> obs_dir;
  bool verbose = false;
};

std::optional<long> ParseInt(const wchar_t* text, long min, long max) {
  wchar_t* end = nullptr;
  const long value = std::wcstol(text, &end, 10);
  if (end == text || *end != L'\0' || value < min || value > max) return std::nullopt;
  return value;
}

std::optional<Options> ParseArgs(int argc, wchar_t** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view arg = argv[i];
    const wchar_t* value = i + 1 < argc ? argv[i + 1] : nullptr;
    if (!value && arg != L"--verbose") return std::nullopt;
    if (arg == L"--in") {
      options.in = fs::absolute(value);
    } else if (arg == L"--source") {
      options.source = fs::absolute(value);
    } else if (arg == L"--chain" && (value == std::wstring_view(L"coverage") ||
                                     value == std::wstring_view(L"gain"))) {
      options.chain = ToUtf8(std::wstring_view(value));
    } else if (arg == L"--out") {
      options.out = fs::absolute(value);
    } else if (arg == L"--save-input") {
      options.save_input = fs::absolute(value);
    } else if (arg == L"--chunk") {
      const auto chunk = ParseInt(value, 1, kSampleRate);
      if (!chunk) return std::nullopt;
      options.chunk = static_cast<uint32_t>(*chunk);
    } else if (arg == L"--runs") {
      const auto runs = ParseInt(value, 1, kMaxRuns);
      if (!runs) return std::nullopt;
      options.runs = static_cast<int>(*runs);
    } else if (arg == L"--obs-dir") {
      options.obs_dir = fs::absolute(value);
    } else if (arg == L"--verbose") {
      options.verbose = true;
      continue;
    } else {
      return std::nullopt;
    }
    ++i;
  }
  return options;
}

std::string Level(const std::vector<float>& samples) {
  float peak = 0;
  for (const float x : samples) peak = std::max(peak, std::fabs(x));
  return FormatPeak(peak);
}

// What the capture callback has collected, interleaved.
struct Collected {
  uint32_t channels = 0;
  std::vector<float> samples;
};

void OnAudio(void* param, obs_source_t*, const audio_data* audio, bool) {
  // Called on the pushing thread, from within obs_source_output_audio.
  auto* out = static_cast<Collected*>(param);
  const size_t base = out->samples.size();
  out->samples.resize(base + size_t{audio->frames} * out->channels);
  for (uint32_t c = 0; c < out->channels; ++c) {
    const auto* plane = reinterpret_cast<const float*>(audio->data[c]);
    if (!plane) continue;
    for (uint32_t i = 0; i < audio->frames; ++i) out->samples[base + size_t{i} * out->channels + c] = plane[i];
  }
}

// Loads the chain as a push source, pushes the input and the tail through
// it, and returns the output.
Result<Collected> RunOnce(const runtime::ObsApi& api, runtime::ObsSession& session, const std::string& json,
                          const FloatAudio& input, uint32_t chunk, bool report_chain) {
  auto source = audio::LoadSourceJson(api, json, kPushSourceId);
  if (!source) return Error{source.error()};
  if (report_chain) {
    const audio::ChainInfo chain = audio::DescribeChain(api, *source);
    ReportChain(chain);
    if (chain.volume != 1.0f) {
      Report(Outcome::kNote, "volume",
             std::format("the output leaves out the source's volume ({:.2f}); libobs applies it afterwards",
                         chain.volume));
    }
  }

  Collected collected;
  collected.channels = get_audio_channels(session.options().speakers);
  api.obs_source_add_audio_capture_callback(*source, OnAudio, &collected);

  const size_t in_frames = input.frames();
  const size_t total = (in_frames + kTailFrames + chunk - 1) / chunk * chunk;
  std::vector<std::vector<float>> planes(input.channels, std::vector<float>(chunk));
  std::vector<const float*> pointers;
  for (const auto& plane : planes) pointers.push_back(plane.data());
  for (size_t start = 0; start < total; start += chunk) {
    for (uint32_t c = 0; c < input.channels; ++c) {
      for (size_t i = 0; i < chunk; ++i) {
        const size_t frame = start + i;
        planes[c][i] = frame < in_frames ? input.samples[frame * input.channels + c] : 0.0f;
      }
    }
    const uint64_t timestamp = kFirstTimestampNs + start * 1'000'000'000 / kSampleRate;
    PushAudio(api, *source, pointers.data(), input.channels, chunk, kSampleRate, timestamp);
  }

  api.obs_source_remove_audio_capture_callback(*source, OnAudio, &collected);
  api.obs_source_release(*source);
  session.DrainDestroyQueue();
  return collected;
}

int Run(const Options& options) {
  Print(std::format("{} offline harness\n", kDisplayName));

  // Input.
  FloatAudio input;
  if (options.in) {
    auto wav = ReadFloatWav(*options.in);
    if (!wav) {
      Check(false, "input", wav.error());
      return kExitFail;
    }
    if (wav->sample_rate != kSampleRate || wav->channels > 2) {
      Check(false, "input",
            std::format("{} is {} Hz with {} channels; the harness takes 48000 Hz mono or stereo.",
                        ToUtf8(*options.in), wav->sample_rate, wav->channels));
      return kExitFail;
    }
    input = std::move(*wav);
  } else {
    input = MakeTestSignal();
  }
  Report(Outcome::kNote, "input",
         std::format("{}: {:.2f} s, {} channel(s), peak {}",
                     options.in ? ToUtf8(*options.in) : std::string("built-in test signal"),
                     static_cast<double>(input.frames()) / kSampleRate, input.channels, Level(input.samples)));
  if (options.save_input) {
    const auto saved = WriteFloatWav(*options.save_input, input);
    Check(saved.ok(), "save input", saved.ok() ? ToUtf8(*options.save_input) : saved.error());
  }

  // Chain.
  std::string json;
  if (options.source) {
    auto text = ReadText(*options.source);
    if (!text) {
      Check(false, "chain", text.error());
      return kExitFail;
    }
    json = std::move(*text);
  } else {
    json = options.chain == "gain" ? audio::MicWithGainSourceJson("default", 0.0) : kCoverageChain;
  }

  // libobs.
  if (!options.obs_dir && runtime::ObsInstallCandidates().empty()) {
    Report(Outcome::kNote, "find OBS", "OBS isn't installed");
    Print("SKIP (OBS isn't installed)\n");
    return kExitSkip;
  }
  runtime::HostOptions host_options;
  host_options.obs_dir = options.obs_dir;
  host_options.log_prefix = L"harness ";
  host_options.verbose = options.verbose;
  auto host = runtime::ObsHost::Start(host_options);
  if (!host) {
    Check(false, "start libobs", host.error());
    return kExitFail;
  }
  const runtime::ObsApi& api = (*host)->api();
  Check(true, "start libobs",
        std::format("OBS {}, no video, no audio devices", (*host)->install().version.ToString()));
  RegisterPushSource(api);

  // Runs. Stops at the first one that fails; earlier failures, such as
  // --save-input's, don't stop them.
  std::optional<Collected> first;
  std::string first_hash;
  bool run_failed = false;
  for (int run = 1; run <= options.runs && !run_failed; ++run) {
    const auto started = std::chrono::steady_clock::now();
    auto output = RunOnce(api, (*host)->session(), json, input, options.chunk, run == 1);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (!output) {
      Check(false, "load chain", output.error());
      break;
    }
    const std::string hash = Sha256Hex(output->samples.data(), output->samples.size() * sizeof(float));
    const double out_seconds = static_cast<double>(output->samples.size() / output->channels) / kSampleRate;
    const std::string step = std::format("run {}", run);
    if (hash.empty()) {
      Check(false, step, "couldn't hash the output");
      run_failed = true;
    } else if (!first) {
      run_failed = output->samples.empty();
      Check(!run_failed, step,
            std::format("{:.2f} s out in {:.2f} s ({:.0f}x real time), peak {}, sha256 {}", out_seconds,
                        seconds, out_seconds / seconds, Level(output->samples), hash));
      first = std::move(*output);
      first_hash = hash;
    } else {
      const bool same = hash == first_hash && output->samples.size() == first->samples.size();
      run_failed = !same;
      Check(same, step,
            same ? std::format("bit-identical to run 1 ({:.2f} s)", seconds)
                 : std::format("differs from run 1: {} frames, sha256 {}",
                               output->samples.size() / output->channels, hash));
    }
  }

  if (first && options.out) {
    const auto written = WriteFloatWav(*options.out, {kSampleRate, first->channels, first->samples});
    Check(written.ok(), "output", written.ok() ? ToUtf8(*options.out) : written.error());
  }
  return FinishRun(**host);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // Plain LoadLibrary calls skip PATH and the working directory; see
  // ObsRuntime::Load.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  SetConsoleOutputCP(CP_UTF8);
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
