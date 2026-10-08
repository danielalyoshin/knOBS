// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "import/obs_config.h"
#include "runtime/obs_api.h"
#include "util/result.h"

// Finds the mics in an OBS scene collection and prepares one for libobs's
// loader. The collection is parsed by libobs itself (obs_data), as OBS parses
// it, so what reaches the loader is what OBS would load.
namespace knobs::import {

// How OBS loads a mic depends on where the collection keeps it.
enum class MicOrigin {
  // A global audio device from Settings > Audio, saved under a top-level key
  // such as "AuxAudioDevice1". OBS loads these with obs_load_source alone
  // (OBSBasic_SceneCollections.cpp, LoadAudioDevice).
  kGlobalDevice,
  // An entry of "sources". obs_load_sources loads each and then runs
  // obs_source_load2 on it, which calls the source's and its filters' load
  // callbacks.
  kSource,
};

struct MicCandidate {
  std::string name;
  // Where it's saved: "AuxAudioDevice1", or "sources[3]".
  std::string location;
  MicOrigin origin = MicOrigin::kSource;
  // The top-level key, or the index in "sources".
  std::string key;
  size_t index = 0;
  // win-wasapi's device setting: an endpoint ID, or "default".
  std::string device_id;
  // Whether OBS monitors it.
  bool monitored = false;
  // The filters that run, by name, in order: as ImportedMic::filters will
  // list them once this mic is imported.
  std::vector<std::string> filters;

  friend bool operator==(const MicCandidate&, const MicCandidate&) = default;
};

// Picks the mic that `query` names: its number in `mics`, counting from 1,
// or its name, ignoring ASCII case. An empty query picks the only mic.
// Returns its index in `mics`. The tools' --pick.
Result<size_t> PickMic(const std::vector<MicCandidate>& mics, std::string_view query);

// Picks the mic named `name`, as the tray saves a pick (core::Settings::mic):
// a mic named "2" is that mic, never the second one. The exact name comes
// first, then one that differs only in ASCII case. An empty name picks the
// only mic. Returns its index in `mics`.
Result<size_t> PickMicByName(const std::vector<MicCandidate>& mics, std::string_view name);

// "1. "Mic/Aux" (AuxAudioDevice1, monitored)"
std::string DescribeMic(const MicCandidate& mic, size_t number);

struct ImportNote {
  // A warning means knobs's cable won't sound like OBS's; a note is
  // information only.
  bool warning = false;
  std::string text;
  // A note that changes what to expect all the same, such as push-to-talk
  // not silencing the cable. The first run shows these with the warnings.
  bool changes_expectations = false;
  // What a filter's note is about, which renaming in OBS doesn't change: the
  // kind of note and the filter's uuid. Notifications tell a new warning from
  // a renamed filter's by it. Empty for the source's own notes.
  std::string key;

  friend bool operator==(const ImportNote&, const ImportNote&) = default;
};

struct ImportedMic {
  MicCandidate mic;
  // The mic's source object for libobs's loader: as OBS saved it, minus the
  // filters pre-flight removed. Serialized by libobs, the way OBS saves it.
  std::string source_json;
  std::vector<ImportNote> notes;
  // The filters that run, by name, in processing order: the chain as OBS
  // shows it, minus filters that are off and placeholders for types libobs
  // doesn't have (a placeholder passes audio through, and pre-flight warns
  // about it).
  std::vector<std::string> filters;
  // source_json without the keys that don't change what reaches the cable:
  // libobs's monitor ignores mute, push-to-talk and push-to-mute, the source's
  // enabled flag and its sync offset (docs/design.md, Mute and push-to-talk),
  // knobs sets monitoring itself, its private source registers no hotkeys, the
  // names of the mic and its filters only label them, and the rest is the
  // output mix, video or OBS's own bookkeeping, filters' UUIDs included.
  // Imports with the same key load the same chain.
  std::string chain_key;

  // Whether loading should run obs_source_load2 afterwards, as OBS does.
  bool load_callbacks() const { return mic.origin == MicOrigin::kSource; }
  size_t warning_count() const;
};

// A scene collection as libobs parses it.
class SceneCollection {
 public:
  // Reads `file`, or `file`.bak if `file` doesn't parse. OBS falls back the
  // same way, but also renames the backup over the broken file
  // (obs_data_create_from_json_file_safe); knobs only reads.
  static Result<std::unique_ptr<SceneCollection>> Read(const runtime::ObsApi& api,
                                                       const std::filesystem::path& file);
  ~SceneCollection();
  SceneCollection(const SceneCollection&) = delete;
  SceneCollection& operator=(const SceneCollection&) = delete;

  // The file that was read: `file` or its backup.
  const std::filesystem::path& file() const { return file_; }
  bool from_backup() const { return from_backup_; }

  // The name it saves, or "" (see FindSceneCollectionFile).
  std::string name() const;

  // Every win-wasapi input: the global devices first, then "sources" in
  // order.
  std::vector<MicCandidate> Mics() const;

  // Runs the pre-flight checks on `mic` and returns it ready to load. Needs
  // libobs started with its modules, to tell which filters it has.
  Result<ImportedMic> Import(const MicCandidate& mic) const;

 private:
  SceneCollection(const runtime::ObsApi& api, obs_data_t* data) : api_(api), data_(data) {}

  const runtime::ObsApi& api_;
  obs_data_t* data_;
  std::filesystem::path file_;
  bool from_backup_ = false;
};

// Reads the active scene collection: the file FindSceneCollectionFile picks,
// or its backup. Each file is parsed once along the way.
Result<std::unique_ptr<SceneCollection>> ReadActiveCollection(const runtime::ObsApi& api,
                                                              const ActiveObsConfig& config);

}  // namespace knobs::import
