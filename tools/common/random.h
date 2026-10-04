// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace knobs::tools {

// xorshift32: the same sequence on every run and build, for test signals.
class XorShift32 {
 public:
  explicit XorShift32(uint32_t seed) : state_(seed) {}

  float Uniform() {  // [-1, 1)
    state_ ^= state_ << 13;
    state_ ^= state_ >> 17;
    state_ ^= state_ << 5;
    return static_cast<float>(static_cast<int32_t>(state_)) / 2147483648.0f;
  }
  double Between(double lo, double hi) { return lo + (hi - lo) * (Uniform() * 0.5 + 0.5); }

 private:
  uint32_t state_;
};

}  // namespace knobs::tools
