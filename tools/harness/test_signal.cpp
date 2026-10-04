// SPDX-License-Identifier: GPL-2.0-or-later
#include "harness/test_signal.h"

#include <cmath>
#include <cstdint>
#include <numbers>

#include "common/random.h"

namespace knobs::tools {
namespace {

constexpr uint32_t kRate = 48000;
constexpr double kSeconds = 12;
constexpr double kSettleSeconds = 2;
// Covers the default thresholds of the expander (-40), gate (-32/-26),
// upward compressor (-20), compressor (-18) and limiter (-6).
constexpr double kBurstLevelsDb[] = {-40, -30, -20, -12, -6, -1};
constexpr uint32_t kSeed = 0x6B6E6F62;  // "knob"

double DbToGain(double db) { return std::pow(10.0, db / 20.0); }

}  // namespace

FloatAudio MakeTestSignal() {
  FloatAudio audio;
  audio.sample_rate = kRate;
  audio.channels = 2;
  const size_t frames = static_cast<size_t>(kSeconds * kRate);
  audio.samples.resize(frames * 2);

  XorShift32 random(kSeed);
  const double floor = DbToGain(-60);
  for (size_t i = 0; i < frames; ++i) {
    audio.samples[2 * i] = static_cast<float>(floor * random.Uniform());
    audio.samples[2 * i + 1] = static_cast<float>(floor * random.Uniform());
  }

  size_t start = static_cast<size_t>(kSettleSeconds * kRate);
  for (int burst = 0;; ++burst) {
    const size_t length = static_cast<size_t>(random.Between(0.15, 0.6) * kRate);
    if (start + length > frames) break;
    const double level = DbToGain(kBurstLevelsDb[burst % std::size(kBurstLevelsDb)]);
    const double fundamental = random.Between(100, 250);
    // Every fourth burst starts hard, to exercise attack times.
    const size_t ramp = static_cast<size_t>((burst % 4 == 3 ? 0.001 : 0.01) * kRate);
    for (size_t i = 0; i < length; ++i) {
      const double t = static_cast<double>(i) / kRate;
      double voice = 0;
      for (int harmonic = 1; harmonic <= 8; ++harmonic) {
        voice += std::sin(2 * std::numbers::pi * fundamental * harmonic * t) / harmonic;
      }
      voice = 0.8 * voice / 2.72 + 0.2 * random.Uniform();  // 2.72 ~ the harmonics' sum.
      const size_t edge = std::min(i, length - 1 - i);
      const double shape =
          edge < ramp ? 0.5 - 0.5 * std::cos(std::numbers::pi * static_cast<double>(edge) / ramp) : 1.0;
      const double left = level * shape * voice;
      audio.samples[2 * (start + i)] += static_cast<float>(left);
      audio.samples[2 * (start + i) + 1] += static_cast<float>(0.7 * left + 0.05 * level * random.Uniform());
    }
    start += length + static_cast<size_t>(random.Between(0.05, 0.4) * kRate);
  }
  return audio;
}

}  // namespace knobs::tools
