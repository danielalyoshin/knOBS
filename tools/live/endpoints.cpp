// SPDX-License-Identifier: GPL-2.0-or-later
#include "live/endpoints.h"

#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <utility>

#include "util/win_strings.h"

namespace knobs::tools {
namespace {

using Microsoft::WRL::ComPtr;

// The stream's buffer. Packets arrive every engine period (10 ms), so this
// only has to absorb scheduling hiccups.
constexpr REFERENCE_TIME kBufferDuration = 200 * 10'000;  // 100 ns units

std::string HrText(HRESULT hr) { return std::format("0x{:08X}", static_cast<uint32_t>(hr)); }

Result<ComPtr<IMMDevice>> GetEndpoint(const std::string& device_id, EndpointFlow flow) {
  ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  if (FAILED(hr)) return Error{std::format("Couldn't list audio devices ({}).", HrText(hr))};
  ComPtr<IMMDevice> device;
  if (device_id == "default") {
    // As libobs: win-wasapi records from the communications device, and the
    // monitor plays to the console device.
    hr = flow == EndpointFlow::kRecording ? enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &device)
                                          : enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  } else {
    hr = enumerator->GetDevice(FromUtf8(device_id).c_str(), &device);
  }
  if (FAILED(hr)) return Error{std::format("Couldn't find the audio device {} ({}).", device_id, HrText(hr))};
  return device;
}

std::string ProcessName(DWORD pid) {
  std::string name = std::format("process {}", pid);
  if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
    wchar_t path[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(std::size(path));
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
      name = ToUtf8(std::filesystem::path(std::wstring_view(path, size)).filename());
    }
    CloseHandle(process);
  }
  return name;
}

bool IsFloat32(const WAVEFORMATEX& format) {
  const bool is_float =
      format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
      (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
       reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format).SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
  return is_float && format.wBitsPerSample == 32;
}

using MixFormat = std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)>;

// An audio client on the endpoint, and its shared-mode format.
Status ActivateClient(const std::string& device_id, EndpointFlow flow, ComPtr<IAudioClient>& client,
                      MixFormat& format) {
  auto device = GetEndpoint(device_id, flow);
  if (!device) return Error{device.error()};
  HRESULT hr = (*device)->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
  if (FAILED(hr)) return Error{std::format("Couldn't open the audio device {} ({}).", device_id, HrText(hr))};
  WAVEFORMATEX* mix = nullptr;
  hr = client->GetMixFormat(&mix);
  if (FAILED(hr)) return Error{std::format("Couldn't read the format of {} ({}).", device_id, HrText(hr))};
  format.reset(mix);
  if (!IsFloat32(*mix)) return Error{std::format("The shared-mode format of {} isn't 32-bit float.", device_id)};
  return Ok{};
}

struct CaptureStream {
  ComPtr<IAudioClient> client;
  ComPtr<IAudioCaptureClient> capture;
  uint32_t sample_rate = 0;
  uint32_t channels = 0;
};

Status OpenCapture(const std::string& device_id, HANDLE packet_event, CaptureStream& stream) {
  MixFormat format(nullptr, &CoTaskMemFree);
  const Status activated = ActivateClient(device_id, EndpointFlow::kRecording, stream.client, format);
  if (!activated) return activated;
  stream.sample_rate = format->nSamplesPerSec;
  stream.channels = format->nChannels;
  HRESULT hr = stream.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                         kBufferDuration, 0, format.get(), nullptr);
  if (SUCCEEDED(hr)) hr = stream.client->SetEventHandle(packet_event);
  if (SUCCEEDED(hr)) hr = stream.client->GetService(IID_PPV_ARGS(&stream.capture));
  if (SUCCEEDED(hr)) hr = stream.client->Start();
  if (FAILED(hr)) return Error{std::format("Couldn't start recording from {} ({}).", device_id, HrText(hr))};
  return Ok{};
}

}  // namespace

Result<std::vector<std::string>> OtherActiveSessions(const std::string& device_id, EndpointFlow flow) {
  auto device = GetEndpoint(device_id, flow);
  if (!device) return Error{device.error()};
  ComPtr<IAudioSessionManager2> manager;
  ComPtr<IAudioSessionEnumerator> sessions;
  int count = 0;
  HRESULT hr = (*device)->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager);
  if (SUCCEEDED(hr)) hr = manager->GetSessionEnumerator(&sessions);
  if (SUCCEEDED(hr)) hr = sessions->GetCount(&count);
  if (FAILED(hr)) return Error{std::format("Couldn't list the audio sessions on {} ({}).", device_id, HrText(hr))};

  std::vector<std::string> names;
  for (int i = 0; i < count; ++i) {
    ComPtr<IAudioSessionControl> control;
    ComPtr<IAudioSessionControl2> control2;
    AudioSessionState state = AudioSessionStateInactive;
    DWORD pid = 0;
    if (FAILED(sessions->GetSession(i, &control)) || FAILED(control.As(&control2)) ||
        FAILED(control2->GetState(&state)) || state != AudioSessionStateActive ||
        control2->IsSystemSoundsSession() == S_OK) {
      continue;
    }
    control2->GetProcessId(&pid);
    if (pid == GetCurrentProcessId()) continue;
    const std::string name = ProcessName(pid);
    if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
  }
  return names;
}

