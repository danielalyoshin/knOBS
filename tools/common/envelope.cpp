// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/envelope.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace knobs::tools {

uint64_t NowNs() {
  static const uint64_t frequency = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<uint64_t>(f.QuadPart);
  }();
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  const uint64_t ticks = static_cast<uint64_t>(counter.QuadPart);
  // As libobs's util_mul_div64, without overflowing.
  return ticks / frequency * 1'000'000'000 + ticks % frequency * 1'000'000'000 / frequency;
}

Envelope::Envelope(uint64_t origin_ns, uint64_t bin_ns, size_t bins)
    : origin_ns_(origin_ns), bin_ns_(bin_ns), power_sum_(bins), count_(bins) {}

void Envelope::Add(uint64_t first_frame_ns, uint32_t sample_rate, uint32_t channels,
                   const float* interleaved, size_t frames) {
  if (sample_rate == 0 || channels == 0) return;
  for (size_t i = 0; i < frames; ++i) {
    const uint64_t t = first_frame_ns + i * 1'000'000'000 / sample_rate;
    if (t < origin_ns_) continue;
    const uint64_t bin = (t - origin_ns_) / bin_ns_;
    if (bin >= power_sum_.size()) break;
    double power = 0;
    for (uint32_t c = 0; c < channels; ++c) {
      const double x = interleaved[i * channels + c];
      power += x * x;
    }
    power_sum_[bin] += power / channels;
    ++count_[bin];
  }
}

std::vector<double> Envelope::Amplitude() const {
  std::vector<double> amplitude(power_sum_.size());
  for (size_t i = 0; i < amplitude.size(); ++i) {
    if (count_[i]) amplitude[i] = std::sqrt(power_sum_[i] / count_[i]);
  }
  return amplitude;
}

DelayEstimate EstimateDelay(const Envelope& reference, const Envelope& delayed, uint64_t max_delay_ns,
                            size_t first_bin, size_t end_bin) {
  const std::vector<double> r = reference.Amplitude();
  const std::vector<double> d = delayed.Amplitude();
  end_bin = std::min(end_bin, r.size());
  if (first_bin >= end_bin || reference.bin_ns() != delayed.bin_ns() ||
      reference.origin_ns() != delayed.origin_ns()) {
    return {};
  }
  const size_t n = end_bin - first_bin;

  // The reference window, centered.
  double r_mean = 0;
  for (size_t i = first_bin; i < end_bin; ++i) r_mean += r[i];
  r_mean /= static_cast<double>(n);
  std::vector<double> rc(n);
  double r_energy = 0;
  for (size_t i = 0; i < n; ++i) {
    rc[i] = r[first_bin + i] - r_mean;
    r_energy += rc[i] * rc[i];
  }

  // Prefix sums give each delayed window's mean and energy in O(1).
  std::vector<double> sum(d.size() + 1), sum_sq(d.size() + 1);
  for (size_t i = 0; i < d.size(); ++i) {
    sum[i + 1] = sum[i] + d[i];
    sum_sq[i + 1] = sum_sq[i] + d[i] * d[i];
  }

  const size_t max_lag = static_cast<size_t>(max_delay_ns / reference.bin_ns());
  std::vector<double> correlation;
  for (size_t lag = 0; lag <= max_lag && end_bin + lag <= d.size(); ++lag) {
    const size_t begin = first_bin + lag;
    const double mean = (sum[begin + n] - sum[begin]) / static_cast<double>(n);
    const double energy = (sum_sq[begin + n] - sum_sq[begin]) - static_cast<double>(n) * mean * mean;
    double dot = 0;  // rc sums to 0, so the delayed window needn't be centered.
    for (size_t i = 0; i < n; ++i) dot += rc[i] * d[begin + i];
    const double norm = std::sqrt(r_energy * energy);
    correlation.push_back(norm > 0 ? dot / norm : 0);
  }
  if (correlation.empty()) return {};

  const size_t best = static_cast<size_t>(
      std::max_element(correlation.begin(), correlation.end()) - correlation.begin());
  double offset = 0;
  if (best > 0 && best + 1 < correlation.size()) {
    const double a = correlation[best - 1], b = correlation[best], c = correlation[best + 1];
    const double curvature = a - 2 * b + c;
    if (curvature < 0) offset = 0.5 * (a - c) / curvature;
  }
  const double bin_ms = static_cast<double>(reference.bin_ns()) / 1e6;
  return {(static_cast<double>(best) + offset) * bin_ms, correlation[best]};
}

}  // namespace knobs::tools
