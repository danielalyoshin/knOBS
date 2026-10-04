// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace knobs::tools {

// Now on the QueryPerformanceCounter clock, in nanoseconds. libobs's
// os_gettime_ns() and WASAPI's capture timestamps use the same clock.
uint64_t NowNs();

// The largest absolute sample since the last Take(). An audio thread can add
// while another thread takes.
class PeakHold {
 public:
  void Add(const float* samples, size_t count);
  float Take();

 private:
  std::mutex mutex_;
  float peak_ = 0;
};

// A signal's power on a fixed time grid: bin i averages the frames that fell
// in [origin + i * bin, origin + (i + 1) * bin). Keeps no audio, so it can't
// be played back, only compared.
class Envelope {
 public:
  Envelope(uint64_t origin_ns, uint64_t bin_ns, size_t bins);

  // Adds interleaved frames. The first one was at `first_frame_ns`; the rest
  // follow at `sample_rate`. Frames outside the grid are ignored.
  void Add(uint64_t first_frame_ns, uint32_t sample_rate, uint32_t channels, const float* interleaved,
           size_t frames);

  // Root-mean-square amplitude per bin; 0 where nothing was added.
  std::vector<double> Amplitude() const;

  uint64_t origin_ns() const { return origin_ns_; }
  uint64_t bin_ns() const { return bin_ns_; }
  size_t size() const { return power_sum_.size(); }

 private:
  uint64_t origin_ns_;
  uint64_t bin_ns_;
  std::vector<double> power_sum_;
  std::vector<uint32_t> count_;
};

struct DelayEstimate {
  double delay_ms = 0;
  // Normalized cross-correlation at that delay: 1 is the same shape, around
  // 0 is no relation.
  double correlation = 0;
};

// How much later `delayed` repeats `reference`, searching delays from 0 to
// `max_delay_ns`. Cross-correlates the two amplitude envelopes over the
// reference bins [first_bin, end_bin), then refines the best bin with a
// parabola through its neighbours. Both envelopes must share an origin and a
// bin width. Delays whose window runs past the end of `delayed` are skipped.
DelayEstimate EstimateDelay(const Envelope& reference, const Envelope& delayed, uint64_t max_delay_ns,
                            size_t first_bin, size_t end_bin);

}  // namespace knobs::tools
