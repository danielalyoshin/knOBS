# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.25)
project(knobs VERSION 0.0.1 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT MSVC)
  message(FATAL_ERROR "knOBS v1 builds with MSVC for 64-bit Windows only.")
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

# Declarations only. knOBS resolves libobs exports at runtime and never links
# obs.lib (see src/runtime/obs_api.h).
add_library(libobs_headers INTERFACE)
target_include_directories(libobs_headers SYSTEM INTERFACE third_party/libobs)

add_library(knobs_runtime STATIC
  src/runtime/obs_api.cpp
  src/runtime/obs_install.cpp
  src/runtime/obs_log.cpp
  src/runtime/obs_runtime.cpp
  src/runtime/obs_session.cpp
  src/runtime/obs_version.cpp
  src/runtime/pe_imports.cpp
  src/runtime/runtime_copy.cpp
  src/util/app_dirs.cpp
  src/util/win_strings.cpp
)
target_include_directories(knobs_runtime PUBLIC src)
target_link_libraries(knobs_runtime
  PUBLIC libobs_headers knobs_options
  PRIVATE version shell32 ole32
)

add_executable(knobs-smoke tools/smoke/smoke_main.cpp)
target_link_libraries(knobs-smoke PRIVATE knobs_runtime psapi)

add_executable(knobs-tests tests/unit_tests.cpp)
target_link_libraries(knobs-tests PRIVATE knobs_runtime delayimp)
# Gives the test binary a delay-load import for the PE parser tests to find.
target_link_options(knobs-tests PRIVATE /DELAYLOAD:version.dll)

enable_testing()
add_test(NAME unit COMMAND knobs-tests)
# Needs OBS installed; exits with 77 (skipped) when it isn't. Never opens the
# mic: capture is opt-in.
add_test(NAME smoke COMMAND knobs-smoke)
set_tests_properties(smoke PROPERTIES SKIP_RETURN_CODE 77)
