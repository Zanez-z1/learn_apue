if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "deployment source directory is required")
endif()

file(READ "${SOURCE_DIR}/deploy/systemd/rk-media-gateway.service" gateway_unit)
file(READ "${SOURCE_DIR}/deploy/systemd/mediamtx.service" mediamtx_unit)
file(READ "${SOURCE_DIR}/deploy/systemd/rk-media-gateway.tmpfiles.conf" tmpfiles)
file(READ "${SOURCE_DIR}/deploy/systemd/gateway.env.example" environment)
file(READ "${SOURCE_DIR}/README.md" readme)
file(READ "${SOURCE_DIR}/docs/user-manual.md" user_manual)
set(project_guides "${readme}\n${user_manual}")
file(READ "${SOURCE_DIR}/web/diagnostic.html" diagnostic_page)
file(READ "${SOURCE_DIR}/config/mediamtx.pc-source.yml" pc_source_config)
file(READ "${SOURCE_DIR}/scripts/run_pc_camera_source.sh" pc_source_runner)

foreach(required IN ITEMS
        "User=rk-media-gateway"
        "EnvironmentFile=/etc/rk-media-gateway/gateway.env"
        "ExecReload=/bin/kill -HUP $MAINPID"
        "Restart=on-failure"
        "TimeoutStopSec=20s"
        "KillSignal=SIGTERM"
        "KillMode=control-group"
        "NoNewPrivileges=yes"
        "ProtectSystem=strict"
        "ProtectClock=yes"
        "DeviceAllow=/dev/mpp_service rw"
        "DeviceAllow=/dev/rga rw"
        "DeviceAllow=/dev/dma_heap/system rw"
        "DeviceAllow=/dev/dri/renderD128 rw"
        "ReadWritePaths=/var/lib/rk-media-gateway")
    string(FIND "${gateway_unit}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "gateway unit is missing '${required}'")
    endif()
endforeach()

foreach(required IN ITEMS
        "gatewayd"
        "FFmpeg-Rockchip"
        "MediaMTX"
        "/v1/health"
        "/v1/channels/cam01"
        "/v1/channels/cam01/metrics"
        "/view/cam01"
        "/v1/recording"
        "不包含音频")
    string(FIND "${project_guides}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "project guides are missing '${required}'")
    endif()
endforeach()

foreach(required IN ITEMS
        "MediaMTX WebRTC video"
        "textContent"
        "setInterval(refresh, 1000)"
        "normalizeMediaHost"
        "media_host"
        "unavailable"
        "/metrics")
    string(FIND "${diagnostic_page}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "diagnostic page is missing '${required}'")
    endif()
endforeach()

foreach(required IN ITEMS
        "rtspAddress: ':8554'"
        "rtspTransports: [tcp]"
        "webrtc: false"
        "paths:"
        "source:")
    string(FIND "${pc_source_config}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "PC MediaMTX config is missing '${required}'")
    endif()
endforeach()

foreach(required IN ITEMS
        "--board-ip"
        "/dev/video0"
        "MJPEG 1280x720 at 30 fps"
        "rtsp://127.0.0.1:8554/source"
        "MediaMTX binary and config must use persistent paths outside /tmp")
    string(FIND "${pc_source_runner}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "PC source runner is missing '${required}'")
    endif()
endforeach()

foreach(forbidden IN ITEMS "pkill" "killall")
    string(FIND "${pc_source_runner}" "${forbidden}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "PC source runner contains broad termination '${forbidden}'")
    endif()
endforeach()

foreach(forbidden IN ITEMS "innerHTML" "<script src=" "https://" "rtsp://")
    string(FIND "${diagnostic_page}" "${forbidden}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "diagnostic page contains forbidden text '${forbidden}'")
    endif()
endforeach()

foreach(required IN ITEMS
        "User=rk-media-gateway"
        "ExecStart=/usr/local/bin/mediamtx /etc/rk-media-gateway/mediamtx.yml"
        "Restart=on-failure"
        "KillMode=control-group"
        "ProtectSystem=strict"
        "ReadWritePaths=/var/lib/rk-media-gateway")
    string(FIND "${mediamtx_unit}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "MediaMTX unit is missing '${required}'")
    endif()
endforeach()

string(FIND "${tmpfiles}" "/var/lib/rk-media-gateway/recordings" position)
if(position EQUAL -1)
    message(FATAL_ERROR "tmpfiles config does not create the recording directory")
endif()

foreach(forbidden IN ITEMS "Password=" "fixture-password" "http-password")
    string(FIND "${gateway_unit}${mediamtx_unit}${tmpfiles}${environment}${demo}${diagnostic_page}"
           "${forbidden}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "deployment files contain forbidden text '${forbidden}'")
    endif()
endforeach()
