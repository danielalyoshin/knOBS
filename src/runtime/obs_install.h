// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "runtime/obs_version.h"
#include "util/result.h"

namespace knobs::runtime {

// A user's OBS Studio install. knOBS only ever reads from it: to find the
// version and to make the runtime copy. It never loads code from here.
struct ObsInstall {
  std::filesystem::path root;  // Holds bin\, data\ and obs-plugins\.
  ObsVersion version;          // From bin\64bit\obs.dll's version resource.
};

// Checks that `folder` is an OBS install with everything knOBS needs, and
// reads its version. Also accepts the install's bin\64bit folder, which is
// what people tend to pick when browsing for it.
Result<ObsInstall> InspectObsInstall(const std::filesystem::path& folder);

// Existing folders where the installer puts OBS: the ones recorded in the
// registry, then the default Program Files folder. Deduplicated; not
// validated. Empty means OBS doesn't appear to be installed at all.
std::vector<std::filesystem::path> ObsInstallCandidates();

// The first candidate that passes InspectObsInstall(). Steam and portable
// installs need PickObsInstallFolder().
Result<ObsInstall> FindObsInstall();

// Asks the user for the OBS folder. `owner` is an HWND or null. Returns
// nullopt if they cancel. Call from a thread that can host a COM STA.
std::optional<std::filesystem::path> PickObsInstallFolder(void* owner);

}  // namespace knobs::runtime
