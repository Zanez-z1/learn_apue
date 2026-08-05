if(NOT DEFINED GATEWAY OR NOT DEFINED FIXTURE OR NOT DEFINED CONFIG OR
   NOT DEFINED MODE OR NOT DEFINED REQUIRED OR NOT DEFINED FORBIDDEN)
    message(FATAL_ERROR "failure test arguments are incomplete")
endif()
if(NOT DEFINED PROBE_MODE)
    set(PROBE_MODE success)
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "GW_FIXTURE_MODE=${MODE}"
            "GW_PROBE_FIXTURE_MODE=${PROBE_MODE}"
            "${GATEWAY}" --config "${CONFIG}"
            --ffprobe-binary "${FIXTURE}" --ffmpeg-binary "${FIXTURE}"
    RESULT_VARIABLE gateway_result
    OUTPUT_VARIABLE gateway_stdout
    ERROR_VARIABLE gateway_stderr
)

set(gateway_output "${gateway_stdout}\n${gateway_stderr}")

if(gateway_result EQUAL 0)
    message(FATAL_ERROR "gateway unexpectedly succeeded:\n${gateway_output}")
endif()

string(FIND "${gateway_output}" "${REQUIRED}" required_position)
if(required_position EQUAL -1)
    message(FATAL_ERROR
            "required output '${REQUIRED}' was not found:\n${gateway_output}")
endif()

string(FIND "${gateway_output}" "${FORBIDDEN}" forbidden_position)
if(NOT forbidden_position EQUAL -1)
    message(FATAL_ERROR
            "forbidden output '${FORBIDDEN}' was found:\n${gateway_output}")
endif()

message(STATUS "expected gateway failure verified:\n${gateway_output}")
