// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/offline_chain.h"

#include <format>
#include <vector>

#include "common/push_source.h"

namespace knobs::tools {
namespace {

// Where the synthetic timestamps start. Only their spacing matters.
constexpr uint64_t kFirstTimestampNs = 1'000'000'000;

struct Collector {
  uint32_t channels = 0;
  std::vector<float> samples;  // Interleaved.
};

void OnAudio(void* param, obs_source_t*, const audio_data* audio, bool) {
  // Called on the pushing thread, from within obs_source_output_audio.
  auto* out = static_cast<Collector*>(param);
  const size_t base = out->samples.size();
  out->samples.resize(base + size_t{audio->frames} * out->channels);
  for (uint32_t c = 0; c < out->channels; ++c) {
    const auto* plane = reinterpret_cast<const float*>(audio->data[c]);
    if (!plane) continue;
    for (uint32_t i = 0; i < audio->frames; ++i) out->samples[base + size_t{i} * out->channels + c] = plane[i];
  }
}

}  // namespace

Result<OfflineRun> RunChainOffline(const runtime::ObsApi& api, runtime::ObsSession& session,
                                   std::string_view source_json, bool load_callbacks, const FloatAudio& input,
                                   uint32_t chunk, uint32_t tail_frames) {
  const uint32_t rate = session.options().samples_per_sec;
  if (input.sample_rate != rate || input.channels == 0 || input.channels > 2 || chunk == 0) {
    return Error{std::format("The input is {} Hz with {} channels; the chain takes {} Hz, mono or stereo.",
                             input.sample_rate, input.channels, rate)};
  }
  auto source = audio::LoadSourceJson(api, source_json, {.type_id = kPushSourceId, .load_callbacks = load_callbacks});
  if (!source) return Error{source.error()};

  OfflineRun run;
  run.chain = audio::DescribeChain(api, *source);
  Collector collector;
  collector.channels = get_audio_channels(session.options().speakers);
  api.obs_source_add_audio_capture_callback(*source, OnAudio, &collector);

  const size_t in_frames = input.frames();
  const size_t total = (in_frames + tail_frames + chunk - 1) / chunk * chunk;
  std::vector<std::vector<float>> planes(input.channels, std::vector<float>(chunk));
  std::vector<const float*> pointers;
  for (const auto& plane : planes) pointers.push_back(plane.data());
  for (size_t start = 0; start < total; start += chunk) {
    for (uint32_t c = 0; c < input.channels; ++c) {
      for (size_t i = 0; i < chunk; ++i) {
        const size_t frame = start + i;
        planes[c][i] = frame < in_frames ? input.samples[frame * input.channels + c] : 0.0f;
      }
    }
    const uint64_t timestamp = kFirstTimestampNs + start * 1'000'000'000 / rate;
    PushAudio(api, *source, pointers.data(), input.channels, chunk, rate, timestamp);
  }

  api.obs_source_remove_audio_capture_callback(*source, OnAudio, &collector);
  api.obs_source_release(*source);
  session.DrainDestroyQueue();
  run.output = {rate, collector.channels, std::move(collector.samples)};
  return run;
}

}  // namespace knobs::tools
