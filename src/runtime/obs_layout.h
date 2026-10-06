// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <filesystem>
#include <string_view>

// Folder layout shared by an OBS install and knobs's runtime copy of it. The
// copy mirrors the install because libobs finds files relative to bin\64bit:
// "../../data/libobs/" for its own data (obs-windows.c) and "../../obs-plugins"
// for modules.
namespace knobs::runtime {

// The only modules knobs loads. No obs-vst in v1.
inline constexpr std::array<std::string_view, 2> kObsModules = {"win-wasapi", "obs-filters"};

// Graphics module for the dummy-video fallback (see VideoMode::kDummy).
inline constexpr std::string_view kGraphicsModule = "libobs-d3d11";

// OBS's program, in bin\64bit.
inline constexpr std::wstring_view kObsExe = L"obs64.exe";

inline std::filesystem::path BinDir(const std::filesystem::path& root) {
  return root / L"bin" / L"64bit";
}

// libobs, in bin\64bit.
inline constexpr std::wstring_view kObsDll = L"obs.dll";

inline std::filesystem::path ObsDll(const std::filesystem::path& root) {
  return BinDir(root) / kObsDll;
}

inline std::filesystem::path PluginBinDir(const std::filesystem::path& root) {
  return root / L"obs-plugins" / L"64bit";
}

inline std::filesystem::path PluginDll(const std::filesystem::path& root, std::string_view module) {
  return PluginBinDir(root) / (std::filesystem::path(module) += L".dll");
}

inline std::filesystem::path LibobsDataDir(const std::filesystem::path& root) {
  return root / L"data" / L"libobs";
}

inline std::filesystem::path PluginDataDir(const std::filesystem::path& root,
                                           std::string_view module) {
  return root / L"data" / L"obs-plugins" / std::filesystem::path(module);
}

}  // namespace knobs::runtime
