if(NOT DEFINED GATEWAY OR NOT DEFINED CONFIG)
    message(FATAL_ERROR "MediaMTX config test arguments are incomplete")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "CAM01_RTSP_URL=rtsp://user:generator-password@camera/live"
            "${GATEWAY}" --config "${CONFIG}" --print-mediamtx-config
    RESULT_VARIABLE gateway_result
    OUTPUT_VARIABLE generated_config
    ERROR_VARIABLE gateway_stderr
)

if(NOT gateway_result EQUAL 0)
    message(FATAL_ERROR "MediaMTX config generation failed: ${gateway_stderr}")
endif()

foreach(required IN ITEMS
        "playback: true"
        "playbackAddress: '127.0.0.1:9996'"
        "record: true"
        "recordFormat: fmp4"
        "recordDeleteAfter: 604800s")
    string(FIND "${generated_config}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
                "generated config is missing '${required}':\n${generated_config}")
    endif()
endforeach()

foreach(forbidden IN ITEMS "generator-password" "rtsp://" "Configuration valid")
    string(FIND "${generated_config}" "${forbidden}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR
                "generated config contains '${forbidden}':\n${generated_config}")
    endif()
endforeach()
