// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <compare>
#include <cstdint>
#include <string>

namespace knobs::runtime {

struct ObsVersion {
  uint32_t major = 0;
  uint32_t minor = 0;
  uint32_t patch = 0;

  friend auto operator<=>(const ObsVersion&, const ObsVersion&) = default;

  // "32.2.2"
  std::string ToString() const;

  // Unpacks obs_get_version(): (major << 24) | (minor << 16) | patch.
  static ObsVersion FromLibobs(uint32_t packed);
};

// The oldest supported version is the minor release the vendored headers come
// from (third_party/libobs), so knOBS never calls into a libobs older than the
// declarations it was compiled against. The cap is the next major: libobs
// bumps the major for breaking API changes (obs-config.h). Only 32.2.2 has
// actually been tested so far.
inline constexpr ObsVersion kMinSupportedObs{32, 2, 0};
inline constexpr ObsVersion kFirstUnsupportedObs{33, 0, 0};

bool IsSupportedObsVersion(const ObsVersion& version);

// For messages, e.g. "OBS Studio 32.2 or a later 32.x release".
std::string DescribeSupportedObsVersions();

}  // namespace knobs::runtime
