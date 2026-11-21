file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND "${MAM}" --rack "${RACK}" --schema-strict --wav "${WORK}/must-not-exist.wav"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 10)
if(NOT result STREQUAL "1" OR NOT errors MATCHES "Full JSON Schema validation is unavailable")
    message(FATAL_ERROR "Strict schema must fail explicitly: ${result}\n${output}\n${errors}")
endif()
if(EXISTS "${WORK}/must-not-exist.wav")
    message(FATAL_ERROR "Strict schema rejection must not create audio")
endif()
