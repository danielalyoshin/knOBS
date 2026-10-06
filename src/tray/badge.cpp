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

// Windows 11's status badge colors, as WinUI's InfoBadge and InfoBar use
// them in dark mode: SystemFillColorCaution for "!" and
// SystemFillColorSolidNeutral for pause, both with a black mark. The same on
// a light taskbar (decided 2026-10-05).
struct Palette {
  Color fill;
  Color mark;
};

Palette Colors(Badge badge) {
  return {Rgb(badge == Badge::kAttention ? 0xfce100 : 0x9d9d9d), Rgb(0x000000)};
}

constexpr float kBadge = 0.5625f;  // Of the icon's size: 9 px at 16.

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

// The nearest whole number to `value` that's odd if `odd`, else even: a
// stroke that size centered on a badge of that parity has whole-pixel edges.
int WithParity(float value, bool odd) {
  int whole = static_cast<int>(std::lround(value));
  if ((whole % 2 != 0) != odd) whole += value > whole ? 1 : -1;
  return std::max(odd ? 1 : 2, whole);
}

// The badge's mark, as coverage of each of the icon's pixels: Windows 11's
// "!" and pause in proportion, with whole-pixel strokes, so they stay crisp
// at tray sizes.
std::vector<float> Mark(Badge badge, int size, int diameter) {
  std::vector<float> mark(static_cast<size_t>(size) * size, 0.0f);
  const bool odd = diameter % 2 != 0;
  // The badge's top-left corner.
  const int corner = size - diameter;
  const auto fill = [&](int left, int top, int width, int height, bool round) {
    const float radius = round ? std::min(width, height) / 2.0f : 0.0f;
    for (int y = top; y < top + height; ++y) {
      for (int x = left; x < left + width; ++x) {
        float coverage = 1.0f;
        if (radius >= 1.5f) {
          coverage = Coverage(RoundedRect(x + 0.5f, y + 0.5f, left + width / 2.0f, top + height / 2.0f,
                                          width / 2.0f, height / 2.0f));
        }
        if (x >= 0 && y >= 0 && x < size && y < size) mark[static_cast<size_t>(y) * size + x] = coverage;
      }
    }
  };
  if (badge == Badge::kAttention) {
    // A stem and a dot a stroke apart, about half the badge tall.
    const int stroke = WithParity(diameter * 0.11f, odd);
    const int height = WithParity(diameter * 0.56f, odd);
    const int left = corner + (diameter - stroke) / 2;
    const int top = corner + (diameter - height) / 2;
    fill(left, top, stroke, height - 2 * stroke, true);
    fill(left, top + height - stroke, stroke, stroke, true);
  } else {
    // Two solid bars, as Windows' pause glyph has them.
    const int bar = std::max(1, static_cast<int>(std::lround(diameter * 0.18f)));
    int gap = std::max(1, static_cast<int>(std::lround(diameter * 0.12f)));
    if (((2 * bar + gap) % 2 != 0) != odd) ++gap;
    const int height = WithParity(diameter * 0.5f, odd);
    const int left = corner + (diameter - (2 * bar + gap)) / 2;
    const int top = corner + (diameter - height) / 2;
    fill(left, top, bar, height, true);
    fill(left + bar + gap, top, bar, height, true);
  }
  return mark;
}

}  // namespace

IconPixels AddBadge(IconPixels icon, Badge badge) {
  if (badge == Badge::kNone || icon.size <= 0) return icon;
  const float size = static_cast<float>(icon.size);
  // Whole pixels, flush with the corner.
  const int whole_diameter = static_cast<int>(std::lround(size * kBadge));
  const float diameter = static_cast<float>(whole_diameter);
  const float radius = diameter / 2;
  const float center = size - radius;
  // The clear ring: a pixel at 16 px, two at 32.
  const float gap = std::max(1.0f, std::round(size / 16));
  const Palette colors = Colors(badge);
  const std::vector<float> mark = Mark(badge, icon.size, whole_diameter);

  for (int y = 0; y < icon.size; ++y) {
    for (int x = 0; x < icon.size; ++x) {
      const float px = x + 0.5f;
      const float py = y + 0.5f;
      const float distance = std::hypot(px - center, py - center);
      const float cut = Coverage(distance - (radius + gap));
      const float disc = Coverage(distance - radius);
      if (cut <= 0) continue;
      const size_t index = static_cast<size_t>(y) * icon.size + x;
      const Color color = Mix(colors.fill, colors.mark, mark[index]);

      uint32_t& pixel = icon.bgra[index];
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

HICON BadgedIcon(HICON icon, Badge badge) {
  if (!icon) return nullptr;
  if (badge == Badge::kNone) return CopyIcon(icon);
  auto pixels = ReadIcon(icon);
  return pixels ? CreateIconFrom(AddBadge(std::move(*pixels), badge)) : nullptr;
}

}  // namespace knobs::tray
