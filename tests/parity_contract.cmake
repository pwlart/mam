file(MAKE_DIRECTORY "${WORK}")
set(fake [=[#!/usr/bin/env bash
set -eu
wav=''
while [[ $# -gt 0 ]]; do
  if [[ "$1" == --wav ]]; then wav=$2; shift; fi
  shift
done
hash=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
if [[ '@CASE@' != missing_audio ]]; then printf x >"$wav"; fi
case '@CASE@' in
  missing) exit 0 ;;
  malformed) hash=invalid ;;
  mismatch) if [[ "$wav" == *topo-parallel.wav ]]; then hash=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb; fi ;;
  duplicate) printf 'SHA1(samples): %s\n' "$hash" >&2 ;;
esac
printf 'SHA1(samples): %s\n' "$hash" >&2
if [[ '@CASE@' == exit_failure ]]; then exit 9; fi
]=])
string(CONFIGURE "${fake}" fake @ONLY)
file(WRITE "${WORK}/fake-mam.sh" "${fake}")
file(CHMOD "${WORK}/fake-mam.sh" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
execute_process(COMMAND "${BASH}" "${PARITY_SCRIPT}" "${WORK}/fake-mam.sh" unused.json
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 10)
if(CASE STREQUAL "valid")
    if(NOT result STREQUAL "0" OR NOT output MATCHES "OK: parity confirmed")
        message(FATAL_ERROR "Valid parity was rejected: ${result}\n${output}\n${errors}")
    endif()
elseif(NOT result STREQUAL "1" OR NOT errors MATCHES "ERROR:")
    message(FATAL_ERROR "Expected parity rejection for ${CASE}, got ${result}:\n${output}\n${errors}")
endif()
