# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runs knobs-import against tests/fixtures/obs-config, a made-up OBS settings
# folder, and checks what it reports. Needs OBS installed; prints SKIP when
# it isn't, which the test's SKIP_REGULAR_EXPRESSION turns into a skip.
#
#   cmake -DTOOL=<knobs-import.exe> -DCONFIG=<tests/fixtures/obs-config> -P import_fixture.cmake

function(run_import out_var code_var)
  execute_process(
    COMMAND "${TOOL}" --obs-config "${CONFIG}" ${ARGN}
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output
    RESULT_VARIABLE code)
  set(${out_var} "${output}" PARENT_SCOPE)
  set(${code_var} "${code}" PARENT_SCOPE)
endfunction()

function(expect output text)
  string(FIND "${output}" "${text}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "Expected \"${text}\" in:\n${output}")
  endif()
endfunction()

function(expect_not output text)
  string(FIND "${output}" "${text}" at)
  if(NOT at EQUAL -1)
    message(FATAL_ERROR "Didn't expect \"${text}\" in:\n${output}")
  endif()
endfunction()

# Two mics and no --pick: lists them and fails.
run_import(output code)
if(code EQUAL 77)
  message("SKIP (OBS isn't installed)")
  return()
endif()
message("${output}")
if(code EQUAL 0)
  message(FATAL_ERROR "Importing without --pick should fail when there are two mics.")
endif()
# The profile and collection go by the names saved in them, not by folder or
# file names, and [Locations] folders that don't exist fall back.
expect("${output}" "\"Streaming Voice\": 44100 Hz, Mono")
expect("${output}" "\"Main Scenes\":")
expect("${output}" "Main_Scenes.json")
expect("${output}" "1. \"Mic/Aux\" (AuxAudioDevice1, monitored)")
expect("${output}" "2. \"Podcast Mic\" (sources[1])")
expect("${output}" "Pass --pick")

# The mic from "sources": every pre-flight check fires.
run_import(output code --pick "podcast mic")
message("${output}")
if(NOT code EQUAL 0)
  message(FATAL_ERROR "Importing \"Podcast Mic\" failed.")
endif()
expect("${output}" "[warn] pre-flight         Filter \"VST\" is a VST plugin (ReaComp-standalone.dll)")
expect("${output}" "[warn] pre-flight         Filter \"NVIDIA Noise Removal\" is NVIDIA's noise removal")
expect("${output}" "[warn] pre-flight         Compressor \"Ducking\" turns the mic down under \"Music\" in OBS.")
expect("${output}" " loads only the mic, so it keeps only this compressor's output gain (+2.5 dB), and the mic sounds as it does in OBS while nothing plays on \"Music\".")
expect("${output}" "[--  ] pre-flight         Filter \"Gate\" is off in OBS, and stays off.")
expect("${output}" "[--  ] pre-flight         Filter \"Third-Party\" (some_plugin_filter) isn't one ")
expect("${output}" " has, but it's off in OBS anyway.")
expect("${output}" "[--  ] pre-flight         Filter \"Old VST\", a VST plugin, is off in OBS.")
expect("${output}" "The mic is muted and on push-to-talk in OBS.")
expect("${output}" "sync offset of 50 ms")
expect("${output}" "OBS doesn't monitor this mic")
# Both VST filters are gone, and the compressor with a sidechain is a Gain
# filter at its output gain, in its place; the rest load in order, with
# source-level state.
expect("${output}" "balance 0.30, Mono off, then 5 filter(s), then volume 0.50")
expect("${output}" "1. nvidia_audiofx_filter \"NVIDIA Noise Removal\" (unknown to libobs")
expect("${output}" "2. gain_filter \"Ducking\"")
expect("${output}" "5. gain_filter \"Makeup\"")
expect_not("${output}" "vst_filter")
expect_not("${output}" "compressor_filter")
expect("${output}" "libobs loaded the chain and ran its load callbacks; 3 warning(s)")

# The global Mic/Aux device: no load callbacks, nothing to warn about.
run_import(output code --pick 1)
message("${output}")
if(NOT code EQUAL 0)
  message(FATAL_ERROR "Importing \"Mic/Aux\" failed.")
endif()
expect("${output}" "[ok  ] pre-flight         nothing to report")
expect("${output}" "balance 0.50, Mono on, then 2 filter(s), then volume 1.00")
expect("${output}" "1. noise_suppress_filter_v2 \"Noise Suppression\"")
expect("${output}" "libobs loaded the chain; 0 warning(s)")
