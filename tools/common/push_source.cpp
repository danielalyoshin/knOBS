// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/push_source.h"

#include "app_info.h"

namespace knobs::tools {

void RegisterPushSource(const runtime::ObsApi& api) {
  obs_source_info info = {};
  info.id = kPushSourceId;
  info.type = OBS_SOURCE_TYPE_INPUT;
  info.output_flags = OBS_SOURCE_AUDIO;
  info.get_name = [](void*) { return KNOBS_DISPLAY_NAME " push source"; };
  // libobs logs an error for a null context, so hand back the source itself.
  info.create = [](obs_data_t*, obs_source_t* source) -> void* { return source; };
  info.destroy = [](void*) {};
  // libobs copies the struct. The strings are literals, so they outlive it.
  api.obs_register_source_s(&info, sizeof(info));
}

void PushAudio(const runtime::ObsApi& api, obs_source_t* source, const float* const* planes,
               uint32_t channels, uint32_t frames, uint32_t sample_rate, uint64_t timestamp_ns) {
  obs_source_audio audio = {};
  for (uint32_t c = 0; c < channels; ++c) audio.data[c] = reinterpret_cast<const uint8_t*>(planes[c]);
  audio.frames = frames;
  audio.speakers = channels == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;
  audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
  audio.samples_per_sec = sample_rate;
  audio.timestamp = timestamp_ns;
  api.obs_source_output_audio(source, &audio);
}

}  // namespace knobs::tools
