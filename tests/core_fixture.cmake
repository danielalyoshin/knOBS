# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runs knobs-core against tests/fixtures/obs-config, a made-up OBS settings
# folder, and checks the states it reports. A dry run that doesn't watch for
# OBS: no audio device is opened, and OBS running alongside doesn't change the
# outcome. Needs OBS installed; prints SKIP when it isn't, which the test's
# SKIP_REGULAR_EXPRESSION turns into a skip.
#
#   cmake -DTOOL=<knobs-core.exe> -DCONFIG=<tests/fixtures/obs-config> -P core_fixture.cmake

function(run_core out_var code_var)
  execute_process(
    COMMAND "${TOOL}" --obs-config "${CONFIG}" --ignore-obs --seconds 2 ${ARGN}
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

# Two mics and no --pick: the core needs the user to pick one, and loads
# nothing.
run_core(output code)
if(code EQUAL 77)
  message("SKIP (OBS isn't installed)")
  return()
endif()
message("${output}")
if(NOT code EQUAL 0)
  message(FATAL_ERROR "knobs-core failed.")
endif()
expect("${output}" "s] starting")
expect("${output}" "s] needs setup: The scene collection has 2 mics. Choose one:")
expect("${output}" "1. \"Mic/Aux\" (AuxAudioDevice1, monitored)")
expect("${output}" "0 leaked allocations")

# Mic/Aux: its device and the profile's cable are both made up, so neither is
# connected. The cable comes first, under the name the profile saved.
run_core(output code --pick 1)
message("${output}")
if(NOT code EQUAL 0)
  message(FATAL_ERROR "knobs-core --pick 1 failed.")
endif()
expect("${output}" "s] cable missing: \"CABLE Input (VB-Audio Virtual Cable)\" isn't connected.")
expect("${output}" "0 leaked allocations")
