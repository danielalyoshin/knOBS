// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/audio_diff.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

#include "common/envelope.h"

namespace knobs::tools {
namespace {

constexpr uint64_t kBinNs = 1'000'000;

Envelope MakeEnvelope(const FloatAudio& audio) {
  const size_t bins = static_cast<size_t>(audio.frames() * 1000 / audio.sample_rate) + 1;
  Envelope envelope(0, kBinNs, bins);
  envelope.Add(0, audio.sample_rate, audio.channels, audio.samples.data(), audio.frames());
  return envelope;
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

// Mean squared difference per sample at `offset`; infinity without overlap.
double MeanSquaredResidual(const FloatAudio& a, const FloatAudio& b, int64_t offset) {
  const Overlap overlap = OverlapAt(a, b, offset);
  if (overlap.begin == overlap.end) return std::numeric_limits<double>::infinity();
  const size_t c = a.channels;
  const float* pa = a.samples.data() + overlap.begin * c;
  const float* pb = b.samples.data() + static_cast<size_t>(static_cast<int64_t>(overlap.begin) + offset) * c;
  const size_t n = (overlap.end - overlap.begin) * c;
  double sum = 0;
  for (size_t i = 0; i < n; ++i) {
    const double d = static_cast<double>(pa[i]) - pb[i];
    sum += d * d;
  }
  return sum / static_cast<double>(n);
}

}  // namespace

Result<Alignment> AlignAudio(const FloatAudio& a, const FloatAudio& b, size_t max_offset) {
  if (a.sample_rate == 0 || a.sample_rate != b.sample_rate || a.channels == 0 || a.channels != b.channels) {
    return Error{std::format("Can't line up {} Hz, {} channel audio with {} Hz, {} channel audio.", a.sample_rate,
                             a.channels, b.sample_rate, b.channels)};
  }
  const uint32_t rate = a.sample_rate;
  const Envelope ea = MakeEnvelope(a);
  const Envelope eb = MakeEnvelope(b);
  const uint64_t max_delay_ns = uint64_t{max_offset} * 1'000'000'000 / rate;
  const size_t max_lag_bins = static_cast<size_t>(max_delay_ns / kBinNs) + 1;

  // EstimateDelay only looks for the second envelope lagging the first, so
  // try both ways round.
  const auto estimate = [&](const Envelope& reference, const Envelope& delayed) -> DelayEstimate {
    if (delayed.size() <= max_lag_bins) return {};
    const size_t end = std::min(reference.size(), delayed.size() - max_lag_bins);
    return EstimateDelay(reference, delayed, max_delay_ns, 0, end);
  };
  const DelayEstimate b_later = estimate(ea, eb);
  const DelayEstimate a_later = estimate(eb, ea);
  if (b_later.correlation <= 0 && a_later.correlation <= 0) {
    return Error{"The two don't line up: they're too short for the offsets searched, or too different."};
  }
  const bool b_lags = b_later.correlation >= a_later.correlation;
  const DelayEstimate& best = b_lags ? b_later : a_later;
  const int64_t coarse = static_cast<int64_t>(std::llround(best.delay_ms * rate / 1000.0)) * (b_lags ? 1 : -1);

  // To the frame: the least residual within 2 ms either way.
  const int64_t search = rate / 500;
  Alignment alignment{coarse, best.correlation};
  double least = std::numeric_limits<double>::infinity();
  for (int64_t offset = coarse - search; offset <= coarse + search; ++offset) {
    const double residual = MeanSquaredResidual(a, b, offset);
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
      const double x = a.samples[i * c + ch];
      const double y = b.samples[j * c + ch];
      const double d = x - y;
      signal += x * x;
      residual += d * d;
      window_sum += d * d;
      ab += x * y;
      bb += y * y;
      diff.residual_peak = std::max(diff.residual_peak, std::fabs(d));
      if (a.samples[i * c + ch] == b.samples[j * c + ch]) ++diff.identical_samples;
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

double ToDb(double amplitude) {
  return amplitude > 0 ? 20 * std::log10(amplitude) : -std::numeric_limits<double>::infinity();
}

}  // namespace knobs::tools
