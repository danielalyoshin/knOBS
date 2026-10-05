// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "runtime/obs_install.h"
#include "util/result.h"

// Scripted runs of the user's own OBS, for knobs-compare: OBS records a
// source's audio through its own output path, with nobody at the controls.
namespace knobs::tools {

// A private copy of `install` in `base`\obs-<version>, made once per install
// and set to portable mode, so OBS keeps a run's settings next to the copy
// and never touches the user's own. Leaves out what a run doesn't need: debug
// symbols, the browser source and its Chromium files, and obs-websocket.
Result<std::filesystem::path> PrepareObsCopy(const runtime::ObsInstall& install, const std::filesystem::path& base);

struct ObsRecording {
  std::filesystem::path copy;  // From PrepareObsCopy.
  runtime::ObsVersion version;
  // The source to record, as an OBS source object. It's loaded as OBS's
  // Mic/Aux device, so it's active from the start and mixed into track 1.
  std::string source_json;
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
// monitors or captures.
Result<ObsRecordingResult> RecordWithObs(const ObsRecording& run);

}  // namespace knobs::tools
