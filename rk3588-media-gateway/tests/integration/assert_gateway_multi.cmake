if(NOT DEFINED GATEWAY OR NOT DEFINED FIXTURE OR NOT DEFINED CONFIG)
    message(FATAL_ERROR "multi-channel test arguments are incomplete")
endif()

execute_process(
    COMMAND "${GATEWAY}" --config "${CONFIG}"
            --exit-when-idle
            --ffmpeg-binary "${FIXTURE}"
    RESULT_VARIABLE gateway_result
    OUTPUT_VARIABLE gateway_stdout
    ERROR_VARIABLE gateway_stderr
)

set(gateway_output "${gateway_stdout}\n${gateway_stderr}")
if(NOT gateway_result EQUAL 0)
    message(FATAL_ERROR "multi-channel gateway failed:\n${gateway_output}")
endif()

foreach(channel_id IN ITEMS cam01 cam02)
    set(expected "channel=${channel_id} state=STOPPED event=clean_exit")
    string(FIND "${gateway_output}" "${expected}" expected_position)
    if(expected_position EQUAL -1)
        message(FATAL_ERROR
                "required output '${expected}' was not found:\n${gateway_output}")
    endif()
endforeach()

string(FIND "${gateway_output}" "fixture-password" password_position)
if(NOT password_position EQUAL -1)
    message(FATAL_ERROR "plaintext password was logged:\n${gateway_output}")
endif()
