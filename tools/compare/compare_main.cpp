// SPDX-License-Identifier: GPL-2.0-or-later
//
// knobs-compare: runs the same audio through the imported mic's chain in
// knobs and in OBS itself, and measures how far apart the outputs are.
//
// knobs's side is the offline run knobs-harness does (common/offline_chain.h).
// OBS's side is a scripted run of a private, portable copy of the user's OBS
// (compare/obs_run.h): the audio plays from a Media Source carrying the mic's
// filters and source settings, and OBS records it with its own output path.
// Neither side opens an audio device.

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "app_info.h"
#include "common/audio_diff.h"
#include "common/console.h"
#include "common/obs_import.h"
#include "common/offline_chain.h"
#include "common/push_source.h"
#include "common/test_signal.h"
#include "common/wav.h"
#include "compare/obs_run.h"
#include "util/app_dirs.h"
#include "util/win_strings.h"

namespace {

using namespace knobs;
using namespace knobs::tools;
namespace fs = std::filesystem;

// Silence before the input. OBS starts recording during it, and both chains
// settle in it.
constexpr double kLeadSeconds = 5;
// Silence after the input, so filters that hold audio back flush it.
constexpr double kTrailSeconds = 2;
// knobs's packets by default: the size OBS's Media Source plays a WAV file in
// (FFmpeg's PCM demuxer aims for 10 packets a second and rounds down to a
// power of two), so both sides filter the same packets, as they do live,
// where both get win-wasapi's.
constexpr uint32_t kDefaultChunk = 4096;
constexpr double kDefaultThresholdDb = -100;

std::string Usage() {
  return std::format(R"(Usage: knobs-compare [options]

Compares {0} with OBS on the same audio. Runs it through the mic imported from
OBS's active profile and scene collection twice: in {0}, offline, and in OBS
itself, as a Media Source with the mic's filters and source settings that OBS
records to a 32-bit float WAV. Then lines the two up and measures the
difference.

OBS runs from a private copy of its install in portable mode, so your OBS
settings aren't touched. The copy is about 125 MB, made once per OBS version.
OBS starts minimized to the tray, records for the length of the input and
closes. No audio device is opened, and nothing plays.

  --in <file.wav>          32-bit float, mono or stereo, at the profile's sample rate.
                           Default: knobs-harness's built-in 12 s test signal (48 kHz).
                           {1} s of silence goes before it and {2} s after.
{3}
  --obs-wav <file.wav>     Don't run OBS; compare with this recording of OBS instead,
                           made from the input.wav in --work-dir, which {0} then
                           runs as it is. Not with --in.
  --work-dir <folder>      Where the OBS copy and each run's input.wav, {0}.wav,
                           obs.wav and residual.wav go. Default: %LocalAppData%\{0}\compare.
  --threshold-db <x>       Pass if the residual's RMS is at least x dB below the
                           signal's (default {4}).
  --chunk <frames>         {0}'s packet size. The default, {5}, is the size OBS's
                           Media Source plays the input in. Other sizes change the
                           output at rounding level, e.g. 480 (10 ms, as WASAPI
                           delivers a mic).
  --obs-dir <folder>       Use this OBS install instead of searching for one.
  --verbose                Echo the libobs log, including debug lines.
)",
                     kDisplayName, kLeadSeconds, kTrailSeconds, kImportUsage, -kDefaultThresholdDb,
                     kDefaultChunk);
}

struct Options {
  std::optional<fs::path> in;
  ImportArgs import_args;
  std::optional<fs::path> obs_wav;
  std::optional<fs::path> work_dir;
  double threshold_db = kDefaultThresholdDb;
  uint32_t chunk = kDefaultChunk;
  std::optional<fs::path> obs_dir;
  bool verbose = false;
};

