// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/obs_api.h"

namespace knobs::runtime {

std::vector<std::string_view> ResolveObsApi(ObsApi& api, const SymbolLookup& lookup) {
  std::vector<std::string_view> missing;
#define KNOBS_RESOLVE_OBS_FUNCTION(name)                            \
  api.name = reinterpret_cast<decltype(api.name)>(lookup(#name)); \
  if (!api.name) missing.push_back(#name);
  KNOBS_OBS_API(KNOBS_RESOLVE_OBS_FUNCTION)
#undef KNOBS_RESOLVE_OBS_FUNCTION
  return missing;
}

size_t ObsApiSize() {
#define KNOBS_COUNT_OBS_FUNCTION(name) +1
  return 0 KNOBS_OBS_API(KNOBS_COUNT_OBS_FUNCTION);
#undef KNOBS_COUNT_OBS_FUNCTION
}

}  // namespace knobs::runtime
