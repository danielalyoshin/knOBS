// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

#include "audio/live_chain.h"
#include "common/wav.h"
#include "runtime/obs_session.h"
#include "util/result.h"

namespace knobs::tools {

struct OfflineRun {
  audio::ChainInfo chain;  // As loaded.
  // What the filters output, interleaved, at the session's sample rate and
  // channel count. The source's volume isn't applied: libobs does that
  // afterwards, in the monitor and the mix.
  FloatAudio output;
};

// Runs `input` through a chain offline. The chain loads as a push source
// (common/push_source.h), which stands in for the mic: it outputs the input
// in `chunk`-frame packets with synthetic, gapless timestamps, then
// `tail_frames` of silence so buffered filters flush. libobs runs the chain
// on this thread before each push returns, and an audio capture callback,
// the hook libobs's monitor uses, collects the output. No audio device is
// opened. The push source must be registered, and `input` must be at the
// session's sample rate, mono or stereo.
Result<OfflineRun> RunChainOffline(const runtime::ObsApi& api, runtime::ObsSession& session,
                                   std::string_view source_json, bool load_callbacks, const FloatAudio& input,
                                   uint32_t chunk, uint32_t tail_frames);

}  // namespace knobs::tools
