// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/audio_diff.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <limits>
#include <vector>

#include "common/envelope.h"

namespace knobs::tools {
namespace {

constexpr uint64_t kBinNs = 1'000'000;

std::vector<double> EnergyEnvelope(const FloatAudio& audio) {
  const size_t bins = static_cast<size_t>(audio.frames() * 1000 / audio.sample_rate) + 1;
  Envelope envelope(0, kBinNs, bins);
  envelope.Add(0, audio.sample_rate, audio.channels, audio.samples.data(), audio.frames());
  return envelope.Amplitude();
}

// Pearson correlation of ea[i] and eb[i + lag] over the bins both have.
double CorrelationAt(const std::vector<double>& ea, const std::vector<double>& eb, int64_t lag) {
  const int64_t begin = std::max<int64_t>(0, -lag);
  const int64_t end = std::min<int64_t>(static_cast<int64_t>(ea.size()), static_cast<int64_t>(eb.size()) - lag);
  if (end - begin < 2) return 0;
  double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
  for (int64_t i = begin; i < end; ++i) {
    const double x = ea[static_cast<size_t>(i)];
    const double y = eb[static_cast<size_t>(i + lag)];
    sa += x;
    sb += y;
    saa += x * x;
    sbb += y * y;
    sab += x * y;
  }
  const double n = static_cast<double>(end - begin);
  const double va = saa - sa * sa / n;
  const double vb = sbb - sb * sb / n;
  return va > 0 && vb > 0 ? (sab - sa * sb / n) / std::sqrt(va * vb) : 0;
}

// Frames of a, [begin, end), that b also has at `offset`.
struct Overlap {
  size_t begin = 0;
  size_t end = 0;
};

Overlap OverlapAt(const FloatAudio& a, const FloatAudio& b, int64_t offset) {
  const int64_t begin = std::max<int64_t>(0, -offset);
  const int64_t end = std::min<int64_t>(static_cast<int64_t>(a.frames()), static_cast<int64_t>(b.frames()) - offset);
  if (end <= begin) return {};
  return {static_cast<size_t>(begin), static_cast<size_t>(end)};
}

// Sum of squared differences over a's frames [begin, end) at `offset`.
double ResidualEnergy(const FloatAudio& a, const FloatAudio& b, int64_t offset, size_t begin, size_t end) {
  const size_t c = a.channels;
  const float* pa = a.samples.data() + begin * c;
  const float* pb = b.samples.data() + static_cast<size_t>(static_cast<int64_t>(begin) + offset) * c;
  double sum = 0;
  for (size_t i = 0; i < (end - begin) * c; ++i) {
    const double d = static_cast<double>(pa[i]) - pb[i];
    sum += d * d;
  }
  return sum;
}

// The loudest `length` frames of a within [begin, end).
size_t LoudestStart(const FloatAudio& a, size_t begin, size_t end, size_t length) {
  std::vector<double> energy(end - begin + 1);
  for (size_t i = begin; i < end; ++i) {
    double e = 0;
    for (size_t c = 0; c < a.channels; ++c) e += static_cast<double>(a.samples[i * a.channels + c]) * a.samples[i * a.channels + c];
    energy[i - begin + 1] = energy[i - begin] + e;
  }
  size_t best = begin;
  double loudest = -1;
  for (size_t s = begin; s + length <= end; ++s) {
    const double e = energy[s + length - begin] - energy[s - begin];
    if (e > loudest) {
      loudest = e;
      best = s;
    }
  }
  return best;
}

}  // namespace

Result<Alignment> AlignAudio(const FloatAudio& a, const FloatAudio& b, int64_t min_offset, int64_t max_offset) {
  if (a.sample_rate == 0 || a.sample_rate != b.sample_rate || a.channels == 0 || a.channels != b.channels) {
    return Error{std::format("Can't line up {} Hz, {} channel audio with {} Hz, {} channel audio.", a.sample_rate,
                             a.channels, b.sample_rate, b.channels)};
  }
  const uint32_t rate = a.sample_rate;
  const std::vector<double> ea = EnergyEnvelope(a);
  const std::vector<double> eb = EnergyEnvelope(b);

  // Coarse: every 1 ms bin lag in range.
  const auto to_bins = [&](int64_t frames) { return static_cast<int64_t>(std::floor(frames * 1000.0 / rate)); };
  double best_correlation = 0;
  int64_t best_lag = 0;
  for (int64_t lag = to_bins(min_offset); lag <= to_bins(max_offset) + 1; ++lag) {
    const double correlation = CorrelationAt(ea, eb, lag);
    if (correlation > best_correlation) {
      best_correlation = correlation;
      best_lag = lag;
    }
  }
  if (best_correlation <= 0) {
    return Error{"The two don't line up: they don't overlap at any offset tried, or have nothing in common."};
  }
  const int64_t coarse = std::clamp<int64_t>(std::llround(best_lag * rate / 1000.0), min_offset, max_offset);

  // To the frame: the least residual within 2 ms either way, scored on the
  // loudest second that every offset tried can see.
  const int64_t search = rate / 500;
  const Overlap overlap = OverlapAt(a, b, coarse);
  const size_t begin = overlap.begin + static_cast<size_t>(search);
  const size_t end = overlap.end > static_cast<size_t>(search) ? overlap.end - static_cast<size_t>(search) : 0;
  if (end <= begin) return Error{"The two barely overlap, so they can't be lined up to the frame."};
  const size_t length = std::min<size_t>(rate, end - begin);
  const size_t start = LoudestStart(a, begin, end, length);
  Alignment alignment{coarse, best_correlation};
  double least = std::numeric_limits<double>::infinity();
  for (int64_t offset = coarse - search; offset <= coarse + search; ++offset) {
    const double residual = ResidualEnergy(a, b, offset, start, start + length);
    if (residual < least) {
      least = residual;
      alignment.offset = offset;
    }
  }
  return alignment;
}

AudioDiff DiffAudio(const FloatAudio& a, const FloatAudio& b, int64_t offset) {
  AudioDiff diff;
  const Overlap overlap = OverlapAt(a, b, offset);
  if (a.channels != b.channels || overlap.begin == overlap.end) return diff;
  const size_t c = a.channels;
  const size_t window = std::max<size_t>(1, a.sample_rate / 10);
  double signal = 0, residual = 0, ab = 0, bb = 0, window_sum = 0;
  size_t window_frames = 0;
  for (size_t i = overlap.begin; i < overlap.end; ++i) {
    const size_t j = static_cast<size_t>(static_cast<int64_t>(i) + offset);
    for (size_t ch = 0; ch < c; ++ch) {
      const float fx = a.samples[i * c + ch];
      const float fy = b.samples[j * c + ch];
      const double x = fx;
      const double y = fy;
      const double d = x - y;
      signal += x * x;
      residual += d * d;
      window_sum += d * d;
      ab += x * y;
      bb += y * y;
      // Once NaN, the peak stays NaN.
      if (std::isnan(d) || std::fabs(d) > diff.residual_peak) diff.residual_peak = std::fabs(d);
      if (std::bit_cast<uint32_t>(fx) == std::bit_cast<uint32_t>(fy)) ++diff.identical_samples;
    }
    if (++window_frames == window || i + 1 == overlap.end) {
      const double rms = std::sqrt(window_sum / static_cast<double>(window_frames * c));
      if (rms > diff.worst_window_rms) {
        diff.worst_window_rms = rms;
        diff.worst_window_frame = i + 1 - window_frames;
      }
      window_sum = 0;
      window_frames = 0;
    }
  }
  diff.frames = overlap.end - overlap.begin;
  diff.samples = diff.frames * c;
  diff.signal_rms = std::sqrt(signal / static_cast<double>(diff.samples));
  diff.residual_rms = std::sqrt(residual / static_cast<double>(diff.samples));
  diff.gain = bb > 0 ? ab / bb : 1;
  return diff;
}

FloatAudio Residual(const FloatAudio& a, const FloatAudio& b, int64_t offset) {
  FloatAudio out{a.sample_rate, a.channels, {}};
  const Overlap overlap = OverlapAt(a, b, offset);
  if (a.channels != b.channels) return out;
  const size_t c = a.channels;
  out.samples.reserve((overlap.end - overlap.begin) * c);
  for (size_t i = overlap.begin; i < overlap.end; ++i) {
    const size_t j = static_cast<size_t>(static_cast<int64_t>(i) + offset);
    for (size_t ch = 0; ch < c; ++ch) out.samples.push_back(a.samples[i * c + ch] - b.samples[j * c + ch]);
  }
  return out;
}

MixChanges MixLikeObs(FloatAudio& audio) {
  MixChanges changes;
  for (float& x : audio.samples) {
    float y = 0.0f;
    y += x;
    if (std::bit_cast<uint32_t>(y) != std::bit_cast<uint32_t>(x) && x == 0.0f) ++changes.negative_zeros;
    // In audio-io.c's order.
    const float mixed = y;
    y = y == y ? y : 0.0f;
    y = y > 1.0f ? 1.0f : y;
    y = y < -1.0f ? -1.0f : y;
    if (std::bit_cast<uint32_t>(y) != std::bit_cast<uint32_t>(mixed)) ++changes.clamped;
    x = y;
  }
  return changes;
}

double ToDb(double amplitude) {
  return amplitude > 0 ? 20 * std::log10(amplitude) : -std::numeric_limits<double>::infinity();
}

}  // namespace knobs::tools
