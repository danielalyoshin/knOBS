// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/wav.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>

#include "util/win_strings.h"

namespace knobs::tools {
namespace {

constexpr uint16_t kFormatFloat = 3;
constexpr uint16_t kFormatExtensible = 0xFFFE;
// The GUID of KSDATAFORMAT_SUBTYPE_IEEE_FLOAT after its first two bytes,
// which hold the format tag.
constexpr uint8_t kFloatSubtypeTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                           0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

template <typename T>
T Load(const uint8_t* p) {
  T value;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

template <typename T>
void Append(std::vector<uint8_t>& out, T value) {
  const auto* p = reinterpret_cast<const uint8_t*>(&value);
  out.insert(out.end(), p, p + sizeof(value));
}

void AppendTag(std::vector<uint8_t>& out, std::string_view tag) { out.insert(out.end(), tag.begin(), tag.end()); }

}  // namespace

Result<FloatAudio> ReadFloatWav(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return Error{std::format("Couldn't open {}.", ToUtf8(file))};
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::string name = ToUtf8(file);
  if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
      std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
    return Error{std::format("{} isn't a WAV file.", name)};
  }

  const uint8_t* fmt = nullptr;
  size_t fmt_size = 0;
  const uint8_t* data = nullptr;
  size_t data_size = 0;
  for (size_t pos = 12; pos + 8 <= bytes.size();) {
    const uint32_t size = Load<uint32_t>(&bytes[pos + 4]);
    const size_t body = pos + 8;
    // Streaming writers leave the data size too large; take what's there.
    const size_t available = std::min<size_t>(size, bytes.size() - body);
    if (std::memcmp(&bytes[pos], "fmt ", 4) == 0) {
      fmt = &bytes[body];
      fmt_size = available;
    } else if (std::memcmp(&bytes[pos], "data", 4) == 0) {
      data = &bytes[body];
      data_size = available;
    }
    if (size > bytes.size() - body) break;
    pos = body + size + (size & 1);  // Chunks are padded to even sizes.
  }
  if (!fmt || fmt_size < 16 || !data) {
    return Error{std::format("{} has no {} chunk.", name, !fmt || fmt_size < 16 ? "fmt" : "data")};
  }

  const uint16_t tag = Load<uint16_t>(fmt);
  const uint16_t channels = Load<uint16_t>(fmt + 2);
  const uint32_t rate = Load<uint32_t>(fmt + 4);
  const uint16_t block_align = Load<uint16_t>(fmt + 12);
  const uint16_t bits = Load<uint16_t>(fmt + 14);
  const bool is_float =
      tag == kFormatFloat ||
      (tag == kFormatExtensible && fmt_size >= 40 && Load<uint16_t>(fmt + 24) == kFormatFloat &&
       std::memcmp(fmt + 26, kFloatSubtypeTail, sizeof(kFloatSubtypeTail)) == 0);
  if (!is_float || bits != 32) {
    return Error{std::format("{} isn't 32-bit float (format {}, {} bits). Convert it first, e.g. "
                             "ffmpeg -i in.wav -c:a pcm_f32le out.wav",
                             name, tag, bits)};
  }
  if (channels == 0 || rate == 0 || block_align != channels * 4u) {
    return Error{std::format("{} has an inconsistent format header.", name)};
  }

  FloatAudio audio;
  audio.sample_rate = rate;
  audio.channels = channels;
  audio.samples.resize(data_size / block_align * channels);
  std::memcpy(audio.samples.data(), data, audio.samples.size() * sizeof(float));
  return audio;
}

Status WriteFloatWav(const std::filesystem::path& file, const FloatAudio& audio) {
  const uint64_t data_size = audio.samples.size() * sizeof(float);
  if (audio.channels == 0 || audio.samples.size() % audio.channels != 0 ||
      data_size > std::numeric_limits<uint32_t>::max() - 64) {
    return Error{std::format("Can't write {}: the audio is malformed or too long for a WAV file.",
                             ToUtf8(file))};
  }
  const uint16_t block_align = static_cast<uint16_t>(audio.channels * sizeof(float));

  std::vector<uint8_t> header;
  AppendTag(header, "RIFF");
  Append<uint32_t>(header, static_cast<uint32_t>(4 + (8 + 18) + (8 + 4) + 8 + data_size));
  AppendTag(header, "WAVE");
  AppendTag(header, "fmt ");
  Append<uint32_t>(header, 18);
  Append<uint16_t>(header, kFormatFloat);
  Append<uint16_t>(header, static_cast<uint16_t>(audio.channels));
  Append<uint32_t>(header, audio.sample_rate);
  Append<uint32_t>(header, audio.sample_rate * block_align);
  Append<uint16_t>(header, block_align);
  Append<uint16_t>(header, 32);
  Append<uint16_t>(header, 0);  // cbSize
  // Required for non-PCM formats.
  AppendTag(header, "fact");
  Append<uint32_t>(header, 4);
  Append<uint32_t>(header, static_cast<uint32_t>(audio.frames()));
  AppendTag(header, "data");
  Append<uint32_t>(header, static_cast<uint32_t>(data_size));

  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
  out.write(reinterpret_cast<const char*>(audio.samples.data()), static_cast<std::streamsize>(data_size));
  out.close();
  if (!out) return Error{std::format("Couldn't write {}.", ToUtf8(file))};
  return Ok{};
}

}  // namespace knobs::tools
