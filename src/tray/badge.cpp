// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/badge.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace knobs::tray {
namespace {

struct Color {
  float r, g, b;
};

constexpr Color Rgb(uint32_t rgb) {
  return {((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f};
}

// The logo's ink and light gray (assets/README.md), and an amber that reads
// on both, with a darker rim against a light taskbar.
constexpr Color kInk = Rgb(0x17181b);
constexpr Color kLight = Rgb(0xe8eaee);
constexpr Color kAmber = Rgb(0xf5a400);
constexpr Color kAmberRim = Rgb(0xa86a00);

// In fractions of the badge's diameter, from its center.
constexpr float kBadge = 0.5625f;  // Of the icon's size: 9 px at 16.
constexpr float kStemHalfWidth = 0.1f;
constexpr float kStemTop = -0.32f;
constexpr float kStemBottom = 0.08f;
constexpr float kDotCenter = 0.27f;
constexpr float kDotRadius = 0.11f;
constexpr float kBarHalfWidth = 0.09f;
constexpr float kBarHalfHeight = 0.24f;
constexpr float kBarOffset = 0.15f;

// How much of a pixel a shape covers, from the signed distance of the
// pixel's center to the shape's edge, in pixels (negative inside).
float Coverage(float distance) { return std::clamp(0.5f - distance, 0.0f, 1.0f); }

// Signed distance to a rectangle with rounded ends, centered on (cx, cy).
float RoundedRect(float x, float y, float cx, float cy, float half_width, float half_height) {
  const float radius = std::min(half_width, half_height);
  const float qx = std::abs(x - cx) - (half_width - radius);
  const float qy = std::abs(y - cy) - (half_height - radius);
  const float outside = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f));
  return outside + std::min(std::max(qx, qy), 0.0f) - radius;
}

Color Mix(Color a, Color b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

uint8_t Byte(float value) { return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)); }

}  // namespace

IconPixels AddBadge(IconPixels icon, Badge badge, bool dark_taskbar) {
  if (badge == Badge::kNone || icon.size <= 0) return icon;
  const float size = static_cast<float>(icon.size);
  const float diameter = size * kBadge;
  const float radius = diameter / 2;
  const float center = size - radius;
  // The clear ring: a pixel at 16 px, two at 32.
  const float gap = std::max(1.0f, size / 16);
  const float rim = size / 24;

  Color fill = kAmber;
  Color mark = kInk;
  if (badge == Badge::kPaused) {
    fill = dark_taskbar ? kLight : kInk;
    mark = dark_taskbar ? kInk : kLight;
  }
  const bool has_rim = badge == Badge::kAttention && !dark_taskbar;

  for (int y = 0; y < icon.size; ++y) {
    for (int x = 0; x < icon.size; ++x) {
      const float px = x + 0.5f;
      const float py = y + 0.5f;
      const float distance = std::hypot(px - center, py - center);
      const float cut = Coverage(distance - (radius + gap));
      const float disc = Coverage(distance - radius);
      if (cut <= 0) continue;

      float mark_distance = 0;
      if (badge == Badge::kAttention) {
        const float stem_center = center + diameter * (kStemTop + kStemBottom) / 2;
        const float stem = RoundedRect(px, py, center, stem_center, diameter * kStemHalfWidth,
                                       diameter * (kStemBottom - kStemTop) / 2);
        const float dot = std::hypot(px - center, py - (center + diameter * kDotCenter)) - diameter * kDotRadius;
        mark_distance = std::min(stem, dot);
      } else {
        const float half_width = diameter * kBarHalfWidth;
        const float half_height = diameter * kBarHalfHeight;
        mark_distance =
            std::min(RoundedRect(px, py, center - diameter * kBarOffset, center, half_width, half_height),
                     RoundedRect(px, py, center + diameter * kBarOffset, center, half_width, half_height));
      }
      Color color = fill;
      if (has_rim) color = Mix(kAmberRim, fill, Coverage(distance - (radius - rim)));
      color = Mix(color, mark, Coverage(mark_distance));

      uint32_t& pixel = icon.bgra[static_cast<size_t>(y) * icon.size + x];
      const float base_alpha = ((pixel >> 24) / 255.0f) * (1 - cut);
      const Color base = Rgb(pixel & 0xffffff);
      const float alpha = disc + base_alpha * (1 - disc);
      Color out = color;
      if (alpha > 0) {
        const float weight = base_alpha * (1 - disc) / alpha;
        out = Mix(color, base, weight);
      }
      pixel = (uint32_t{Byte(alpha)} << 24) | (uint32_t{Byte(out.r)} << 16) | (uint32_t{Byte(out.g)} << 8) |
              Byte(out.b);
    }
  }
  return icon;
}

