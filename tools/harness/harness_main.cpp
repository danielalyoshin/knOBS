// SPDX-License-Identifier: GPL-2.0-or-later
//
// knobs-harness: runs audio through an OBS filter chain offline and checks
// that the output is bit-identical from run to run.
//
// No audio device is opened: see RunChainOffline (common/offline_chain.h).
// Each run loads the chain afresh, so no filter state carries over.

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
#include "common/obs_import.h"
#include "common/offline_chain.h"
#include "common/push_source.h"
#include "common/sha256.h"
#include "common/text_file.h"
#include "common/wav.h"
#include "common/test_signal.h"
#include "runtime/obs_host.h"
#include "runtime/obs_install.h"
#include "util/win_strings.h"

namespace {

using namespace knobs;
using namespace knobs::tools;
namespace fs = std::filesystem;

// The built-in test signal's rate.
constexpr uint32_t kSampleRate = 48000;
// Pushed after the input so filters that hold audio back flush it (noise
// suppression works in 10 ms segments).
constexpr uint32_t kTailSeconds = 1;
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

  --in <file.wav>          32-bit float, mono or stereo, at the chain's sample rate.
                           Default: a built-in 12 s, 48 kHz test signal.
  --import                 The chain: the mic from OBS's active profile and scene
                           collection, at the profile's sample rate and channels.
                           Its filters and the source-level state libobs applies
                           before them (balance, Mono) are used; its device isn't.
{}
  --source <file.json>     The chain, as an OBS source object (one entry of a scene
                           collection's "sources"), used the same way.
  --chain coverage|gain    A built-in chain instead: every obs-filters audio filter
                           (default), or {}'s M1 chain, one gain filter at 0 dB.
  --out <file.wav>         Write the output: 32-bit float at the chain's sample rate
                           and channels. It's what the filters output, before the
                           source's volume, which libobs applies afterwards (in the
                           monitor and the mix).
  --save-input <file.wav>  Write the input, e.g. to reuse the built-in signal.
  --chunk <frames>         Frames per push (default 480: 10 ms, WASAPI's usual packet).
  --runs <n>               Runs to compare (default 3, at most {}).
  --obs-dir <folder>       Use this OBS install instead of searching for one.
  --verbose                Echo the libobs log, including debug lines.

After the input, {} s of silence is pushed so buffered filters flush, and the
output keeps it.
)",
                     kImportUsage, kDisplayName, kMaxRuns, kTailSeconds);
}

struct Options {
  std::optional<fs::path> in;
  bool import = false;
  ImportArgs import_args;
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
    if (arg == L"--verbose" || arg == L"--import") {
      (arg == L"--verbose" ? options.verbose : options.import) = true;
      continue;
    }
    bool bad = false;
    if (ParseImportArg(arg, value, options.import_args, bad)) {
      if (bad) return std::nullopt;
      ++i;
      continue;
    }
    if (!value) return std::nullopt;
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
    } else {
      return std::nullopt;
    }
    ++i;
  }
  // One chain, and the import options only with --import.
  const bool import_args = options.import_args.config_dir || !options.import_args.pick.empty();
  if ((options.import && options.source) || (import_args && !options.import)) return std::nullopt;
  return options;
}

std::string Level(const std::vector<float>& samples) {
  float peak = 0;
  for (const float x : samples) peak = std::max(peak, std::fabs(x));
  return FormatPeak(peak);
}