std::optional<Options> ParseArgs(int argc, wchar_t** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view arg = argv[i];
    const wchar_t* value = i + 1 < argc ? argv[i + 1] : nullptr;
    if (arg == L"--verbose") {
      options.verbose = true;
      continue;
    }
    bool bad = false;
    if (ParseImportArg(arg, value, options.import_args, bad)) {
      if (bad) return std::nullopt;
    } else if (!value) {
      return std::nullopt;
    } else if (arg == L"--in") {
      options.in = fs::absolute(value);
    } else if (arg == L"--obs-wav") {
      options.obs_wav = fs::absolute(value);
    } else if (arg == L"--work-dir") {
      options.work_dir = fs::absolute(value);
    } else if (arg == L"--threshold-db") {
      wchar_t* end = nullptr;
      const double x = std::wcstod(value, &end);
      if (end == value || *end != L'\0' || !std::isfinite(x) || x <= 0) return std::nullopt;
      options.threshold_db = -x;
    } else if (arg == L"--chunk") {
      wchar_t* end = nullptr;
      const unsigned long frames = std::wcstoul(value, &end, 10);
      if (end == value || *end != L'\0' || frames == 0 || frames > 48000) return std::nullopt;
      options.chunk = static_cast<uint32_t>(frames);
    } else if (arg == L"--obs-dir") {
      options.obs_dir = fs::absolute(value);
    } else {
      return std::nullopt;
    }
    ++i;
  }
  if (options.in && options.obs_wav) return std::nullopt;
  return options;
}

std::string Seconds(double frames, uint32_t rate) { return std::format("{:.2f} s", frames / rate); }

// OBS's side: a recording of the input through OBS.
Result<FloatAudio> RecordObs(const runtime::ObsApi& api, const runtime::ObsInstall& install,
                             const import::ActiveObsConfig& config, const import::ImportedMic& mic,
                             const fs::path& work_dir, const fs::path& input_file, double input_seconds) {
  Report(Outcome::kNote, "OBS copy", std::format("checking the copy of OBS {} in {} (made once per version)",
                                                 install.version.ToString(), ToUtf8(work_dir)));
  auto copy = PrepareObsCopy(install, work_dir);
  if (!copy) return Error{copy.error()};
  ObsRecording run;
  run.copy = *copy;
  run.version = install.version;
  run.mic_json = mic.source_json;
  run.mic_from_sources = mic.load_callbacks();
  run.input = input_file;
  run.sample_rate = config.audio.sample_rate;
  run.channel_setup = config.audio.channel_setup;
  // OBS starts recording within a second or two of loading the source, so
  // this covers the input with room to spare.
  run.seconds = static_cast<int>(std::ceil(input_seconds)) + 3;
  Report(Outcome::kNote, "OBS",
         std::format("running OBS {} from {} for about {} s, with the source {}", install.version.ToString(),
                     ToUtf8(*copy), run.seconds,
                     run.mic_from_sources ? "in the scene, as the mic was" : "as Mic/Aux, as the mic was"));
  auto recorded = RecordWithObs(api, run);
  if (!recorded) return Error{recorded.error()};
  if (recorded->ended) Report(Outcome::kWarn, "OBS", "OBS didn't close when asked after recording, so it was ended");
  auto wav = ReadFloatWav(recorded->wav);
  if (!wav) return Error{wav.error()};
  Check(true, "OBS", std::format("recorded {} through OBS's own output; its log: {}",
                                 Seconds(static_cast<double>(wav->frames()), wav->sample_rate), ToUtf8(recorded->log)));
  const fs::path kept = work_dir / L"obs.wav";
  std::error_code ec;
  fs::copy_file(recorded->wav, kept, fs::copy_options::overwrite_existing, ec);
  return wav;
}