std::optional<IconPixels> ReadIcon(HICON icon) {
  ICONINFO info = {};
  if (!icon || !GetIconInfo(icon, &info)) return std::nullopt;
  // Deletes the icon's bitmaps on the way out.
  struct Bitmaps {
    ICONINFO& info;
    ~Bitmaps() {
      if (info.hbmColor) DeleteObject(info.hbmColor);
      if (info.hbmMask) DeleteObject(info.hbmMask);
    }
  } bitmaps{info};
  BITMAP bitmap = {};
  if (!info.hbmColor || !GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) || bitmap.bmWidth <= 0 ||
      bitmap.bmWidth != bitmap.bmHeight) {
    return std::nullopt;
  }

  IconPixels pixels;
  pixels.size = bitmap.bmWidth;
  const int size = pixels.size;
  pixels.bgra.resize(static_cast<size_t>(size) * size);
  BITMAPINFO format = {};
  format.bmiHeader.biSize = sizeof(format.bmiHeader);
  format.bmiHeader.biWidth = size;
  format.bmiHeader.biHeight = -size;  // Top-down.
  format.bmiHeader.biPlanes = 1;
  format.bmiHeader.biBitCount = 32;
  format.bmiHeader.biCompression = BI_RGB;
  const HDC screen = GetDC(nullptr);
  const bool read = GetDIBits(screen, info.hbmColor, 0, size, pixels.bgra.data(), &format, DIB_RGB_COLORS) == size;
  // An icon without alpha takes it from its mask, where a set bit is clear.
  bool has_alpha = std::any_of(pixels.bgra.begin(), pixels.bgra.end(), [](uint32_t p) { return (p >> 24) != 0; });
  if (read && !has_alpha) {
    std::vector<uint32_t> mask(pixels.bgra.size());
    format.bmiHeader.biHeight = -size;
    if (GetDIBits(screen, info.hbmMask, 0, size, mask.data(), &format, DIB_RGB_COLORS) == size) {
      for (size_t i = 0; i < mask.size(); ++i) {
        if ((mask[i] & 0xffffff) == 0) pixels.bgra[i] |= 0xff000000;
      }
    }
  }
  ReleaseDC(nullptr, screen);
  if (!read) return std::nullopt;
  return pixels;
}

HICON CreateIconFrom(const IconPixels& pixels) {
  const int size = pixels.size;
  if (size <= 0 || pixels.bgra.size() != static_cast<size_t>(size) * size) return nullptr;
  BITMAPINFO format = {};
  format.bmiHeader.biSize = sizeof(format.bmiHeader);
  format.bmiHeader.biWidth = size;
  format.bmiHeader.biHeight = -size;  // Top-down.
  format.bmiHeader.biPlanes = 1;
  format.bmiHeader.biBitCount = 32;
  format.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  const HBITMAP color = CreateDIBSection(nullptr, &format, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!color) return nullptr;
  std::copy(pixels.bgra.begin(), pixels.bgra.end(), static_cast<uint32_t*>(bits));
  // All zero: the color bitmap's alpha decides what shows.
  const std::vector<uint8_t> clear(static_cast<size_t>((size + 15) / 16 * 2) * size, 0);
  const HBITMAP mask = CreateBitmap(size, size, 1, 1, clear.data());
  ICONINFO badged = {TRUE, 0, 0, mask, color};
  const HICON result = mask ? CreateIconIndirect(&badged) : nullptr;
  DeleteObject(color);
  if (mask) DeleteObject(mask);
  return result;
}

HICON BadgedIcon(HICON icon, Badge badge, bool dark_taskbar) {
  if (!icon) return nullptr;
  if (badge == Badge::kNone) return CopyIcon(icon);
  auto pixels = ReadIcon(icon);
  return pixels ? CreateIconFrom(AddBadge(std::move(*pixels), badge, dark_taskbar)) : nullptr;
}

}  // namespace knobs::tray
