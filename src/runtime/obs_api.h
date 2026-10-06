// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <string_view>
#include <vector>

// Declarations only, from the vendored headers (third_party/libobs). knobs
// never links obs.lib: calling a libobs function directly instead of through
// ObsApi fails at link time, which is intended.
#include <obs.h>
#include <util/base.h>

namespace knobs::runtime {

// Every libobs export knobs calls. Add entries here as milestones need them;
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
  /* Settings data */                         \
  X(obs_data_create)                          \
  X(obs_data_create_from_json)                \
  X(obs_data_release)                         \
  X(obs_data_get_json)                        \
  X(obs_data_has_user_value)                  \
  X(obs_data_erase)                           \
  X(obs_data_set_string)                      \
  X(obs_data_set_double)                      \
  X(obs_data_set_int)                         \
  X(obs_data_set_bool)                        \
  X(obs_data_set_obj)                         \
  X(obs_data_set_array)                       \
  X(obs_data_set_default_int)                 \
  X(obs_data_get_string)                      \
  X(obs_data_get_int)                         \
  X(obs_data_get_double)                      \
  X(obs_data_get_bool)                        \
  X(obs_data_get_obj)                         \
  X(obs_data_get_array)                       \
  X(obs_data_array_create)                    \
  X(obs_data_array_release)                   \
  X(obs_data_array_count)                     \
  X(obs_data_array_item)                      \
  X(obs_data_array_push_back)                 \
  /* Sources */                               \
  X(obs_register_source_s)                    \
  X(obs_get_source_output_flags)              \
  X(obs_source_create_private)                \
  X(obs_load_private_source)                  \
  X(obs_source_load2)                         \
  X(obs_source_release)                       \
  X(obs_source_get_id)                        \
  X(obs_source_get_name)                      \
  X(obs_source_get_output_flags)              \
  X(obs_source_get_volume)                    \
  X(obs_source_get_balance_value)             \
  X(obs_source_get_flags)                     \
  X(obs_source_enabled)                       \
  X(obs_source_filter_add)                    \
  X(obs_source_filter_remove)                 \
  X(obs_source_enum_filters)                  \
  X(obs_source_inc_active)                    \
  X(obs_source_dec_active)                    \
  X(obs_source_output_audio)                  \
  X(obs_source_add_audio_capture_callback)    \
  X(obs_source_remove_audio_capture_callback) \
  X(obs_get_source_properties)                \
  X(obs_properties_get)                       \
  X(obs_properties_destroy)                   \
  X(obs_property_list_item_count)             \
  X(obs_property_list_item_name)              \
  X(obs_property_list_item_string)            \
  /* Monitoring */                            \
  X(obs_enum_audio_monitoring_devices)        \
  X(obs_set_audio_monitoring_device)          \
  X(obs_source_set_monitoring_type)           \
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
