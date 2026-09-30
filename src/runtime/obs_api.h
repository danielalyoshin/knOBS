// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <string_view>
#include <vector>

// Declarations only, from the vendored headers (third_party/libobs). knOBS
// never links obs.lib: calling a libobs function directly instead of through
// ObsApi fails at link time, which is intended.
#include <obs.h>
#include <util/base.h>

namespace knobs::runtime {

// Every libobs export knOBS calls. Add entries here as milestones need them;
// the types come from the vendored headers via decltype.
#define KNOBS_OBS_API(X)                      \
  /* Version */                               \
  X(obs_get_version)                          \
  X(obs_get_version_string)                   \
  /* Memory and logging */                    \
  X(bfree)                                    \
  X(bnum_allocs)                              \
  X(base_set_log_handler)                     \
  /* Lifecycle */                             \
  X(obs_startup)                              \
  X(obs_shutdown)                             \
  X(obs_reset_audio)                          \
  X(obs_reset_video)                          \
  X(obs_queue_task)                           \
  /* Modules and data files */                \
  X(obs_open_module)                          \
  X(obs_init_module)                          \
  X(obs_post_load_modules)                    \
  X(obs_find_module_file)                     \
  X(obs_module_get_locale_string)             \
  X(obs_find_data_file)                       \
  /* Sources */                               \
  X(obs_data_create)                          \
  X(obs_data_release)                         \
  X(obs_data_set_string)                      \
  X(obs_data_set_double)                      \
  X(obs_source_create_private)                \
  X(obs_source_release)                       \
  X(obs_source_filter_add)                    \
  X(obs_source_filter_remove)                 \
  X(obs_source_inc_active)                    \
  X(obs_source_dec_active)                    \
  X(obs_source_add_audio_capture_callback)    \
  X(obs_source_remove_audio_capture_callback) \
  /* Signals */                               \
  X(obs_source_get_signal_handler)            \
  X(signal_handler_connect)                   \
  X(signal_handler_disconnect)

struct ObsApi {
#define KNOBS_DECLARE_OBS_FUNCTION(name) decltype(&::name) name = nullptr;
  KNOBS_OBS_API(KNOBS_DECLARE_OBS_FUNCTION)
#undef KNOBS_DECLARE_OBS_FUNCTION
};

using AnyFunction = void (*)();
// Returns the export with the given name, or null.
using SymbolLookup = std::function<AnyFunction(const char* name)>;

// Fills every entry of `api` from `lookup`. Returns the names that couldn't
// be resolved; the table is only usable if that's empty.
std::vector<std::string_view> ResolveObsApi(ObsApi& api, const SymbolLookup& lookup);

// Number of entries in ObsApi.
size_t ObsApiSize();

}  // namespace knobs::runtime
