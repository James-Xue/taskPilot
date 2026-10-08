# tests — GTest suites
#
# Scope split, mirroring the layering:
#   unit/        no I/O beyond an in-memory SQLite database; no sockets
#   integration/ binds a real socket on port 0 and drives it end to end
#
# gtest_discover_tests registers one CTest entry per TEST(), so a failure
# names the failing case instead of just "taskpilot_tests failed".

find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(taskpilot_tests
    unit/test_result.cpp
    unit/test_task.cpp
    unit/test_weights.cpp
    unit/test_priority_engine.cpp
    unit/test_task_store.cpp
    unit/test_services.cpp
    unit/test_mcp_server.cpp
    unit/test_json_rpc_server.cpp
    unit/test_repl.cpp
    unit/test_control_client.cpp
    integration/test_daemon_roundtrip.cpp
)

target_link_libraries(taskpilot_tests
    PRIVATE
        taskPilot::control
        GTest::gtest
        GTest::gtest_main
        taskpilot_warnings)

gtest_discover_tests(taskpilot_tests
    PROPERTIES
        TIMEOUT 60
    DISCOVERY_TIMEOUT 30)
