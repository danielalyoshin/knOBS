// SPDX-License-Identifier: GPL-2.0-or-later
#include "import/mic_import.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>

#include "app_info.h"
#include "util/text_file.h"
#include "util/win_strings.h"

namespace knobs::import {
namespace {

namespace fs = std::filesystem;

// win-wasapi's input source.
constexpr std::string_view kMicSourceId = "wasapi_input_capture";
// The global audio devices, in the order OBS loads them (OBSBasic::LoadData).
constexpr std::array<const char*, 6> kGlobalDeviceKeys = {"DesktopAudioDevice1", "DesktopAudioDevice2",
                                                          "AuxAudioDevice1",     "AuxAudioDevice2",
                                                          "AuxAudioDevice3",     "AuxAudioDevice4"};
constexpr std::string_view kVstFilterId = "vst_filter";
constexpr std::string_view kNvidiaFilterId = "nvidia_audiofx_filter";
constexpr std::string_view kCompressorId = "compressor_filter";
constexpr std::string_view kGainId = "gain_filter";
// What ImportedMic::chain_key leaves out of a saved source (obs.c,
// obs_save_source).
constexpr std::array<const char*, 17> kKeysTheCableIgnores = {
    // The monitor ignores these (M1 findings), in OBS too.
    "muted", "push-to-mute", "push-to-mute-delay", "push-to-talk", "push-to-talk-delay", "enabled", "sync",
    // knobs sets these itself.
    "monitoring_type", "monitoring_enabled", "hotkeys",
    // The output mix, video, and OBS's own bookkeeping.
    "mixers", "deinterlace_mode", "deinterlace_field_order", "uuid", "canvas_uuid", "private_settings",
    // Only libobs's log uses it.
    "name"};
// What it leaves out of each filter: the same name and UUID. A filter's
// "enabled" stays, since it turns the filter off.
constexpr std::array<const char*, 2> kFilterKeysTheCableIgnores = {"name", "uuid"};

struct DataReleaser {
  const runtime::ObsApi* api;
  void operator()(obs_data_t* data) const { api->obs_data_release(data); }
};
using DataPtr = std::unique_ptr<obs_data_t, DataReleaser>;

struct ArrayReleaser {
  const runtime::ObsApi* api;
  void operator()(obs_data_array_t* array) const { api->obs_data_array_release(array); }
};
using ArrayPtr = std::unique_ptr<obs_data_array_t, ArrayReleaser>;

DataPtr Own(const runtime::ObsApi& api, obs_data_t* data) { return DataPtr(data, DataReleaser{&api}); }
ArrayPtr Own(const runtime::ObsApi& api, obs_data_array_t* array) { return ArrayPtr(array, ArrayReleaser{&api}); }

std::string String(const runtime::ObsApi& api, obs_data_t* data, const char* name) {
  const char* value = api.obs_data_get_string(data, name);
  return value ? value : "";
}

// libobs creates a source by its versioned type, falling back to its type
// (obs_load_source_type).
std::string VersionedId(const runtime::ObsApi& api, obs_data_t* data) {
  std::string id = String(api, data, "versioned_id");
  return id.empty() ? String(api, data, "id") : id;
}

// A key the loader defaults when it's missing (obs_load_source_type).
bool Bool(const runtime::ObsApi& api, obs_data_t* data, const char* name, bool missing) {
  return api.obs_data_has_user_value(data, name) ? api.obs_data_get_bool(data, name) : missing;
}

// A compressor that ducks under another source: the only filter in
// obs-filters 32.2.2 with a sidechain.
struct Ducking {
  std::string sidechain;   // The source it follows.
  double output_gain = 0;  // In dB, as saved.
};

std::optional<Ducking> AsDucking(const runtime::ObsApi& api, obs_data_t* filter) {
  if (String(api, filter, "id") != kCompressorId) return std::nullopt;
  DataPtr settings = Own(api, api.obs_data_get_obj(filter, "settings"));
  const std::string sidechain = settings ? String(api, settings.get(), "sidechain_source") : "";
  if (sidechain.empty() || sidechain == "none") return std::nullopt;
  return Ducking{sidechain, api.obs_data_get_double(settings.get(), "output_gain")};
}

// What OBS's compressor does while nothing plays on its sidechain: OBS's Gain
// filter at the compressor's output gain, in its place.
//
// With a sidechain, the compressor follows the sidechain's level instead of
// the mic's; without one, the mic's (compressor-filter.c,
// compressor_filter_audio). knobs loads only the mic, so as saved it would
// compress the mic by its own level. While the sidechain is below the
// threshold, silence included, the compressor's gain is exactly 1, and each
// sample is multiplied by db_to_mul((float)output_gain), which is what the
// Gain filter does with its "db" (gain-filter.c): the same audio, bit for bit.
DataPtr GainInsteadOf(const runtime::ObsApi& api, obs_data_t* compressor, double db) {
  // A copy keeps the filter's name, its enabled flag and the rest the loader
  // reads; only its type and settings change.
  const char* json = api.obs_data_get_json(compressor);
  DataPtr gain = Own(api, json ? api.obs_data_create_from_json(json) : nullptr);
  if (!gain) return gain;
  api.obs_data_set_string(gain.get(), "id", kGainId.data());
  api.obs_data_set_string(gain.get(), "versioned_id", kGainId.data());
  DataPtr settings = Own(api, api.obs_data_create());
  api.obs_data_set_double(settings.get(), "db", db);
  api.obs_data_set_obj(gain.get(), "settings", settings.get());
  return gain;
}

// Whether a filter is part of the chain anyone hears: it's on, libobs has its
// type, and pre-flight keeps it as something that changes the audio. For a
// type it doesn't have, such as NVIDIA's noise removal from nv-filters or a
// VST plugin from obs-vst, libobs loads a placeholder that passes audio
// through untouched. A compressor with a sidechain runs only as its output
// gain (GainInsteadOf).
bool Runs(const runtime::ObsApi& api, obs_data_t* filter) {
  if (!Bool(api, filter, "enabled", true) || String(api, filter, "id") == kVstFilterId ||
      api.obs_get_source_output_flags(VersionedId(api, filter).c_str()) == 0) {
    return false;
  }
  const auto ducking = AsDucking(api, filter);
  return !ducking || ducking->output_gain != 0;
}

// `file` parsed by libobs as its JSON reader takes it: os_quick_read_utf8_file
// skips a BOM. Null if it doesn't parse.
obs_data_t* ParseJsonFile(const runtime::ObsApi& api, const fs::path& file) {
  auto text = ReadText(file);
  if (!text) return nullptr;
  if (text->starts_with("\xEF\xBB\xBF")) text->erase(0, 3);
  return api.obs_data_create_from_json(text->c_str());
}

// `file` parsed by libobs, else its .bak, as obs_data_create_from_json_file_safe
// would read them. Null if neither parses.
obs_data_t* ParseCollection(const runtime::ObsApi& api, const fs::path& file, bool* from_backup) {
  *from_backup = false;
  if (obs_data_t* data = ParseJsonFile(api, file)) return data;
  obs_data_t* backup = ParseJsonFile(api, fs::path(file) += L".bak");
  *from_backup = backup != nullptr;
  return backup;
}

std::optional<MicCandidate> AsMic(const runtime::ObsApi& api, obs_data_t* source) {
  if (String(api, source, "id") != kMicSourceId) return std::nullopt;
  MicCandidate mic;
  mic.name = String(api, source, "name");
  DataPtr settings = Own(api, api.obs_data_get_obj(source, "settings"));
  mic.device_id = settings ? String(api, settings.get(), "device_id") : "";
  if (mic.device_id.empty()) mic.device_id = "default";  // win-wasapi's default.
  // OBS 33 saves monitoring on or off as its own key (plan.md, OBS 33.0 notes).
  mic.monitored = api.obs_data_get_int(source, "monitoring_type") != OBS_MONITORING_TYPE_NONE ||
                  api.obs_data_get_bool(source, "monitoring_enabled");
  ArrayPtr filters = Own(api, api.obs_data_get_array(source, "filters"));
  const size_t count = filters ? api.obs_data_array_count(filters.get()) : 0;
  for (size_t i = 0; i < count; ++i) {
    DataPtr filter = Own(api, api.obs_data_array_item(filters.get(), i));
    if (Runs(api, filter.get())) mic.filters.push_back(String(api, filter.get(), "name"));
  }
  return mic;
}

std::string FileName(std::string_view path) {
  const size_t slash = path.find_last_of("/\\");
  return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

// One filter's pre-flight. Returns whether to keep it, and sets `instead` to
// a filter to load in its place, if any.
bool CheckFilter(const runtime::ObsApi& api, obs_data_t* filter, std::vector<ImportNote>& notes, DataPtr& instead) {
  const std::string name = String(api, filter, "name");
  const std::string id = String(api, filter, "id");
  const std::string type = VersionedId(api, filter);
  const bool enabled = Bool(api, filter, "enabled", true);
  DataPtr settings = Own(api, api.obs_data_get_obj(filter, "settings"));

  if (id == kVstFilterId) {
    const std::string plugin = settings ? FileName(String(api, settings.get(), "plugin_path")) : "";
    notes.push_back({enabled, enabled ? std::format("Filter \"{}\" is a VST plugin ({}). {} can't run VST "
                                                    "plugins yet, so it leaves the filter out.",
                                                    name, plugin.empty() ? "none chosen" : plugin, kDisplayName)
                                      : std::format("Filter \"{}\", a VST plugin, is off in OBS. {} leaves it "
                                                    "out.",
                                                    name, kDisplayName)});
    return false;
  }
  if (api.obs_get_source_output_flags(type.c_str()) == 0) {
    // libobs knows no such type, so it loads a placeholder that does nothing.
    if (!enabled) {
      notes.push_back({false, std::format("Filter \"{}\" ({}) isn't one {} has, but it's off in OBS anyway.",
                                          name, type, kDisplayName)});
    } else if (id == kNvidiaFilterId) {
      notes.push_back({true, std::format("Filter \"{}\" is NVIDIA's noise removal, from OBS's nv-filters "
                                         "module, which {} doesn't load. It passes audio through untouched.",
                                         name, kDisplayName)});
    } else {
      notes.push_back({true, std::format("Filter \"{}\" ({}) isn't one {} has. It passes audio through "
                                         "untouched.",
                                         name, type, kDisplayName)});
    }
    return true;
  }
  if (!enabled) {
    notes.push_back({false, std::format("Filter \"{}\" is off in OBS, and stays off.", name)});
    return true;
  }
  if (const auto ducking = AsDucking(api, filter)) {
    // As OBS while nothing plays on the sidechain (GainInsteadOf). At 0 dB
    // that's no change at all.
    const std::string what = std::format("Compressor \"{}\" turns the mic down under \"{}\" in OBS.", name,
                                         ducking->sidechain);
    const std::string then = std::format("and the mic sounds as it does in OBS while nothing plays on \"{}\".",
                                         ducking->sidechain);
    if (ducking->output_gain != 0) instead = GainInsteadOf(api, filter, ducking->output_gain);
    if (instead) {
      notes.push_back({true, std::format("{} {} loads only the mic, so it keeps only this compressor's output gain "
                                         "({:+.1f} dB), {}",
                                         what, kDisplayName, ducking->output_gain, then)});
      return true;
    }
    // At 0 dB, or if libobs couldn't copy the filter.
    notes.push_back({true, ducking->output_gain == 0
                               ? std::format("{} {} loads only the mic, so it leaves this compressor out, {}", what,
                                             kDisplayName, then)
                               : std::format("{} {} loads only the mic, so it leaves this compressor out, and its "
                                             "output gain ({:+.1f} dB) with it.",
                                             what, kDisplayName, ducking->output_gain)});
    return false;
  }
  return true;
}

// Source-level state the monitor ignores, which a user might expect to
// matter.
void CheckSourceState(const runtime::ObsApi& api, obs_data_t* source, const MicCandidate& mic,
                      std::vector<ImportNote>& notes) {
  std::vector<std::string_view> silencers;
  if (!Bool(api, source, "enabled", true)) silencers.push_back("disabled");
  if (Bool(api, source, "muted", false)) silencers.push_back("muted");
  if (Bool(api, source, "push-to-talk", false)) silencers.push_back("on push-to-talk");
  if (Bool(api, source, "push-to-mute", false)) silencers.push_back("on push-to-mute");
  if (!silencers.empty()) {
    std::string states;
    for (size_t i = 0; i < silencers.size(); ++i) {
      states += std::format("{}{}", i == 0 ? "" : i + 1 == silencers.size() ? " and " : ", ", silencers[i]);
    }
    // obs_source_t's enabled flag counts as muted (obs-source.c).
    notes.push_back({.text = std::format("The mic is {} in OBS. libobs's monitor ignores mute, push-to-talk and "
                                         "push-to-mute (OBS 32.2 and later), so the cable gets the mic either way, "
                                         "from OBS and from {}.",
                                         states, kDisplayName),
                     .changes_expectations = true});
  }
  if (const int64_t sync = api.obs_data_get_int(source, "sync"); sync != 0) {
    notes.push_back({false, std::format("The mic has a sync offset of {} ms. libobs's monitor ignores it for "
                                        "audio-only sources, in OBS too.",
                                        sync / 1'000'000)});
  }
  if (!mic.monitored) {
    notes.push_back(
        {false, std::format("OBS doesn't monitor this mic, so it doesn't send it to a cable. {} will.", kDisplayName)});
  }
}

// The mics, numbered, one to a line, for an error that asks to choose one.
std::string MicList(const std::vector<MicCandidate>& mics) {
  std::string list;
  for (size_t i = 0; i < mics.size(); ++i) list += std::format("\n  {}", DescribeMic(mics[i], i + 1));
  return list;
}

// The mics whose name is `name`, ignoring ASCII case.
std::vector<size_t> MicsNamed(const std::vector<MicCandidate>& mics, std::string_view name) {
  const std::string needle = AsciiLower(name);
  std::vector<size_t> matches;
  for (size_t i = 0; i < mics.size(); ++i) {
    if (AsciiLower(mics[i].name) == needle) matches.push_back(i);
  }
  return matches;
}

// What an empty pick picks: the only mic.
Result<size_t> OnlyMic(const std::vector<MicCandidate>& mics) {
  if (mics.empty()) return Error{"The scene collection has no mic (win-wasapi input) sources."};
  if (mics.size() == 1) return size_t{0};
  return Error{std::format("The scene collection has {} mics. Choose one:{}", mics.size(), MicList(mics))};
}

}  // namespace

Result<size_t> PickMic(const std::vector<MicCandidate>& mics, std::string_view query) {
  if (mics.empty() || query.empty()) return OnlyMic(mics);
  size_t number = 0;
  const auto [end, ec] = std::from_chars(query.data(), query.data() + query.size(), number);
  if (ec == std::errc() && end == query.data() + query.size()) {
    if (number >= 1 && number <= mics.size()) return number - 1;
    return Error{std::format("There's no mic {}. Choose one:{}", number, MicList(mics))};
  }
  const std::vector<size_t> matches = MicsNamed(mics, query);
  if (matches.size() == 1) return matches.front();
  return Error{std::format("{} named \"{}\". Choose by number:{}",
                           matches.empty() ? "There's no mic" : "Several mics are", query, MicList(mics))};
}

Result<size_t> PickMicByName(const std::vector<MicCandidate>& mics, std::string_view name) {
  if (mics.empty() || name.empty()) return OnlyMic(mics);
  for (size_t i = 0; i < mics.size(); ++i) {
    if (mics[i].name == name) return i;
  }
  const std::vector<size_t> matches = MicsNamed(mics, name);
  if (matches.size() == 1) return matches.front();
  return Error{std::format("{} named \"{}\". Choose one:{}", matches.empty() ? "There's no mic" : "Several mics are",
                           name, MicList(mics))};
}

std::string DescribeMic(const MicCandidate& mic, size_t number) {
  return std::format("{}. \"{}\" ({}{})", number, mic.name, mic.location, mic.monitored ? ", monitored" : "");
}

size_t ImportedMic::warning_count() const {
  return static_cast<size_t>(std::count_if(notes.begin(), notes.end(), [](const ImportNote& n) { return n.warning; }));
}

Result<std::unique_ptr<SceneCollection>> SceneCollection::Read(const runtime::ObsApi& api, const fs::path& file) {
  bool from_backup = false;
  obs_data_t* data = ParseCollection(api, file, &from_backup);
  if (!data) return Error{std::format("Couldn't read the scene collection {}, or its backup.", ToUtf8(file))};
  std::unique_ptr<SceneCollection> collection(new SceneCollection(api, data));
  collection->file_ = from_backup ? fs::path(file) += L".bak" : file;
  collection->from_backup_ = from_backup;
  return collection;
}

SceneCollection::~SceneCollection() { api_.obs_data_release(data_); }

std::string SceneCollection::name() const { return String(api_, data_, "name"); }

std::vector<MicCandidate> SceneCollection::Mics() const {
  std::vector<MicCandidate> mics;
  for (const char* key : kGlobalDeviceKeys) {
    DataPtr source = Own(api_, api_.obs_data_get_obj(data_, key));
    if (!source) continue;
    if (auto mic = AsMic(api_, source.get())) {
      mic->location = key;
      mic->origin = MicOrigin::kGlobalDevice;
      mic->key = key;
      mics.push_back(std::move(*mic));
    }
  }
  ArrayPtr sources = Own(api_, api_.obs_data_get_array(data_, "sources"));
  const size_t count = sources ? api_.obs_data_array_count(sources.get()) : 0;
  for (size_t i = 0; i < count; ++i) {
    DataPtr source = Own(api_, api_.obs_data_array_item(sources.get(), i));
    if (auto mic = AsMic(api_, source.get())) {
      mic->location = std::format("sources[{}]", i);
      mic->origin = MicOrigin::kSource;
      mic->key = "sources";
      mic->index = i;
      mics.push_back(std::move(*mic));
    }
  }
  return mics;
}

Result<ImportedMic> SceneCollection::Import(const MicCandidate& mic) const {
  DataPtr saved;
  if (mic.origin == MicOrigin::kGlobalDevice) {
    saved = Own(api_, api_.obs_data_get_obj(data_, mic.key.c_str()));
  } else {
    ArrayPtr sources = Own(api_, api_.obs_data_get_array(data_, mic.key.c_str()));
    if (sources && mic.index < api_.obs_data_array_count(sources.get())) {
      saved = Own(api_, api_.obs_data_array_item(sources.get(), mic.index));
    }
  }
  if (!saved || !AsMic(api_, saved.get())) {
    return Error{std::format("The scene collection has no mic at {}.", mic.location)};
  }
  // A copy, so the collection stays as read.
  const char* saved_json = api_.obs_data_get_json(saved.get());
  DataPtr source = Own(api_, saved_json ? api_.obs_data_create_from_json(saved_json) : nullptr);
  if (!source) return Error{std::format("libobs couldn't copy the mic at {}.", mic.location)};

  ImportedMic imported;
  imported.mic = mic;
  ArrayPtr filters = Own(api_, api_.obs_data_get_array(source.get(), "filters"));
  if (filters) {
    ArrayPtr kept = Own(api_, api_.obs_data_array_create());
    const size_t count = api_.obs_data_array_count(filters.get());
    bool changed = false;
    for (size_t i = 0; i < count; ++i) {
      DataPtr filter = Own(api_, api_.obs_data_array_item(filters.get(), i));
      DataPtr instead;
      const bool keep = CheckFilter(api_, filter.get(), imported.notes, instead);
      changed |= !keep || instead;
      if (!keep) continue;
      api_.obs_data_array_push_back(kept.get(), instead ? instead.get() : filter.get());
      if (Runs(api_, filter.get())) imported.filters.push_back(String(api_, filter.get(), "name"));
    }
    if (changed) api_.obs_data_set_array(source.get(), "filters", kept.get());
  }
  CheckSourceState(api_, source.get(), mic, imported.notes);
  const char* json = api_.obs_data_get_json(source.get());
  if (!json) return Error{std::format("libobs couldn't save the mic at {}.", mic.location)};
  imported.source_json = json;
  for (const char* key : kKeysTheCableIgnores) api_.obs_data_erase(source.get(), key);
  ArrayPtr key_filters = Own(api_, api_.obs_data_get_array(source.get(), "filters"));
  const size_t key_filter_count = key_filters ? api_.obs_data_array_count(key_filters.get()) : 0;
  for (size_t i = 0; i < key_filter_count; ++i) {
    DataPtr filter = Own(api_, api_.obs_data_array_item(key_filters.get(), i));
    for (const char* key : kFilterKeysTheCableIgnores) api_.obs_data_erase(filter.get(), key);
  }
  const char* key_json = api_.obs_data_get_json(source.get());
  if (!key_json) return Error{std::format("libobs couldn't save the mic at {}.", mic.location)};
  imported.chain_key = key_json;
  return imported;
}

Result<std::unique_ptr<SceneCollection>> ReadActiveCollection(const runtime::ObsApi& api,
                                                              const ActiveObsConfig& config) {
  // The search stops at the match, so the last file parsed is the one it
  // returns, unless that matched by its file name without parsing.
  std::unique_ptr<SceneCollection> collection;
  fs::path parsed;
  auto file = FindSceneCollectionFile(config, [&](const fs::path& path) -> std::string {
    auto read = SceneCollection::Read(api, path);
    parsed = path;
    collection = read ? std::move(*read) : nullptr;
    return collection ? collection->name() : "";
  });
  if (!file) return Error{file.error()};
  if (collection && parsed == *file) return collection;
  return SceneCollection::Read(api, *file);
}

}  // namespace knobs::import
