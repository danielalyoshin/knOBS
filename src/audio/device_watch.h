// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "audio/audio_devices.h"
#include "util/result.h"

// Audio endpoints straight from Windows (MMDevice API), with no libobs
// involved, so knobs can tell what's connected before libobs runs.
namespace knobs::audio {

// The endpoints that are active (enabled and connected). Opens none of them.
struct Endpoints {
  std::vector<AudioDevice> mics;     // Recording.
  std::vector<AudioDevice> outputs;  // Playback.
  // The default communications recording device, which win-wasapi records from
  // for a mic set to "default" (docs/design.md, M1 findings). Empty if there's
  // none.
  std::string default_mic;

  friend bool operator==(const Endpoints&, const Endpoints&) = default;
};

// Call from a thread with COM initialized. Empty lists if Windows' device
// enumerator isn't available.
Endpoints ListEndpoints();

// Hears from Windows whenever an audio endpoint is added, removed, enabled,
// disabled, plugged in or unplugged, or a default device changes
// (IMMNotificationClient). Property changes, which some drivers send all the
// time, are left out.
class DeviceWatch {
 public:
  // `on_change` runs on a thread of Windows' audio service, possibly several
  // times per change; it mustn't block. Start and destroy the watch on a
  // thread with COM initialized.
  static Result<std::unique_ptr<DeviceWatch>> Start(std::function<void()> on_change);
  // Once this returns, `on_change` won't run again.
  ~DeviceWatch();
  DeviceWatch(const DeviceWatch&) = delete;
  DeviceWatch& operator=(const DeviceWatch&) = delete;

 private:
  struct State;
  explicit DeviceWatch(std::unique_ptr<State> state);

  std::unique_ptr<State> state_;
};

}  // namespace knobs::audio
