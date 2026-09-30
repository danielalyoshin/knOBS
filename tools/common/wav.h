// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "util/result.h"

namespace knobs::tools {

// 32-bit float samples, interleaved.
struct FloatAudio {
  uint32_t sample_rate = 0;
  uint32_t channels = 0;
  std::vector<float> samples;

  size_t frames() const { return channels ? samples.size() / channels : 0; }
};

// Reads a WAV file of 32-bit float samples: WAVE_FORMAT_IEEE_FLOAT, or
// WAVE_FORMAT_EXTENSIBLE with the float subtype. Anything else is an error
// rather than a conversion.
Result<FloatAudio> ReadFloatWav(const std::filesystem::path& file);

// Writes a WAVE_FORMAT_IEEE_FLOAT file.
Status WriteFloatWav(const std::filesystem::path& file, const FloatAudio& audio);

}  // namespace knobs::tools
