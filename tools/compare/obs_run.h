// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "runtime/obs_api.h"
#include "runtime/obs_install.h"
#include "util/result.h"

// Scripted runs of the user's own OBS, for knobs-compare: OBS records a
// source's audio through its own output path, with nobody at the controls.
namespace knobs::tools {

// A private copy of `install` in `base`\obs-<version>, made once per install
// and set to portable mode, so OBS keeps a run's settings next to the copy
// and never touches the user's own. Leaves out what a run doesn't need: debug
// symbols, the browser source and its Chromium files, obs-websocket, and a
// portable install's own settings.
Result<std::filesystem::path> PrepareObsCopy(const runtime::ObsInstall& install, const std::filesystem::path& base);

struct ObsRecording {
  std::filesystem::path copy;  // From PrepareObsCopy.
  runtime::ObsVersion version;
  // The imported mic (import::ImportedMic::source_json). OBS plays `input`
  // through a Media Source with its filters and source-level settings.
  std::string mic_json;
  // Where the mic came from, which decides how OBS loads it: as an entry of
  // "sources" in the scene, with its load callbacks, or as the Mic/Aux device.
  bool mic_from_sources = false;
  std::filesystem::path input;
  uint32_t sample_rate = 48000;
  std::string channel_setup = "Stereo";  // As in a profile's basic.ini.
  // OBS's output timer stops the recording after this long.
  int seconds = 0;
};

struct ObsRecordingResult {
  std::filesystem::path wav;
  std::filesystem::path log;  // OBS's own log of the run.
  // OBS didn't close when asked and was ended, after it had finished the
  // recording.
  bool ended = false;
};

// Writes fresh settings into the copy and runs it: OBS loads the source,
// starts recording track 1 to a 32-bit float WAV through its custom FFmpeg
// output, stops when the output timer runs out, and is closed. OBS starts
// minimized to the tray and opens no audio device: nothing in its settings
// monitors or captures. `api` builds the source.
Result<ObsRecordingResult> RecordWithObs(const runtime::ObsApi& api, const ObsRecording& run);

}  // namespace knobs::tools
