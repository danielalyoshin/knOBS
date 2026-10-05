// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/obs_import.h"

#include <format>

#include "common/console.h"
#include "util/win_strings.h"

namespace knobs::tools {

bool ParseImportArg(std::wstring_view arg, const wchar_t* value, ImportArgs& args, bool& bad) {
  if (arg != L"--obs-config" && arg != L"--pick") return false;
  if (!value) {
    bad = true;
  } else if (arg == L"--obs-config") {
    args.config_dir = std::filesystem::absolute(value);
  } else {
    args.pick = ToUtf8(std::wstring_view(value));
  }
  return true;
}

Result<import::ActiveObsConfig> FindObsConfig(const ImportArgs& args, const runtime::ObsInstall& install) {
  import::ObsConfigRoot root;
  if (args.config_dir) {
    root.path = *args.config_dir;
  } else {
    auto found = import::FindObsConfigRoot(install.root);
    if (!found) return Error{found.error()};
    root = *found;
  }
  auto config = import::FindActiveObsConfig(root);
  if (!config) return Error{config.error()};
  Check(true, "OBS settings", std::format("{}{}", ToUtf8(config->settings_file), root.portable ? " (portable)" : ""));
  const import::ProfileAudio& audio = config->audio;
  const std::string monitor = !audio.monitoring_device_name.empty() ? audio.monitoring_device_name
                              : audio.monitoring_device_id == "default" ? std::string("Default")
                                                                        : audio.monitoring_device_id;
  Check(true, "profile",
        std::format("\"{}\": {} Hz, {}, monitoring to \"{}\"", config->profile, audio.sample_rate,
                    audio.channel_setup, monitor));
  return config;
}

void UseProfileAudio(const import::ActiveObsConfig& config, runtime::HostOptions& options) {
  options.samples_per_sec = config.audio.sample_rate;
  options.speakers = config.audio.speakers;
}

Result<import::ImportedMic> ImportMic(const runtime::ObsApi& api, const import::ActiveObsConfig& config,
                                      const ImportArgs& args) {
  auto file = import::FindSceneCollectionFile(
      config, [&](const std::filesystem::path& path) { return import::ReadCollectionName(api, path); });
  if (!file) return Error{file.error()};
  auto collection = import::SceneCollection::Read(api, *file);
  if (!collection) return Error{collection.error()};
  Check(true, "collection", std::format("\"{}\": {}", config.collection, ToUtf8((*collection)->file())));
  if ((*collection)->from_backup()) {
    Report(Outcome::kWarn, "collection", std::format("{} doesn't parse, so this is its backup", ToUtf8(*file)));
  }

  const auto mics = (*collection)->Mics();
  if (mics.size() > 1) {
    for (size_t i = 0; i < mics.size(); ++i) Report(Outcome::kNote, "mics", import::DescribeMic(mics[i], i + 1));
  }
  auto picked = import::PickMic(mics, args.pick);
  if (!picked) {
    return Error{mics.size() > 1 && args.pick.empty() ? picked.error() + "\nPass --pick with a number or a name."
                                                      : picked.error()};
  }
  const import::MicCandidate& mic = mics[*picked];
  Check(true, "mic", std::format("{}, device {}", import::DescribeMic(mic, *picked + 1), mic.device_id));

  auto imported = (*collection)->Import(mic);
  if (!imported) return Error{imported.error()};
  for (const import::ImportNote& note : imported->notes) {
    Report(note.warning ? Outcome::kWarn : Outcome::kNote, "pre-flight", note.text);
  }
  if (imported->notes.empty()) Check(true, "pre-flight", "nothing to report");
  return imported;
}

}  // namespace knobs::tools
