// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "runtime/obs_api.h"
#include "util/result.h"

namespace knobs::audio {

// An audio endpoint as libobs lists it. `id` is the Windows endpoint ID, or
// "default".
struct AudioDevice {
  std::string name;
  std::string id;

  friend bool operator==(const AudioDevice&, const AudioDevice&) = default;
};

// win-wasapi's and OBS's ID for the default device.
inline constexpr std::string_view kDefaultDevice = "default";

// Whether two endpoint IDs name the same device. They may differ in case
// between OBS's settings and Windows.
bool SameId(std::string_view a, std::string_view b);

// The device in `devices` whose ID is the same as `id` (SameId), or null.
const AudioDevice* FindById(const std::vector<AudioDevice>& devices, std::string_view id);

// Recording devices from win-wasapi's own list, as OBS offers them for a mic
// source: "Default" (the default communications device) first when there
// are any. Opens none of them.
std::vector<AudioDevice> ListMicDevices(const runtime::ObsApi& api);

// Playback devices libobs can monitor to, "Default" first. Opens none of
// them.
std::vector<AudioDevice> ListMonitoringDevices(const runtime::ObsApi& api);

// The device whose ID is the same as `query` (SameId), else the one whose
// name contains it, ignoring ASCII case. When several names contain it, one
// that equals it wins; otherwise that's an error. `kind` names the list in
// messages.
Result<AudioDevice> FindDevice(const std::vector<AudioDevice>& devices, std::string_view query,
                               std::string_view kind);

// Sends monitored audio to `device`. Sources already monitoring move to it.
Status SetMonitoringDevice(const runtime::ObsApi& api, const AudioDevice& device);

}  // namespace knobs::audio