Result<std::unique_ptr<EndpointRecorder>> EndpointRecorder::Start(const std::string& device_id,
                                                                  Envelope envelope) {
  std::unique_ptr<EndpointRecorder> recorder(new EndpointRecorder(std::move(envelope)));
  recorder->stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::promise<Status> opened;
  std::future<Status> result = opened.get_future();
  recorder->thread_ = std::thread(&EndpointRecorder::Run, recorder.get(), device_id, &opened);
  Status status = result.get();
  if (!status) return Error{status.error()};  // The thread is done; the destructor joins it.
  return recorder;
}

EndpointRecorder::~EndpointRecorder() {
  Stop();
  CloseHandle(stop_event_);
}

Status EndpointRecorder::Stop() {
  if (thread_.joinable()) {
    SetEvent(stop_event_);
    thread_.join();
  }
  std::lock_guard lock(mutex_);
  if (!error_.empty()) return Error{error_};
  return Ok{};
}

void EndpointRecorder::Run(const std::string& device_id, std::promise<Status>* opened) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const HANDLE packet_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  {
    CaptureStream stream;
    Status status = OpenCapture(device_id, packet_event, stream);
    const bool ok = status.ok();
    opened->set_value(std::move(status));  // `opened` is gone after this.
    if (ok) {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
      const HANDLE events[] = {stop_event_, packet_event};
      std::vector<float> silence;
      uint64_t next_ns = 0;
      HRESULT hr = S_OK;
      while (SUCCEEDED(hr) && WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        UINT32 packet = 0;
        while (SUCCEEDED(hr = stream.capture->GetNextPacketSize(&packet)) && packet > 0) {
          BYTE* data = nullptr;
          UINT32 frames = 0;
          DWORD flags = 0;
          UINT64 qpc_position = 0;  // 100 ns units
          hr = stream.capture->GetBuffer(&data, &frames, &flags, nullptr, &qpc_position);
          if (FAILED(hr)) break;
          const uint64_t first_ns =
              (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && next_ns ? next_ns : qpc_position * 100;
          next_ns = first_ns + uint64_t{frames} * 1'000'000'000 / stream.sample_rate;
          const float* samples = reinterpret_cast<const float*>(data);
          if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            silence.assign(size_t{frames} * stream.channels, 0.0f);
            samples = silence.data();
          }
          envelope_.Add(first_ns, stream.sample_rate, stream.channels, samples, frames);
          peak_.Add(samples, size_t{frames} * stream.channels);
          stream.capture->ReleaseBuffer(frames);
        }
      }
      stream.client->Stop();
      if (FAILED(hr)) {
        std::lock_guard lock(mutex_);
        error_ = std::format("Recording from {} stopped ({}).", device_id, HrText(hr));
      }
    }
  }
  CloseHandle(packet_event);
  CoUninitialize();
}

struct EndpointPlayer::Stream {
  ComPtr<IAudioClient> client;
  ComPtr<IAudioRenderClient> render;
  uint32_t sample_rate = 0;
  uint32_t channels = 0;
};

EndpointPlayer::EndpointPlayer() : stream_(std::make_unique<Stream>()) {}

EndpointPlayer::~EndpointPlayer() {
  if (stream_->client) stream_->client->Stop();
}

Result<std::unique_ptr<EndpointPlayer>> EndpointPlayer::Open(const std::string& device_id) {
  std::unique_ptr<EndpointPlayer> player(new EndpointPlayer());
  Stream& stream = *player->stream_;
  MixFormat format(nullptr, &CoTaskMemFree);
  const Status activated = ActivateClient(device_id, EndpointFlow::kPlayback, stream.client, format);
  if (!activated) return Error{activated.error()};
  stream.sample_rate = format->nSamplesPerSec;
  stream.channels = format->nChannels;
  // The monitor's settings: no flags, a 1 s buffer.
  HRESULT hr = stream.client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10'000'000, 0, format.get(), nullptr);
  if (SUCCEEDED(hr)) hr = stream.client->GetService(IID_PPV_ARGS(&stream.render));
  if (SUCCEEDED(hr)) hr = stream.client->Start();
  if (FAILED(hr)) return Error{std::format("Couldn't start playing to {} ({}).", device_id, HrText(hr))};
  return player;
}

uint32_t EndpointPlayer::sample_rate() const { return stream_->sample_rate; }

uint32_t EndpointPlayer::channels() const { return stream_->channels; }

