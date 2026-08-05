cmake_minimum_required(VERSION 3.20)

project(rk_media_gateway VERSION 0.1.0 LANGUAGES C)

option(GATEWAY_BUILD_TESTS "Build unit tests" ON)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBYAML REQUIRED IMPORTED_TARGET yaml-0.1)

add_library(gateway_core
    src/channel/channel_state.c
    src/config/config.c
    src/pipeline/pipeline_builder.c
    src/monitor/progress_parser.c
    src/process/process_manager.c
)

target_include_directories(gateway_core PUBLIC include)
target_link_libraries(gateway_core PUBLIC PkgConfig::LIBYAML)
target_compile_options(gateway_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

add_executable(gatewayd src/main.c)
target_link_libraries(gatewayd PRIVATE gateway_core)
target_compile_options(gatewayd PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

install(TARGETS gatewayd RUNTIME DESTINATION bin)
install(FILES config/gateway.example.yaml
        DESTINATION share/rk-media-gateway)

if(GATEWAY_BUILD_TESTS)
    enable_testing()
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

    add_executable(gateway_process_fixture tests/integration/process_fixture.c)
    target_compile_options(gateway_process_fixture PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

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
            --ffmpeg-binary $<TARGET_FILE:gateway_process_fixture>)
    set_tests_properties(gateway_single_channel_test PROPERTIES
        TIMEOUT 5
        PASS_REGULAR_EXPRESSION "state=STOPPED event=clean_exit exit_code=0"
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
endif()
