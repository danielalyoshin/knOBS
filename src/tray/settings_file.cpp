// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/settings_file.h"

#include <format>
#include <system_error>

#include "import/obs_ini.h"
#include "util/text_file.h"
#include "util/win_strings.h"

namespace knobs::tray {
namespace {

std::string Escape(std::string_view value) {
  std::string escaped;
  for (const char c : value) {
    switch (c) {
      case '\\':
        escaped += "\\\\";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      default:
        escaped += c;
    }
  }
  return escaped;
}

void Line(std::string& text, std::string_view key, std::string_view value) {
  text += std::format("{}={}\n", key, Escape(value));
}

// As libobs's config_get_bool reads one: "true", or a non-zero number.
// Anything else that isn't "false" or "0" keeps the default.
bool ReadBool(const import::ObsIni& ini, std::string_view section, std::string_view key, bool fallback) {
  const auto value = ini.Get(section, key);
  if (!value) return fallback;
  const std::string lower = AsciiLower(*value);
  if (lower == "true" || lower == "1") return true;
  if (lower == "false" || lower == "0") return false;
  return fallback;
}

}  // namespace

std::string FormatSettings(const core::Settings& settings) {
  std::string text = "[OBS]\n";
  if (settings.obs_dir) Line(text, "Install", ToUtf8(*settings.obs_dir));
  if (settings.obs_config) Line(text, "Settings", ToUtf8(*settings.obs_config));
  Line(text, "PauseWhileOpen", settings.pause_for_obs ? "true" : "false");
  text += "\n[Audio]\n";
  if (!settings.mic.empty()) Line(text, "Mic", settings.mic);
  if (!settings.cable.empty()) {
    Line(text, "Cable", settings.cable);
    if (!settings.cable_name.empty()) Line(text, "CableName", settings.cable_name);
  }
  return text;
}

core::Settings ParseSettings(std::string_view text) {
  const import::ObsIni ini = import::ObsIni::Parse(text);
  core::Settings settings;
  const auto path = [&ini](std::string_view key) -> std::optional<std::filesystem::path> {
    const auto value = ini.Get("OBS", key);
    if (!value || value->empty()) return std::nullopt;
    return std::filesystem::path(FromUtf8(*value));
  };
  settings.obs_dir = path("Install");
  settings.obs_config = path("Settings");
  settings.pause_for_obs = ReadBool(ini, "OBS", "PauseWhileOpen", settings.pause_for_obs);
  settings.mic = ini.Get("Audio", "Mic").value_or("");
  settings.cable = ini.Get("Audio", "Cable").value_or("");
  if (!settings.cable.empty()) settings.cable_name = ini.Get("Audio", "CableName").value_or("");
  return settings;
}

Result<core::Settings> LoadSettings(const std::filesystem::path& file) {
  std::error_code ec;
  if (!std::filesystem::exists(file, ec) && !ec) return core::Settings{};
  auto text = ReadText(file);
  if (!text) return Error{text.error()};
  return ParseSettings(*text);
}

Status SaveSettings(const std::filesystem::path& file, const core::Settings& settings) {
  return WriteText(file, FormatSettings(settings));
}

}  // namespace knobs::tray