Status EndpointPlayer::Write(const float* interleaved, uint32_t frames) {
  BYTE* buffer = nullptr;
  HRESULT hr = stream_->render->GetBuffer(frames, &buffer);
  if (FAILED(hr)) return Error{std::format("Couldn't queue audio ({}).", HrText(hr))};
  std::memcpy(buffer, interleaved, size_t{frames} * stream_->channels * sizeof(float));
  hr = stream_->render->ReleaseBuffer(frames, 0);
  if (FAILED(hr)) return Error{std::format("Couldn't queue audio ({}).", HrText(hr))};
  return Ok{};
}

struct EndpointClock::Stream {
  ComPtr<IAudioClient> client;
  ComPtr<IAudioRenderClient> render;    // Playback.
  ComPtr<IAudioCaptureClient> capture;  // Recording.
  ComPtr<IAudioClock> clock;
  uint32_t sample_rate = 0;
  uint64_t frequency = 0;  // IAudioClock's position units per second.
  uint64_t polls = 0;
  uint64_t glitches = 0;
};

EndpointClock::EndpointClock() : stream_(std::make_unique<Stream>()) {}

EndpointClock::~EndpointClock() {
  if (stream_->client) stream_->client->Stop();
}

Result<std::unique_ptr<EndpointClock>> EndpointClock::Open(const std::string& device_id, EndpointFlow flow) {
  std::unique_ptr<EndpointClock> timer(new EndpointClock());
  Stream& stream = *timer->stream_;
  MixFormat format(nullptr, &CoTaskMemFree);
  const Status activated = ActivateClient(device_id, flow, stream.client, format);
  if (!activated) return Error{activated.error()};
  stream.sample_rate = format->nSamplesPerSec;
  UINT64 frequency = 0;
  HRESULT hr = stream.client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBufferDuration, 0, format.get(), nullptr);
  if (SUCCEEDED(hr)) {
    hr = flow == EndpointFlow::kPlayback ? stream.client->GetService(IID_PPV_ARGS(&stream.render))
                                         : stream.client->GetService(IID_PPV_ARGS(&stream.capture));
  }
  if (SUCCEEDED(hr)) hr = stream.client->GetService(IID_PPV_ARGS(&stream.clock));
  if (SUCCEEDED(hr)) hr = stream.clock->GetFrequency(&frequency);
  if (FAILED(hr) || frequency == 0) {
    return Error{std::format("Couldn't open a stream to time {} ({}).", device_id, HrText(hr))};
  }
  stream.frequency = frequency;
  if (stream.render) {
    // Queue the first silence before starting, so the stream never runs dry.
    const auto first = timer->Poll();
    if (!first) return Error{first.error()};
  }
  hr = stream.client->Start();
  if (FAILED(hr)) return Error{std::format("Couldn't start a stream on {} ({}).", device_id, HrText(hr))};
  return timer;
}

uint32_t EndpointClock::sample_rate() const { return stream_->sample_rate; }

uint64_t EndpointClock::glitches() const { return stream_->glitches; }

Result<EndpointClock::Reading> EndpointClock::Poll() {
  Stream& stream = *stream_;
  HRESULT hr = S_OK;
  if (stream.capture) {
    // Drop what was recorded, unread.
    UINT32 packet = 0;
    while (SUCCEEDED(hr = stream.capture->GetNextPacketSize(&packet)) && packet > 0) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      hr = stream.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
      if (FAILED(hr)) break;
      if (stream.polls > 0 && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)) ++stream.glitches;
      stream.capture->ReleaseBuffer(frames);
    }
    ++stream.polls;
    if (FAILED(hr)) return Error{std::format("The stream stopped ({}).", HrText(hr))};
  }
  UINT32 padding = 0;
  if (stream.render) hr = stream.client->GetCurrentPadding(&padding);
  if (FAILED(hr)) return Error{std::format("The stream stopped ({}).", HrText(hr))};
  if (stream.render && stream.polls++ > 0 && padding < stream.sample_rate / 100) ++stream.glitches;
  const UINT32 target = stream.sample_rate / 10;
  if (stream.render && padding < target) {
    const UINT32 frames = target - padding;
    BYTE* buffer = nullptr;
    hr = stream.render->GetBuffer(frames, &buffer);
    if (SUCCEEDED(hr)) hr = stream.render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr)) return Error{std::format("Couldn't queue silence ({}).", HrText(hr))};
  }
  UINT64 position = 0;
  UINT64 qpc_position = 0;  // 100 ns units
  hr = stream.clock->GetPosition(&position, &qpc_position);
  if (FAILED(hr)) return Error{std::format("Couldn't read the stream's position ({}).", HrText(hr))};
  return Reading{qpc_position * 100, static_cast<double>(position) / static_cast<double>(stream.frequency)};
}

}  // namespace knobs::tools
