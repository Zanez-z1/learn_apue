cmake_minimum_required(VERSION 3.20)

project(rk_media_gateway VERSION 0.1.0 LANGUAGES C)

option(GATEWAY_BUILD_TESTS "Build unit tests" ON)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LIBYAML REQUIRED IMPORTED_TARGET yaml-0.1)

add_library(gateway_core
    src/api/http_server.c
    src/channel/channel_manager.c
    src/channel/channel_snapshot.c
    src/monitor/process_metrics.c
    src/channel/channel_state.c
    src/channel/supervisor.c
    src/config/config.c
    src/config/mediamtx_config.c
    src/pipeline/pipeline_builder.c
    src/monitor/progress_parser.c
    src/monitor/recording_status.c
    src/process/process_manager.c
    src/probe/probe.c
)

target_include_directories(gateway_core PUBLIC include)
target_link_libraries(gateway_core PUBLIC PkgConfig::LIBYAML Threads::Threads)
target_compile_options(gateway_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

add_executable(gatewayd src/main.c)
target_link_libraries(gatewayd PRIVATE gateway_core)
target_compile_options(gatewayd PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

add_library(gateway_metrics_core src/monitor/process_metrics.c)
target_include_directories(gateway_metrics_core PUBLIC include)
target_compile_options(gateway_metrics_core PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

add_executable(gateway-metrics src/tools/gateway_metrics.c)
target_link_libraries(gateway-metrics PRIVATE gateway_metrics_core)
target_compile_options(gateway-metrics PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

install(TARGETS gatewayd gateway-metrics RUNTIME DESTINATION bin)
install(FILES config/gateway.example.yaml config/mediamtx.example.yml
        DESTINATION share/rk-media-gateway)
install(FILES
        deploy/systemd/rk-media-gateway.service
        deploy/systemd/mediamtx.service
        DESTINATION lib/systemd/system)
install(FILES deploy/systemd/rk-media-gateway.tmpfiles.conf
        DESTINATION lib/tmpfiles.d
        RENAME rk-media-gateway.conf)
install(FILES deploy/systemd/gateway.env.example
        DESTINATION share/rk-media-gateway)
install(FILES docs/deployment.md docs/demo.md
        DESTINATION share/doc/rk-media-gateway)
install(PROGRAMS scripts/check_media_env.sh scripts/run_transcode_benchmark.sh
        scripts/run_capacity_benchmark.sh
        DESTINATION share/rk-media-gateway/scripts)

if(GATEWAY_BUILD_TESTS)
    enable_testing()
    find_program(BASH_PROGRAM bash REQUIRED)
    add_executable(gateway_unit_tests tests/unit/gateway_tests.c)
    target_link_libraries(gateway_unit_tests PRIVATE gateway_core)
    target_compile_options(gateway_unit_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    target_compile_definitions(gateway_unit_tests PRIVATE
        GW_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME gateway_unit_tests COMMAND gateway_unit_tests)
    set_tests_properties(gateway_unit_tests PROPERTIES TIMEOUT 5)

    add_executable(gateway_channel_state_tests tests/unit/channel_state_tests.c)
    target_link_libraries(gateway_channel_state_tests PRIVATE gateway_core)
    target_compile_options(gateway_channel_state_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME gateway_channel_state_tests COMMAND gateway_channel_state_tests)
    set_tests_properties(gateway_channel_state_tests PROPERTIES TIMEOUT 5)

    add_executable(gateway_channel_snapshot_tests
        tests/unit/channel_snapshot_tests.c)
    target_link_libraries(gateway_channel_snapshot_tests PRIVATE gateway_core)
    target_compile_options(gateway_channel_snapshot_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME gateway_channel_snapshot_tests
        COMMAND gateway_channel_snapshot_tests)
    set_tests_properties(gateway_channel_snapshot_tests PROPERTIES TIMEOUT 5)

    add_executable(gateway_probe_tests tests/unit/probe_tests.c)
    target_link_libraries(gateway_probe_tests PRIVATE gateway_core)
    target_compile_options(gateway_probe_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME gateway_probe_tests COMMAND gateway_probe_tests)
    set_tests_properties(gateway_probe_tests PROPERTIES TIMEOUT 5)

    add_executable(gateway_http_server_tests tests/unit/http_server_tests.c)
    target_link_libraries(gateway_http_server_tests PRIVATE gateway_core)
    target_compile_options(gateway_http_server_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME gateway_http_server_tests COMMAND gateway_http_server_tests)
    set_tests_properties(gateway_http_server_tests PROPERTIES TIMEOUT 5)

    add_executable(gateway_mediamtx_config_tests
        tests/unit/mediamtx_config_tests.c)
    target_link_libraries(gateway_mediamtx_config_tests PRIVATE gateway_core)
    target_compile_options(gateway_mediamtx_config_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME gateway_mediamtx_config_tests
        COMMAND gateway_mediamtx_config_tests)
    set_tests_properties(gateway_mediamtx_config_tests PROPERTIES TIMEOUT 5)

    add_executable(gateway_recording_status_tests
        tests/unit/recording_status_tests.c)
    target_link_libraries(gateway_recording_status_tests PRIVATE gateway_core)
    target_compile_options(gateway_recording_status_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME gateway_recording_status_tests
        COMMAND gateway_recording_status_tests)
    set_tests_properties(gateway_recording_status_tests PROPERTIES TIMEOUT 5)

    add_executable(gateway_process_metrics_tests
        tests/unit/process_metrics_tests.c)
    target_link_libraries(gateway_process_metrics_tests PRIVATE gateway_metrics_core)
    target_compile_options(gateway_process_metrics_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME gateway_process_metrics_tests
        COMMAND gateway_process_metrics_tests)
    set_tests_properties(gateway_process_metrics_tests PROPERTIES TIMEOUT 5)

    add_test(NAME gateway_metrics_cli_test
        COMMAND gateway-metrics --target self=self --duration-sec 1 --interval-ms 200)
    set_tests_properties(gateway_metrics_cli_test PROPERTIES
        TIMEOUT 3
        PASS_REGULAR_EXPRESSION
            "elapsed_ms,target,pid,status,cpu_percent,rss_kib,fd_count"
        FAIL_REGULAR_EXPRESSION "unavailable")

    add_test(NAME gateway_metrics_summary_test
        COMMAND ${BASH_PROGRAM}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/metrics_summary_tests.sh
            $<TARGET_FILE:gateway-metrics>)
    set_tests_properties(gateway_metrics_summary_test PROPERTIES TIMEOUT 5)

    add_executable(gateway_benchmark_ffmpeg_fixture
        tests/integration/benchmark_ffmpeg_fixture.c)
    target_compile_options(gateway_benchmark_ffmpeg_fixture PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME gateway_benchmark_runner_tests
        COMMAND ${BASH_PROGRAM}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/benchmark_runner_tests.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_transcode_benchmark.sh
            $<TARGET_FILE:gateway_benchmark_ffmpeg_fixture>)
    set_tests_properties(gateway_benchmark_runner_tests PROPERTIES TIMEOUT 10)

    add_test(NAME gateway_capacity_runner_tests
        COMMAND ${BASH_PROGRAM}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/capacity_runner_tests.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_capacity_benchmark.sh)
    set_tests_properties(gateway_capacity_runner_tests PROPERTIES TIMEOUT 5)

    add_test(NAME gateway_mediamtx_cli_test
        COMMAND ${CMAKE_COMMAND}
            -DGATEWAY=$<TARGET_FILE:gatewayd>
            -DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/config/gateway.example.yaml
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/assert_mediamtx_config.cmake)
    set_tests_properties(gateway_mediamtx_cli_test PROPERTIES TIMEOUT 5)

    add_test(NAME gateway_deployment_tests
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/assert_deployment.cmake)
    set_tests_properties(gateway_deployment_tests PROPERTIES TIMEOUT 5)

    add_executable(gateway_process_fixture tests/integration/process_fixture.c)
    target_compile_options(gateway_process_fixture PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

    add_executable(gateway_supervisor_tests tests/unit/supervisor_tests.c)
    target_link_libraries(gateway_supervisor_tests PRIVATE gateway_core)
    target_compile_options(gateway_supervisor_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME gateway_supervisor_tests
        COMMAND gateway_supervisor_tests $<TARGET_FILE:gateway_process_fixture>)
    set_tests_properties(gateway_supervisor_tests PROPERTIES TIMEOUT 5)

    add_executable(gateway_channel_manager_tests
        tests/integration/channel_manager_tests.c)
    target_link_libraries(gateway_channel_manager_tests PRIVATE gateway_core)
    target_compile_options(gateway_channel_manager_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME gateway_channel_manager_tests
        COMMAND gateway_channel_manager_tests
                $<TARGET_FILE:gateway_process_fixture>)
    set_tests_properties(gateway_channel_manager_tests PROPERTIES
        TIMEOUT 15
        FAIL_REGULAR_EXPRESSION "fixture-password")

    add_executable(gateway_reload_tests
        tests/integration/gateway_reload_tests.c)
    target_compile_options(gateway_reload_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME gateway_reload_tests
        COMMAND gateway_reload_tests $<TARGET_FILE:gatewayd>
                $<TARGET_FILE:gateway_process_fixture>)
    set_tests_properties(gateway_reload_tests PROPERTIES TIMEOUT 15)

    add_executable(gateway_http_tests
        tests/integration/gateway_http_tests.c)
    target_compile_options(gateway_http_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME gateway_http_tests
        COMMAND gateway_http_tests $<TARGET_FILE:gatewayd>
                $<TARGET_FILE:gateway_process_fixture>)
    set_tests_properties(gateway_http_tests PROPERTIES
        TIMEOUT 15
        FAIL_REGULAR_EXPRESSION "http-password")

    add_executable(gateway_recovery_tests
        tests/integration/input_recovery_tests.c)
    target_link_libraries(gateway_recovery_tests PRIVATE gateway_core)
    target_compile_options(gateway_recovery_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME gateway_input_recovery_tests
        COMMAND gateway_recovery_tests
                $<TARGET_FILE:gateway_process_fixture> input)
    set_tests_properties(gateway_input_recovery_tests PROPERTIES
        TIMEOUT 10
        FAIL_REGULAR_EXPRESSION "fixture-password")

    add_test(NAME gateway_publish_recovery_tests
        COMMAND gateway_recovery_tests
                $<TARGET_FILE:gateway_process_fixture> publish)
    set_tests_properties(gateway_publish_recovery_tests PROPERTIES
        TIMEOUT 10
        FAIL_REGULAR_EXPRESSION "fixture-password")

    add_executable(gateway_process_tests tests/integration/process_manager_tests.c)
    target_link_libraries(gateway_process_tests PRIVATE gateway_core)
    target_compile_options(gateway_process_tests PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME gateway_process_tests
        COMMAND gateway_process_tests $<TARGET_FILE:gateway_process_fixture>)
    set_tests_properties(gateway_process_tests PROPERTIES TIMEOUT 5)

    add_test(NAME gateway_single_channel_test
        COMMAND gatewayd
            --config ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateway.integration.yaml
            --exit-when-idle
            --ffprobe-binary $<TARGET_FILE:gateway_process_fixture>
            --ffmpeg-binary $<TARGET_FILE:gateway_process_fixture>)
    set_tests_properties(gateway_single_channel_test PROPERTIES
        TIMEOUT 5
        PASS_REGULAR_EXPRESSION "state=STOPPED event=clean_exit exit_code=0"
        FAIL_REGULAR_EXPRESSION "fixture-password")

    add_test(NAME gateway_multi_channel_test
        COMMAND ${CMAKE_COMMAND}
            -DGATEWAY=$<TARGET_FILE:gatewayd>
            -DFIXTURE=$<TARGET_FILE:gateway_process_fixture>
            -DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateway.multi.integration.yaml
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/assert_gateway_multi.cmake)
    set_tests_properties(gateway_multi_channel_test PROPERTIES TIMEOUT 5)

    add_test(NAME gateway_stable_run_test
        COMMAND gatewayd
            --config ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateway.integration.yaml
            --exit-when-idle
            --ffprobe-binary $<TARGET_FILE:gateway_process_fixture>
            --ffmpeg-binary $<TARGET_FILE:gateway_process_fixture>)
    set_tests_properties(gateway_stable_run_test PROPERTIES
        TIMEOUT 5
        ENVIRONMENT "GW_FIXTURE_MODE=steady"
        PASS_REGULAR_EXPRESSION "event=stable failures=0"
        FAIL_REGULAR_EXPRESSION "fixture-password")

    add_test(NAME gateway_startup_timeout_test
        COMMAND ${CMAKE_COMMAND}
            -DGATEWAY=$<TARGET_FILE:gatewayd>
            -DFIXTURE=$<TARGET_FILE:gateway_process_fixture>
            -DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateway.integration.yaml
            -DMODE=no-progress
            "-DREQUIRED=state=FAILED event=startup_timeout"
            -DFORBIDDEN=fixture-password
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/assert_gateway_failure.cmake)
    set_tests_properties(gateway_startup_timeout_test PROPERTIES
        TIMEOUT 4)

    add_test(NAME gateway_progress_timeout_test
        COMMAND ${CMAKE_COMMAND}
            -DGATEWAY=$<TARGET_FILE:gatewayd>
            -DFIXTURE=$<TARGET_FILE:gateway_process_fixture>
            -DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateway.integration.yaml
            -DMODE=stall
            "-DREQUIRED=state=FAILED event=progress_timeout"
            -DFORBIDDEN=fixture-password
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/assert_gateway_failure.cmake)
    set_tests_properties(gateway_progress_timeout_test PROPERTIES
        TIMEOUT 4)

    add_test(NAME gateway_retry_exhaustion_test
        COMMAND ${CMAKE_COMMAND}
            -DGATEWAY=$<TARGET_FILE:gatewayd>
            -DFIXTURE=$<TARGET_FILE:gateway_process_fixture>
            -DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateway.retry.integration.yaml
            -DMODE=fail
            "-DREQUIRED=state=FAILED event=worker_failure exit_code=9 failures=3 restart_count=2"
            -DFORBIDDEN=fixture-password
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/assert_gateway_failure.cmake)
    set_tests_properties(gateway_retry_exhaustion_test PROPERTIES
        TIMEOUT 5)

    add_test(NAME gateway_probe_failure_test
        COMMAND ${CMAKE_COMMAND}
            -DGATEWAY=$<TARGET_FILE:gatewayd>
            -DFIXTURE=$<TARGET_FILE:gateway_process_fixture>
            -DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateway.integration.yaml
            -DMODE=success
            -DPROBE_MODE=fail
            "-DREQUIRED=state=FAILED event=probe_failure exit_code=6 failures=1 restart_count=0"
            -DFORBIDDEN=fixture-password
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/assert_gateway_failure.cmake)
    set_tests_properties(gateway_probe_failure_test PROPERTIES TIMEOUT 4)

    add_test(NAME gateway_probe_timeout_test
        COMMAND ${CMAKE_COMMAND}
            -DGATEWAY=$<TARGET_FILE:gatewayd>
            -DFIXTURE=$<TARGET_FILE:gateway_process_fixture>
            -DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateway.integration.yaml
            -DMODE=success
            -DPROBE_MODE=timeout
            "-DREQUIRED=state=FAILED event=probe_timeout exit_code=143 failures=1 restart_count=0"
            -DFORBIDDEN=fixture-password
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/assert_gateway_failure.cmake)
    set_tests_properties(gateway_probe_timeout_test PROPERTIES TIMEOUT 4)

    add_test(NAME gateway_probe_mismatch_test
        COMMAND ${CMAKE_COMMAND}
            -DGATEWAY=$<TARGET_FILE:gatewayd>
            -DFIXTURE=$<TARGET_FILE:gateway_process_fixture>
            -DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateway.integration.yaml
            -DMODE=success
            -DPROBE_MODE=mismatch
            "-DREQUIRED=state=FAILED event=probe_mismatch exit_code=0 failures=1 restart_count=0"
            -DFORBIDDEN=fixture-password
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/assert_gateway_failure.cmake)
    set_tests_properties(gateway_probe_mismatch_test PROPERTIES TIMEOUT 4)

    add_test(NAME gateway_probe_retry_exhaustion_test
        COMMAND ${CMAKE_COMMAND}
            -DGATEWAY=$<TARGET_FILE:gatewayd>
            -DFIXTURE=$<TARGET_FILE:gateway_process_fixture>
            -DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateway.retry.integration.yaml
            -DMODE=success
            -DPROBE_MODE=fail
            "-DREQUIRED=state=FAILED event=probe_failure exit_code=6 failures=3 restart_count=2"
            -DFORBIDDEN=fixture-password
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/assert_gateway_failure.cmake)
    set_tests_properties(gateway_probe_retry_exhaustion_test PROPERTIES TIMEOUT 5)
endif()