int Run(const Options& options) {
  Print(std::format("{} offline harness\n", kDisplayName));

  // libobs. An import decides its audio format, so that comes first.
  if (!options.obs_dir && runtime::ObsInstallCandidates().empty()) {
    Report(Outcome::kNote, "find OBS", "OBS isn't installed");
    Print("SKIP (OBS isn't installed)\n");
    return kExitSkip;
  }
  auto install = options.obs_dir ? runtime::InspectObsInstall(*options.obs_dir) : runtime::FindObsInstall();
  if (!install) {
    Check(false, "find OBS", install.error());
    return kExitFail;
  }
  runtime::HostOptions host_options;
  host_options.obs_dir = install->root;
  host_options.log_prefix = L"harness ";
  host_options.verbose = options.verbose;
  std::optional<import::ActiveObsConfig> config;
  if (options.import) {
    auto found = FindObsConfig(options.import_args, *install);
    if (!found) {
      Check(false, "OBS settings", found.error());
      return kExitFail;
    }
    config = std::move(*found);
    UseProfileAudio(*config, host_options);
  }
  const uint32_t rate = host_options.samples_per_sec;

  // Input.
  FloatAudio input;
  if (options.in) {
    auto wav = ReadFloatWav(*options.in);
    if (!wav) {
      Check(false, "input", wav.error());
      return kExitFail;
    }
    if (wav->sample_rate != rate || wav->channels > 2) {
      Check(false, "input",
            std::format("{} is {} Hz with {} channels; the harness takes {} Hz mono or stereo here.",
                        ToUtf8(*options.in), wav->sample_rate, wav->channels, rate));
      return kExitFail;
    }
    input = std::move(*wav);
  } else if (rate != kSampleRate) {
    Check(false, "input",
          std::format("The built-in test signal is {} Hz, and the OBS profile runs at {} Hz. Pass --in with a "
                      "{} Hz file.",
                      kSampleRate, rate, rate));
    return kExitFail;
  } else {
    input = MakeTestSignal();
  }
  Report(Outcome::kNote, "input",
         std::format("{}: {:.2f} s, {} channel(s), peak {}",
                     options.in ? ToUtf8(*options.in) : std::string("built-in test signal"),
                     static_cast<double>(input.frames()) / rate, input.channels, Level(input.samples)));
  if (options.save_input) {
    const auto saved = WriteFloatWav(*options.save_input, input);
    Check(saved.ok(), "save input", saved.ok() ? ToUtf8(*options.save_input) : saved.error());
  }

  auto host = runtime::ObsHost::Start(host_options);
  if (!host) {
    Check(false, "start libobs", host.error());
    return kExitFail;
  }
  const runtime::ObsApi& api = (*host)->api();
  Check(true, "start libobs",
        std::format("OBS {}, {} Hz, {} channel(s), no video, no audio devices", install->version.ToString(),
                    rate, get_audio_channels(host_options.speakers)));
  RegisterPushSource(api);

  // Chain.
  std::string json;
  bool load_callbacks = false;
  if (config) {
    auto imported = ImportMic(api, *config, options.import_args);
    if (!imported) {
      Check(false, "import", imported.error());
      return FinishRun(**host);
    }
    json = std::move(imported->source_json);
    load_callbacks = imported->load_callbacks();
  } else if (options.source) {
    auto text = ReadText(*options.source);
    if (!text) {
      Check(false, "chain", text.error());
      return FinishRun(**host);
    }
    json = std::move(*text);
  } else {
    json = options.chain == "gain" ? audio::MicWithGainSourceJson("default", 0.0) : kCoverageChain;
  }

  // Runs. Stops at the first one that fails; earlier failures, such as
  // --save-input's, don't stop them.
  std::optional<FloatAudio> first;
  std::string first_hash;
  bool run_failed = false;
  for (int run = 1; run <= options.runs && !run_failed; ++run) {
    const auto started = std::chrono::steady_clock::now();
    auto result = RunChainOffline(api, (*host)->session(), json, load_callbacks, input, options.chunk,
                                  kTailSeconds * rate);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (!result) {
      Check(false, "load chain", result.error());
      break;
    }
    if (run == 1) {
      ReportChain(result->chain);
      if (result->chain.volume != 1.0f) {
        Report(Outcome::kNote, "volume",
               std::format("the output leaves out the source's volume ({:.2f}); libobs applies it afterwards",
                           result->chain.volume));
      }
    }
    FloatAudio& output = result->output;
    const std::string hash = Sha256Hex(output.samples.data(), output.samples.size() * sizeof(float));
    const double out_seconds = static_cast<double>(output.frames()) / rate;
    const std::string step = std::format("run {}", run);
    if (hash.empty()) {
      Check(false, step, "couldn't hash the output");
      run_failed = true;
    } else if (!first) {
      run_failed = output.samples.empty();
      Check(!run_failed, step,
            std::format("{:.2f} s out in {:.2f} s ({:.0f}x real time), peak {}, sha256 {}", out_seconds, seconds,
                        out_seconds / seconds, Level(output.samples), hash));
      first = std::move(output);
      first_hash = hash;
    } else {
      const bool same = hash == first_hash && output.samples.size() == first->samples.size();
      run_failed = !same;
      Check(same, step,
            same ? std::format("bit-identical to run 1 ({:.2f} s)", seconds)
                 : std::format("differs from run 1: {} frames, sha256 {}", output.frames(), hash));
    }
  }

  if (first && options.out) {
    const auto written = WriteFloatWav(*options.out, *first);
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
