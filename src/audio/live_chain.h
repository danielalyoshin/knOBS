// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/obs_api.h"
#include "runtime/obs_session.h"
#include "util/result.h"

namespace knobs::audio {

// A stand-in for an imported chain: win-wasapi's input source for
// `device_id` with one gain filter, as an OBS source object (the JSON a scene
// collection saves for each source).
std::string MicWithGainSourceJson(std::string_view device_id, double gain_db);

struct LoadOptions {
  // If not empty, replaces the source's type and keeps everything else,
  // filters included.
  std::string_view type_id;
  // Runs obs_source_load2 after loading, which calls the source's and its
  // filters' `load` callbacks. obs_load_sources does that for a scene
  // collection's "sources", but OBS's frontend doesn't for global audio
  // devices (Mic/Aux), so this follows where the mic came from
  // (import::ImportedMic::load_callbacks).
  bool load_callbacks = false;
};

// Loads an OBS source object with libobs's own loader
// (obs_load_private_source), which recreates the source, its filters in order
// and its source-level state (volume, balance, Mono, sync offset). Monitoring
// is off afterwards, whatever the JSON says. Returns a new reference.
Result<obs_source_t*> LoadSourceJson(const runtime::ObsApi& api, std::string_view json,
                                     const LoadOptions& options = {});

// What a loaded source will do to its audio, as libobs reports it.
struct ChainInfo {
  struct Filter {
    std::string type;  // Versioned, e.g. "noise_suppress_filter_v2".
    std::string name;
    bool enabled = true;
    // Whether libobs has a filter of this type. It still loads one it doesn't
    // know, as a placeholder that passes audio through untouched.
    bool known = true;
    // Whether it's an audio filter. Others, such as video filters, pass audio
    // through too.
    bool audio = true;
  };
  std::string type;
  // Applied before the filters (obs-source.c, process_audio).
  float balance = 0.5f;
  bool mono = false;
  // In processing order.
  std::vector<Filter> filters;
  // Applied after the filters: by the monitor, and in libobs's output mix.
  float volume = 1.0f;
};
ChainInfo DescribeChain(const runtime::ObsApi& api, obs_source_t* source);

// knobs's whole audio path: a loaded source monitored to libobs's monitoring
// device, and active. libobs does all the processing. MONITOR_ONLY keeps the
// audio out of libobs's output mix, so the audio thread never touches it.
// The monitor only plays while the source is active (it checks activate_refs
// in wasapi-output.c); nothing else needs activation, which without video
// never runs the source's activate callback.
class LiveChain {
 public:
  // Set the monitoring device first (SetMonitoringDevice): the monitor opens
  // it as soon as monitoring starts.
  static Result<std::unique_ptr<LiveChain>> Start(const runtime::ObsApi& api,
                                                  runtime::ObsSession& session,
                                                  std::string_view source_json,
                                                  const LoadOptions& options = {});
  // Stops monitoring, releases the source and waits until it's destroyed.
  ~LiveChain();
  LiveChain(const LiveChain&) = delete;
  LiveChain& operator=(const LiveChain&) = delete;

  obs_source_t* source() const { return source_; }

  // Opens the monitor's stream afresh. libobs opens a new one on the
  // monitoring device, then stops the old one, dropping what it had queued,
  // and packets that come in between are lost (audio_monitor_reset in
  // wasapi-output.c). It's how OBS moves monitors to another device. It
  // restarts every monitor in the process; knobs has one.
  void RestartMonitor();

 private:
  LiveChain(const runtime::ObsApi& api, runtime::ObsSession& session, obs_source_t* source)
      : api_(api), session_(session), source_(source) {}

  const runtime::ObsApi& api_;
  runtime::ObsSession& session_;
  obs_source_t* source_;
};

}  // namespace knobs::audio