int Run(const Options& options) {
  Print(std::format("{} vs OBS\n", kDisplayName));
  auto started = StartTool({.obs_dir = options.obs_dir,
                            .log_prefix = L"compare ",
                            .verbose = options.verbose,
                            .import = true,
                            .import_args = options.import_args});
  if (const int* code = std::get_if<int>(&started)) return *code;
  auto& [host, config] = std::get<StartedTool>(started);
  const runtime::ObsApi& api = host->api();
  const uint32_t rate = config->audio.sample_rate;

  auto dirs = GetAppDirs();
  if (!dirs && !options.work_dir) {
    Check(false, "work folder", dirs.error());
    return FinishRun(*host);
  }
  const fs::path work_dir = options.work_dir ? *options.work_dir : dirs->local / L"compare";
  std::error_code ec;
  fs::create_directories(work_dir, ec);
  const fs::path input_file = work_dir / L"input.wav";

  // The input, with silence around it. A given OBS recording was made from
  // input.wav, which already has it.
  FloatAudio padded;
  const size_t lead = static_cast<size_t>(kLeadSeconds * rate);
  if (options.obs_wav) {
    auto wav = ReadFloatWav(input_file);
    if (!wav) {
      Check(false, "input", wav.error());
      return FinishRun(*host);
    }
    padded = std::move(*wav);
  } else {
    FloatAudio input;
    if (options.in) {
      auto wav = ReadFloatWav(*options.in);
      if (!wav) {
        Check(false, "input", wav.error());
        return FinishRun(*host);
      }
      input = std::move(*wav);
    } else {
      input = MakeTestSignal();
    }
    const size_t trail = static_cast<size_t>(kTrailSeconds * rate);
    padded = {input.sample_rate, input.channels, std::vector<float>(lead * input.channels, 0.0f)};
    padded.samples.insert(padded.samples.end(), input.samples.begin(), input.samples.end());
    padded.samples.resize(padded.samples.size() + trail * input.channels, 0.0f);
  }
  if (padded.sample_rate != rate || padded.channels == 0 || padded.channels > 2) {
    Check(false, "input",
          std::format("The input is {} Hz with {} channels; the OBS profile needs {} Hz, mono or stereo.",
                      padded.sample_rate, padded.channels, rate));
    return FinishRun(*host);
  }
  if (!options.obs_wav) {
    const auto written = WriteFloatWav(input_file, padded);
    if (!written) {
      Check(false, "input", written.error());
      return FinishRun(*host);
    }
  }
  Report(Outcome::kNote, "input",
         std::format("{}: {} with the silence around it, {} channel(s), peak {}",
                     options.in        ? ToUtf8(*options.in)
                     : options.obs_wav ? ToUtf8(input_file)
                                       : "built-in test signal",
                     Seconds(static_cast<double>(padded.frames()), rate), padded.channels,
                     FormatPeakOf(padded.samples)));

  RegisterPushSource(api);
  auto imported = ImportMic(api, *config, options.import_args);
  if (!imported) {
    Check(false, "import", imported.error());
    return FinishRun(*host);
  }

  // knobs's side.
  auto ran = RunChainOffline(api, host->session(), imported->source_json, imported->load_callbacks(), padded,
                             options.chunk, 0);
  if (!ran) {
    Check(false, kDisplayName, ran.error());
    return FinishRun(*host);
  }
  ReportChain(ran->chain);
  FloatAudio& ours = ran->output;
  const auto ours_written = WriteFloatWav(work_dir / (std::wstring(kDisplayNameW) + L".wav"), ours);
  Check(ours_written.ok(), kDisplayName,
        ours_written ? std::format("{} through the chain offline in {}-frame packets, peak {}",
                                   Seconds(static_cast<double>(padded.frames()), rate), options.chunk,
                                   FormatPeakOf(ours.samples))
                     : ours_written.error());
  // Compared only up to a second before the end of the file: OBS's Media
  // Source drops the file's last few milliseconds as it stops, and its mix
  // fills zeros in where the filters still ring at around -240 dBFS. The
  // offline run also pads its last packet with silence.
  const size_t compared_frames = padded.frames() > rate ? padded.frames() - rate : 0;
  ours.samples.resize(std::min(ours.samples.size(), compared_frames * ours.channels));
  // OBS records its mix, which changes a few values; the cable gets the
  // filters' output as it is, from both.
  const MixChanges mixed = MixLikeObs(ours);
  if (mixed.clamped) {
    Report(Outcome::kNote, "mix",
           std::format("{} of {}'s samples are past 0 dBFS or NaN. OBS's recording mix clamps them, so they're "
                       "clamped here too; the cable gets them as they are, from both.",
                       mixed.clamped, kDisplayName));
  }
  if (mixed.negative_zeros) {
    Report(Outcome::kNote, "mix",
           std::format("{} of {}'s samples are -0.0, which OBS's recording mix turns into 0.0, so they're turned "
                       "here too; the cable gets -0.0 from both.",
                       mixed.negative_zeros, kDisplayName));
  }

  // OBS's side.
  FloatAudio theirs;
  if (options.obs_wav) {
    auto wav = ReadFloatWav(*options.obs_wav);
    if (!wav) {
      Check(false, "OBS", wav.error());
      return FinishRun(*host);
    }
    theirs = std::move(*wav);
    Check(true, "OBS", std::format("{}: {}", ToUtf8(*options.obs_wav),
                                   Seconds(static_cast<double>(theirs.frames()), theirs.sample_rate)));
  } else {
    auto recorded = RecordObs(api, host->install(), *config, *imported, work_dir, input_file,
                              static_cast<double>(padded.frames()) / rate);
    if (!recorded) {
      Check(false, "OBS", recorded.error());
      return FinishRun(*host);
    }
    theirs = std::move(*recorded);
  }

  // The comparison. OBS's frame i + offset is knobs's frame i, and knobs's
  // frame 0 is the input's first, so OBS started recording -offset frames
  // into the input: usually a fraction of a second, and at most a little
  // before the input starts playing.
  const int64_t lead_frames = static_cast<int64_t>(lead);
  auto alignment = AlignAudio(ours, theirs, -(lead_frames + 3 * int64_t{rate}), 2 * int64_t{rate});
  if (!alignment) {
    Check(false, "alignment", alignment.error());
    return FinishRun(*host);
  }
  const int64_t start = -alignment->offset;
  Check(true, "alignment",
        std::format("OBS's recording starts {:.3f} s into the input (envelope correlation {:.3f})",
                    static_cast<double>(start) / rate, alignment->correlation));
  if (start > lead_frames) {
    Report(Outcome::kWarn, "alignment", "OBS started recording after the input's sound began, so the start of it "
                                        "isn't compared");
  }
  const AudioDiff diff = DiffAudio(ours, theirs, alignment->offset);
  const std::string compared = Seconds(static_cast<double>(diff.frames), rate);
  if (diff.bit_identical()) {
    Check(true, "residual", std::format("none: bit-identical over {}", compared));
  } else if (diff.residual_rms == 0) {
    Check(true, "residual",
          std::format("none in value over {}, but {} samples differ bit for bit, in the sign of zero", compared,
                      diff.samples - diff.identical_samples));
  } else {
    const double residual_db = ToDb(diff.residual_rms) - ToDb(diff.signal_rms);
    Check(diff.frames > 0 && residual_db <= options.threshold_db, "residual",
          std::format("{:.1f} dB below the signal over {} (RMS; passes at {:.0f} dB or more)", -residual_db,
                      compared, -options.threshold_db));
    Report(Outcome::kNote, "residual",
           std::format("peak {:.1f} dBFS; loudest 100 ms at {:.2f} s: {:.1f} dBFS (RMS)", ToDb(diff.residual_peak),
                       static_cast<double>(diff.worst_window_frame) / rate, ToDb(diff.worst_window_rms)));
    Report(Outcome::kNote, "residual",
           std::format("{:.2f}% of samples identical; best-fit gain on OBS's {:+.6f} dB",
                       100.0 * static_cast<double>(diff.identical_samples) / static_cast<double>(diff.samples),
                       ToDb(diff.gain)));
  }
  const auto residual_written = WriteFloatWav(work_dir / L"residual.wav", Residual(ours, theirs, alignment->offset));
  Report(residual_written ? Outcome::kNote : Outcome::kWarn, "files",
         residual_written ? ToUtf8(work_dir) : residual_written.error());
  return FinishRun(*host);
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
