// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/screenshot.h"

#include <dwmapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <format>
#include <string_view>

#include "util/win_strings.h"

namespace knobs::tools {
namespace {

using Microsoft::WRL::ComPtr;

Status Failed(std::string_view step, HRESULT result) {
  return Error{std::format("Couldn't save the picture ({} failed, 0x{:08X}).", step, static_cast<uint32_t>(result))};
}

Status SavePng(HBITMAP bitmap, const std::filesystem::path& file) {
  ComPtr<IWICImagingFactory> factory;
  HRESULT result =
      CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(result)) return Failed("CoCreateInstance", result);
  ComPtr<IWICBitmap> source;
  result = factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &source);
  if (FAILED(result)) return Failed("CreateBitmapFromHBITMAP", result);
  ComPtr<IWICStream> stream;
  result = factory->CreateStream(&stream);
  if (SUCCEEDED(result)) result = stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE);
  if (FAILED(result)) return Failed("opening the file", result);
  ComPtr<IWICBitmapEncoder> encoder;
  result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  if (SUCCEEDED(result)) result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  ComPtr<IWICBitmapFrameEncode> frame;
  if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&frame, nullptr);
  if (SUCCEEDED(result)) result = frame->Initialize(nullptr);
  if (SUCCEEDED(result)) result = frame->WriteSource(source.Get(), nullptr);
  if (SUCCEEDED(result)) result = frame->Commit();
  if (SUCCEEDED(result)) result = encoder->Commit();
  if (FAILED(result)) return Failed("encoding", result);
  return Ok{};
}

// This thread's visible top-level windows of `window_class`, top of the
// z-order first.
std::vector<HWND> ThreadWindows(std::wstring_view window_class) {
  struct Search {
    std::wstring_view window_class;
    std::vector<HWND> found;
  } search{window_class};
  EnumThreadWindows(
      GetCurrentThreadId(),
      [](HWND window, LPARAM param) -> BOOL {
        auto* search = reinterpret_cast<Search*>(param);
        wchar_t name[16] = {};
        GetClassNameW(window, name, static_cast<int>(std::size(name)));
        if (std::wstring_view(name) == search->window_class && IsWindowVisible(window)) {
          search->found.push_back(window);
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&search));
  return search.found;
}

}  // namespace

std::vector<HWND> OpenMenus() {
  // The newest submenu is on top.
  const std::vector<HWND> menus = ThreadWindows(L"#32768");
  return {menus.rbegin(), menus.rend()};
}

HWND OpenDialog() {
  const std::vector<HWND> dialogs = ThreadWindows(L"#32770");
  return dialogs.empty() ? nullptr : dialogs.front();
}

std::optional<RECT> BoundsOf(const std::vector<HWND>& windows) {
  std::optional<RECT> bounds;
  for (const HWND window : windows) {
    RECT rect = {};
    if (!GetWindowRect(window, &rect)) continue;
    if (!bounds) {
      bounds = rect;
    } else {
      UnionRect(&*bounds, &*bounds, &rect);
    }
  }
  return bounds;
}

Status SaveScreenPng(const RECT& rect, const std::filesystem::path& file) {
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) return Error{"There's nothing to take a picture of."};
  const HDC screen = GetDC(nullptr);
  const HDC memory = CreateCompatibleDC(screen);
  const HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
  const HGDIOBJ previous = SelectObject(memory, bitmap);
  // CAPTUREBLT takes in layered windows, which menus and their shadows are.
  const bool copied = BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT);
  SelectObject(memory, previous);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
  Status saved = copied ? SavePng(bitmap, file)
                        : Status(Error{std::format("Couldn't copy the screen: {}", DescribeWinError(GetLastError()))});
  DeleteObject(bitmap);
  return saved;
}

Status SavePixelsPng(int width, int height, const std::vector<uint32_t>& bgra, const std::filesystem::path& file) {
  if (width <= 0 || height <= 0 || bgra.size() != static_cast<size_t>(width) * height) {
    return Error{"There's nothing to take a picture of."};
  }
  BITMAPINFO format = {};
  format.bmiHeader.biSize = sizeof(format.bmiHeader);
  format.bmiHeader.biWidth = width;
  format.bmiHeader.biHeight = -height;  // Top-down.
  format.bmiHeader.biPlanes = 1;
  format.bmiHeader.biBitCount = 32;
  format.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  const HBITMAP bitmap = CreateDIBSection(nullptr, &format, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap) return Error{"Couldn't make a bitmap for the picture."};
  std::copy(bgra.begin(), bgra.end(), static_cast<uint32_t*>(bits));
  Status saved = SavePng(bitmap, file);
  DeleteObject(bitmap);
  return saved;
}

RECT NotificationArea() {
  MONITORINFO monitor = {sizeof(monitor)};
  GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
  const RECT work = monitor.rcWork;
  // In pixels at 96 DPI: a notification is 364 wide, with a margin.
  const int scale = static_cast<int>(GetDpiForSystem());
  return {work.right - MulDiv(400, scale, 96), work.bottom - MulDiv(300, scale, 96), work.right, work.bottom};
}

Status SaveWindowPng(HWND window, const std::filesystem::path& file) {
  RECT window_rect = {};
  GetWindowRect(window, &window_rect);
  // The window's rectangle takes in its invisible resizing borders; the
  // frame is what shows.
  RECT frame = window_rect;
  DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame));
  const HDC screen = GetDC(nullptr);
  const HDC whole = CreateCompatibleDC(screen);
  const HBITMAP whole_bitmap =
      CreateCompatibleBitmap(screen, window_rect.right - window_rect.left, window_rect.bottom - window_rect.top);
  const HGDIOBJ whole_previous = SelectObject(whole, whole_bitmap);
  const bool printed = PrintWindow(window, whole, PW_RENDERFULLCONTENT);
  const int width = frame.right - frame.left;
  const int height = frame.bottom - frame.top;
  const HDC cropped = CreateCompatibleDC(screen);
  const HBITMAP cropped_bitmap = CreateCompatibleBitmap(screen, width, height);
  const HGDIOBJ cropped_previous = SelectObject(cropped, cropped_bitmap);
  BitBlt(cropped, 0, 0, width, height, whole, frame.left - window_rect.left, frame.top - window_rect.top, SRCCOPY);
  SelectObject(cropped, cropped_previous);
  SelectObject(whole, whole_previous);
  DeleteDC(cropped);
  DeleteDC(whole);
  ReleaseDC(nullptr, screen);
  DeleteObject(whole_bitmap);
  Status saved = printed ? SavePng(cropped_bitmap, file) : Status(Error{"The window didn't draw itself."});
  DeleteObject(cropped_bitmap);
  return saved;
}

}  // namespace knobs::tools
