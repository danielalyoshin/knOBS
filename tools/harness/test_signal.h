// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/wav.h"

namespace knobs::tools {

// A deterministic 12 s, 48 kHz stereo signal for exercising dynamics filters:
// 2 s of a -60 dBFS noise floor (so envelopes settle), then voice-like
// harmonic bursts at levels from -40 to -1 dBFS, some with sharp attacks,
// over the same floor. The right channel is a quieter mix of the left with
// its own noise, so filters that link channels see different ones.
FloatAudio MakeTestSignal();

}  // namespace knobs::tools
