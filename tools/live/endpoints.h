// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/envelope.h"
#include "util/result.h"

namespace knobs::tools {

enum class EndpointFlow { kRecording, kPlayback };

// Executable names of other processes with an active audio session on an
// endpoint: recording from it, or playing to it. "default" means what libobs
// means by it (the default communications recording device, or the default
// playback device). Call from a thread with COM initialized.
Result<std::vector<std::string>> OtherActiveSessions(const std::string& device_id, EndpointFlow flow);

// Records a recording endpoint the way any app reading it would, with a
// WASAPI shared-mode stream on a thread of its own. Keeps only the energy
// envelope and the peak level: the audio itself is dropped as it arrives.
// Times come from WASAPI's per-packet timestamps, the moment the endpoint
// recorded each packet's first frame.
class EndpointRecorder {
 public:
  // `device_id` is an endpoint ID or "default" (as in OtherActiveSessions).
  static Result<std::unique_ptr<EndpointRecorder>> Start(const std::string& device_id, Envelope envelope);
  ~EndpointRecorder();
  EndpointRecorder(const EndpointRecorder&) = delete;
  EndpointRecorder& operator=(const EndpointRecorder&) = delete;

  // Stops recording. Returns an error if the stream failed while recording.
  Status Stop();
  // The peak absolute sample since the last call.
  float TakePeak();
  // Complete once Stop() has returned.
  const Envelope& envelope() const { return envelope_; }

 private:
  explicit EndpointRecorder(Envelope envelope) : envelope_(std::move(envelope)) {}
  // The recording thread: opens the stream, reports that through `opened`,
  // then records until stop_event_.
  void Run(const std::string& device_id, std::promise<Status>* opened);

  Envelope envelope_;  // Only the recording thread touches it until Stop().
  std::thread thread_;
  void* stop_event_ = nullptr;  // HANDLE
  std::mutex mutex_;
  float peak_ = 0;
  std::string error_;
};

// Plays into a playback endpoint through a WASAPI stream set up the way
// libobs's monitor sets up its own (wasapi-output.c): shared mode, the
// device's mix format, a 1 s buffer and no event callback, with audio written
// the moment it's ready. The monitor's output path without libobs. Use on a
// thread with COM initialized.
class EndpointPlayer {
 public:
  static Result<std::unique_ptr<EndpointPlayer>> Open(const std::string& device_id);
  ~EndpointPlayer();
  EndpointPlayer(const EndpointPlayer&) = delete;
  EndpointPlayer& operator=(const EndpointPlayer&) = delete;

  // The device's mix format, which Write() takes.
  uint32_t sample_rate() const;
  uint32_t channels() const;

  // Queues interleaved frames.
  Status Write(const float* interleaved, uint32_t frames);

 private:
  struct Stream;
  EndpointPlayer();

  std::unique_ptr<Stream> stream_;
};

}  // namespace knobs::tools
