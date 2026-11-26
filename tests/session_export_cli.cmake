if(NOT DEFINED MAM OR NOT DEFINED INSPECTOR OR NOT DEFINED WORK OR NOT DEFINED CASE)
  message(FATAL_ERROR "MAM, INSPECTOR, WORK and CASE are required")
endif()
file(MAKE_DIRECTORY "${WORK}")
set(output "${WORK}/session.wav")
file(REMOVE "${output}")

# Invalid layouts must fail before opening the deliberately missing rack.
if(CASE MATCHES "^invalid_(session|bus)_([03])$")
  set(scope "${CMAKE_MATCH_1}")
  set(channels "${CMAKE_MATCH_2}")
  if(scope STREQUAL "session")
    set(layout "\"channels\":${channels}")
    set(diagnostic "Offline session export supports only mono or stereo channels")
  else()
    set(layout "\"channels\":2,\"buses\":[{\"id\":\"sum\",\"channels\":${channels}}]")
    set(diagnostic "Offline session export supports only mono or stereo bus channels: sum")
  endif()
  file(WRITE "${WORK}/session.json" "{\"kind\":\"session\",${layout},\"racks\":[{\"id\":\"missing\",\"path\":\"absent-rack.json\"}]}")
  execute_process(COMMAND "${MAM}" --session "${WORK}/session.json" --wav "${output}"
      --duration 0.005 --no-progress --no-summary
      RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 20)
  if(NOT result STREQUAL "1" OR NOT stderr MATCHES "${diagnostic}" OR EXISTS "${output}")
    message(FATAL_ERROR "${CASE}: expected early layout rejection, exit 1 and no output; got ${result}\n${stdout}\n${stderr}")
  endif()
  file(REMOVE_RECURSE "${WORK}")
  return()
endif()

set(channels 1)
set(frames 241)
set(duration 0.005020833333333333)
set(depth 32f)
set(bits 32)
set(loop false)
if(CASE STREQUAL "mono_even")
  set(frames 240)
  set(duration 0.005)
elseif(CASE STREQUAL "stereo_odd" OR CASE STREQUAL "default_stereo")
  set(channels 2)
elseif(CASE STREQUAL "mono_loop")
  set(loop true)
elseif(CASE STREQUAL "stereo_loop")
  set(channels 2)
  set(loop true)
elseif(CASE STREQUAL "pcm16" OR CASE STREQUAL "pcm24")
  string(REPLACE "pcm" "" depth "${CASE}")
  set(bits "${depth}")
elseif(NOT CASE STREQUAL "mono_odd")
  message(FATAL_ERROR "Unknown case: ${CASE}")
endif()

set(layout "\"channels\":${channels},")
if(CASE STREQUAL "default_stereo")
  set(layout "")
endif()
set(routing "")
if(CASE STREQUAL "mono_even" OR CASE STREQUAL "stereo_odd")
  set(routing ",\"buses\":[{\"id\":\"sum\",\"channels\":${channels}}],\"routes\":[{\"from\":\"main\",\"to\":\"sum\",\"gain\":1}]")
endif()
# The last command at frame 17 and zero tail give the legacy CLI planner a
# 17-frame single-loop estimate. A 48-frame requested session duration selects
# three loops; --duration independently fixes their total at 241 frames.
file(WRITE "${WORK}/rack.json" "{\"kind\":\"rack\",\"version\":1,\"sampleRate\":48000,\"channels\":2,\"nodes\":[{\"id\":\"kick\",\"type\":\"kick\",\"params\":{\"click\":0}}],\"commands\":[{\"sampleTime\":0,\"nodeId\":\"kick\",\"type\":\"Trigger\",\"value\":1},{\"sampleTime\":17,\"nodeId\":\"kick\",\"type\":\"Trigger\",\"value\":1}]}")
file(WRITE "${WORK}/session.json" "{\"kind\":\"session\",\"version\":1,${layout}\"sampleRate\":48000,\"loop\":${loop},\"durationSec\":0.001,\"racks\":[{\"id\":\"main\",\"path\":\"rack.json\"}]${routing}}")
set(options --sr 48000 --duration "${duration}" --tail-ms 0 --no-progress --no-summary)
execute_process(COMMAND "${MAM}" --session "${WORK}/session.json" --wav "${output}"
    --bitdepth "${depth}" ${options}
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 20)
if(NOT result STREQUAL "0" OR NOT EXISTS "${output}")
  message(FATAL_ERROR "${CASE}: session export failed (${result})\n${stdout}\n${stderr}")
endif()
if(NOT stderr MATCHES "Exported session to .*[(]frames=${frames},")
  message(FATAL_ERROR "${CASE}: incorrect reported frame count\n${stderr}")
endif()
if(loop AND NOT stderr MATCHES "rendering 3 loops for session")
  message(FATAL_ERROR "${CASE}: did not exercise the repeated-session export path\n${stderr}")
endif()

# The separate graph route provides the kick waveform without session layout or
# repetition. The inspector checks literal frame counts and explicit 81/80/80
# segment placement instead of deriving expectations from SessionRuntime.
set(reference "${WORK}/reference.wav")
file(REMOVE "${reference}")
execute_process(COMMAND "${MAM}" --rack "${WORK}/rack.json" --wav "${reference}"
    --bitdepth 32f ${options}
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 20)
if(NOT result STREQUAL "0")
  message(FATAL_ERROR "${CASE}: graph reference failed (${result})\n${stdout}\n${stderr}")
endif()
execute_process(COMMAND "${INSPECTOR}" "${output}" "${reference}" "${frames}" "${channels}" "${bits}" "${loop}"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 10)
if(NOT result STREQUAL "0")
  message(FATAL_ERROR "${CASE}: audio verification failed (${result})\n${stdout}\n${stderr}")
endif()

file(SHA256 "${output}" before)
execute_process(COMMAND "${MAM}" --session "${WORK}/session.json" --wav "${output}"
    --bitdepth "${depth}" ${options}
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 20)
file(SHA256 "${output}" after)
if(NOT result STREQUAL "1" OR NOT before STREQUAL after)
  message(FATAL_ERROR "${CASE}: overwrite refusal failed (${result})\n${stdout}\n${stderr}")
endif()
file(REMOVE_RECURSE "${WORK}")
