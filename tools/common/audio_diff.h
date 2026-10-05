// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

#include "common/wav.h"
#include "util/result.h"

// Comparing two renderings of the same audio, such as OBS's and knobs's
// output for one input.
namespace knobs::tools {

// How `b` lines up with `a`: b's frame i + offset holds a's frame i.
struct Alignment {
  int64_t offset = 0;
  // Of the two energy envelopes at the coarse offset: 1 is the same shape.
  double correlation = 0;
};

// Lines up two renderings at the same rate and channel count, trying offsets
// from `min_offset` to `max_offset` frames. First by how well their energy
// envelopes in 1 ms bins correlate where they overlap, then to the frame: the
// offset within 2 ms of that with the least residual over the loudest second
// they share.
Result<Alignment> AlignAudio(const FloatAudio& a, const FloatAudio& b, int64_t min_offset, int64_t max_offset);

struct AudioDiff {
  size_t frames = 0;  // Compared: where both have audio at the offset.
  double signal_rms = 0;
  double residual_rms = 0;  // Of a - b. NaN if either has a NaN.
  double residual_peak = 0;  // NaN if either has a NaN.
  // The 100 ms window with the loudest residual, by RMS.
  double worst_window_rms = 0;
  size_t worst_window_frame = 0;  // In a.
  // The gain on b that best matches a (least squares). Telling for level
  // mismatches, such as Mono or volume handled differently.
  double gain = 1;
  size_t samples = 0;
  // Bit for bit: 0.0 and -0.0 differ.
  size_t identical_samples = 0;

  bool bit_identical() const { return samples > 0 && identical_samples == samples; }
};

// Compares a with b at `offset` (see Alignment), over the frames both have.
AudioDiff DiffAudio(const FloatAudio& a, const FloatAudio& b, int64_t offset);

// a - b at `offset`, over the frames both have.
FloatAudio Residual(const FloatAudio& a, const FloatAudio& b, int64_t offset);

struct MixChanges {
  size_t clamped = 0;         // Past [-1, 1], or NaN.
  size_t negative_zeros = 0;  // -0.0, which became 0.0.
};

// What libobs's mix does to a source's audio before an output such as the
// custom FFmpeg output gets it. It adds the audio into a zeroed buffer
// (obs-audio.c, mix_audio), so -0.0 becomes 0.0, then turns NaN into 0 and
// clamps to [-1, 1] (audio-io.c, clamp_audio_output). The monitor does none
// of that.
MixChanges MixLikeObs(FloatAudio& audio);

// 20 log10(x), or -inf for 0.
double ToDb(double amplitude);

}  // namespace knobs::tools
