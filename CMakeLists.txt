# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.25)
project(knobs VERSION 0.0.1 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT MSVC)
  message(FATAL_ERROR "This project builds with MSVC for 64-bit Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT: the exe then needs no VC++ redistributable beyond the one OBS
# already installed for its own DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(knobs_options INTERFACE)
target_compile_options(knobs_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /external:W0)
target_compile_definitions(knobs_options INTERFACE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# Declarations only. knobs resolves libobs exports at runtime and never links
# obs.lib (see src/runtime/obs_api.h).
add_library(libobs_headers INTERFACE)
target_include_directories(libobs_headers SYSTEM INTERFACE third_party/libobs)

add_library(knobs_runtime STATIC
  src/audio/audio_devices.cpp
  src/audio/device_watch.cpp
  src/audio/live_chain.cpp
  src/core/controller.cpp
  src/core/core.cpp
  src/core/obs_backend.cpp
  src/core/obs_process.cpp
  src/core/state.cpp
  src/import/mic_import.cpp
  src/import/obs_config.cpp
  src/import/obs_ini.cpp
  src/runtime/obs_api.cpp
  src/runtime/obs_host.cpp
  src/runtime/obs_install.cpp
  src/runtime/obs_log.cpp
  src/runtime/obs_runtime.cpp
  src/runtime/obs_session.cpp
  src/runtime/obs_version.cpp
  src/runtime/pe_imports.cpp
  src/runtime/runtime_copy.cpp
  src/util/app_dirs.cpp
  src/util/json.cpp
  src/util/pick_folder.cpp
  src/util/text_file.cpp
  src/util/win_strings.cpp
)
target_include_directories(knobs_runtime PUBLIC src)
target_link_libraries(knobs_runtime
  PUBLIC libobs_headers knobs_options
  PRIVATE version shell32 ole32
)

# The project's version, for the code and knobs.exe's version resource.
configure_file(src/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h @ONLY)

# The tray app's UI, run by knobs.exe, and by knobs-tray on a fake core.
add_library(knobs_tray STATIC
  src/tray/autostart.cpp
  src/tray/first_run.cpp
  src/tray/first_run_dialog.cpp
  src/tray/menu.cpp
  src/tray/menu_theme.cpp
  src/tray/open_obs.cpp
  src/tray/settings_file.cpp
  src/tray/single_instance.cpp
  src/tray/tray_app.cpp
)
target_include_directories(knobs_tray PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(knobs_tray PUBLIC knobs_runtime PRIVATE comctl32 shell32)

# The tray app. The manifest asks for per-monitor DPI awareness (v2) and
# Common Controls 6; the resource compiler finds the icon in assets/.
add_executable(knobs WIN32 src/main.cpp src/tray/knobs.rc src/tray/knobs.manifest)
target_include_directories(knobs PRIVATE assets)
target_link_libraries(knobs PRIVATE knobs_tray ole32 shell32)

# Shared by the dev tools and their unit tests; not part of the app.
add_library(knobs_tools STATIC
  tools/common/audio_diff.cpp
  tools/common/console.cpp
  tools/common/envelope.cpp
  tools/common/obs_import.cpp
  tools/common/offline_chain.cpp
  tools/common/push_source.cpp
  tools/common/sha256.cpp
  tools/common/test_signal.cpp
  tools/common/wav.cpp
  tools/compare/obs_run.cpp
)
target_include_directories(knobs_tools PUBLIC tools)
target_link_libraries(knobs_tools PUBLIC knobs_runtime PRIVATE bcrypt)

add_executable(knobs-smoke tools/smoke/smoke_main.cpp)
target_link_libraries(knobs-smoke PRIVATE knobs_tools psapi)

add_executable(knobs-harness tools/harness/harness_main.cpp)
target_link_libraries(knobs-harness PRIVATE knobs_tools)

add_executable(knobs-live tools/live/live_main.cpp tools/live/endpoints.cpp)
target_link_libraries(knobs-live PRIVATE knobs_tools ole32)

add_executable(knobs-import tools/import/import_main.cpp)
target_link_libraries(knobs-import PRIVATE knobs_tools)

add_executable(knobs-compare tools/compare/compare_main.cpp)
target_link_libraries(knobs-compare PRIVATE knobs_tools)

add_executable(knobs-core tools/core/core_main.cpp)
target_link_libraries(knobs-core PRIVATE knobs_tools ole32 psapi)

add_executable(knobs-tray
  tools/tray/tray_main.cpp
  tools/tray/fake_backend.cpp
  tools/tray/screenshot.cpp
  tools/tray/knobs-tray.rc
  src/tray/knobs.manifest
)
target_include_directories(knobs-tray PRIVATE assets)
target_link_libraries(knobs-tray PRIVATE knobs_tray knobs_tools dwmapi ole32 windowscodecs)

add_executable(knobs-tests tests/unit_tests.cpp tests/core_tests.cpp tests/tray_tests.cpp tests/first_run_tests.cpp)
target_link_libraries(knobs-tests PRIVATE knobs_tools knobs_tray delayimp)
# Gives the test binary a delay-load import for the PE parser tests to find.
target_link_options(knobs-tests PRIVATE /DELAYLOAD:version.dll)

enable_testing()
add_test(NAME unit COMMAND knobs-tests)
# The tests below need OBS installed and skip when it isn't. None of them
# opens the mic or any other audio device, and the import and core tests
# read a made-up OBS settings folder, not the user's.
add_test(NAME smoke COMMAND knobs-smoke)
add_test(NAME harness COMMAND knobs-harness --runs 3)
set_tests_properties(smoke harness PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME import
  COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:knobs-import>
          -DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/obs-config
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/import_fixture.cmake)
set_tests_properties(import PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP .OBS isn't installed.")
add_test(NAME core
  COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:knobs-core>
          -DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/obs-config
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/core_fixture.cmake)
set_tests_properties(core PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP .OBS isn't installed.")
