// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/obs_version.h"

#include <format>

#include "app_info.h"

namespace knobs::runtime {

std::string ObsVersion::ToString() const { return std::format("{}.{}.{}", major, minor, patch); }

ObsVersion ObsVersion::FromLibobs(uint32_t packed) {
  return {packed >> 24, (packed >> 16) & 0xFF, packed & 0xFFFF};
}

bool IsSupportedObsVersion(const ObsVersion& version) {
  return version >= kMinSupportedObs && version < kFirstUnsupportedObs;
}

std::string DescribeSupportedObsVersions() {
  static_assert(kFirstUnsupportedObs.minor == 0 && kFirstUnsupportedObs.patch == 0 &&
                    kFirstUnsupportedObs.major == kMinSupportedObs.major + 1,
                "Update the wording below to match the new range");
  return std::format("OBS Studio {}.{} or a later {}.x release", kMinSupportedObs.major,
                     kMinSupportedObs.minor, kMinSupportedObs.major);
}

std::string UnsupportedObsMessage(std::string_view version_text) {
  return std::format("OBS {} isn't supported. {} supports {}.", version_text, kDisplayName,
                     DescribeSupportedObsVersions());
}

}  // namespace knobs::runtime
