if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "deployment source directory is required")
endif()

file(READ "${SOURCE_DIR}/deploy/systemd/rk-media-gateway.service" gateway_unit)
file(READ "${SOURCE_DIR}/deploy/systemd/mediamtx.service" mediamtx_unit)
file(READ "${SOURCE_DIR}/deploy/systemd/rk-media-gateway.tmpfiles.conf" tmpfiles)
file(READ "${SOURCE_DIR}/deploy/systemd/gateway.env.example" environment)

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
        "ReadWritePaths=/var/lib/rk-media-gateway")
    string(FIND "${gateway_unit}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "gateway unit is missing '${required}'")
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
    string(FIND "${gateway_unit}${mediamtx_unit}${tmpfiles}${environment}"
           "${forbidden}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "deployment files contain forbidden text '${forbidden}'")
    endif()
endforeach()
