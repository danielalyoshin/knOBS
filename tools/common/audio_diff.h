// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

#include "common/wav.h"
#include "util/result.h"

// Comparing two renderings of the same audio, such as OBS's and knOBS's
// output for one input.
namespace knobs::tools {

// How `b` lines up with `a`: b's frame i + offset holds a's frame i.
struct Alignment {
  int64_t offset = 0;
  // Of the two energy envelopes at the coarse offset: 1 is the same shape.
  double correlation = 0;
};

// Lines up two renderings at the same rate and channel count that are offset
// by at most `max_offset` frames either way. First by energy envelopes in
// 1 ms bins (EstimateDelay), then to the frame, by the offset within 2 ms of
// that with the least residual energy.
Result<Alignment> AlignAudio(const FloatAudio& a, const FloatAudio& b, size_t max_offset);

struct AudioDiff {
  size_t frames = 0;  // Compared: where both have audio at the offset.
  double signal_rms = 0;
  double residual_rms = 0;  // Of a - b.
  double residual_peak = 0;
  // The 100 ms window with the loudest residual, by RMS.
  double worst_window_rms = 0;
  size_t worst_window_frame = 0;  // In a.
  // The gain on b that best matches a (least squares). Telling for level
  // mismatches, such as Mono or volume handled differently.
  double gain = 1;
  size_t samples = 0;
  size_t identical_samples = 0;
};

// Compares a with b at `offset` (see Alignment), over the frames both have.
AudioDiff DiffAudio(const FloatAudio& a, const FloatAudio& b, int64_t offset);

// a - b at `offset`, over the frames both have.
FloatAudio Residual(const FloatAudio& a, const FloatAudio& b, int64_t offset);

// 20 log10(x), or -inf for 0.
double ToDb(double amplitude);

}  // namespace knobs::tools
