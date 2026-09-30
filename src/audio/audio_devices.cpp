// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio/audio_devices.h"

#include <format>

#include "util/win_strings.h"

namespace knobs::audio {
namespace {

// win-wasapi's input source, and the name of its device list property.
constexpr char kMicSourceId[] = "wasapi_input_capture";
constexpr char kDeviceIdProperty[] = "device_id";

}  // namespace

std::vector<AudioDevice> ListMicDevices(const runtime::ObsApi& api) {
  std::vector<AudioDevice> devices;
  obs_properties_t* properties = api.obs_get_source_properties(kMicSourceId);
  if (!properties) return devices;
  if (obs_property_t* list = api.obs_properties_get(properties, kDeviceIdProperty)) {
    const size_t count = api.obs_property_list_item_count(list);
    for (size_t i = 0; i < count; ++i) {
      const char* name = api.obs_property_list_item_name(list, i);
      const char* id = api.obs_property_list_item_string(list, i);
      if (id) devices.push_back({name ? name : id, id});
    }
  }
  api.obs_properties_destroy(properties);
  return devices;
}

std::vector<AudioDevice> ListMonitoringDevices(const runtime::ObsApi& api) {
  // libobs lists the endpoints; OBS's settings dialog adds "Default" itself.
  std::vector<AudioDevice> devices = {{"Default", "default"}};
  api.obs_enum_audio_monitoring_devices(
      [](void* param, const char* name, const char* id) {
        static_cast<std::vector<AudioDevice>*>(param)->push_back({name, id});
        return true;
      },
      &devices);
  return devices;
}

Result<AudioDevice> FindDevice(const std::vector<AudioDevice>& devices, std::string_view query,
                               std::string_view kind) {
  for (const AudioDevice& device : devices) {
    if (device.id == query) return device;
  }
  const std::string needle = AsciiLower(query);
  std::vector<const AudioDevice*> matches;
  for (const AudioDevice& device : devices) {
    const std::string name = AsciiLower(device.name);
    if (name == needle) return device;
    if (name.find(needle) != std::string::npos) matches.push_back(&device);
  }
  if (matches.size() == 1) return *matches.front();
  if (matches.empty()) return Error{std::format("No {} matches \"{}\".", kind, query)};
  std::string names;
  for (const AudioDevice* device : matches) {
    names += std::format("{}\"{}\"", names.empty() ? "" : ", ", device->name);
  }
  return Error{std::format("Several {}s match \"{}\": {}.", kind, query, names)};
}

Status SetMonitoringDevice(const runtime::ObsApi& api, const AudioDevice& device) {
  if (!api.obs_set_audio_monitoring_device(device.name.c_str(), device.id.c_str())) {
    return Error{std::format("libobs couldn't monitor to \"{}\".", device.name)};
  }
  return Ok{};
}

}  // namespace knobs::audio
