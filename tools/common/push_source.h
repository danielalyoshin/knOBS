// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

#include "runtime/obs_api.h"

namespace knobs::tools {

// An input source type that outputs whatever audio the program pushes into
// it, standing in for a capture device. Load a chain as this type (see
// audio::LoadSourceJson) to run a WAV or a test signal through the chain's
// filters and source-level state instead of a mic.
inline constexpr char kPushSourceId[] = "knobs_push_source";

// Registers the type with libobs. Once per session.
void RegisterPushSource(const runtime::ObsApi& api);

// Outputs `frames` planar float frames, one plane per channel (1 or 2), the
// way a capture device's source does (obs_source_output_audio). libobs runs
// the source's filters and audio capture callbacks, the monitor included, on
// the calling thread before this returns.
void PushAudio(const runtime::ObsApi& api, obs_source_t* source, const float* const* planes,
               uint32_t channels, uint32_t frames, uint32_t sample_rate, uint64_t timestamp_ns);

}  // namespace knobs::tools
