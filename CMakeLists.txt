cmake_minimum_required(VERSION 3.20)

project(taskPilot
    VERSION 0.1.0
    DESCRIPTION "Work priority ranking backend with an MCP interface"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json so clangd resolves includes without extra setup.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
# Deliberately system packages only — no vcpkg, no vendored third_party.
# taskPilot needs exactly four things and Ubuntu ships all of them:
#   nlohmann-json3-dev, libasio-dev, libsqlite3-dev, libgtest-dev
# On Debian/Ubuntu:  sudo apt install nlohmann-json3-dev libasio-dev \
#                                    libsqlite3-dev libgtest-dev
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(SQLite3 REQUIRED)

# asio is header-only and ships no reliable CMake config on every distro, so
# locate the header directly rather than gambling on find_package(asio).
find_path(ASIO_INCLUDE_DIR asio.hpp
    HINTS /usr/include /usr/local/include
    DOC "Directory containing asio.hpp (libasio-dev)")
if(NOT ASIO_INCLUDE_DIR)
    message(FATAL_ERROR
        "asio.hpp not found. Install it with: sudo apt install libasio-dev")
endif()

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------
add_library(taskpilot_warnings INTERFACE)
target_compile_options(taskpilot_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
# -Werror is on by default because a warning in a 3k-line project is cheap to
# fix now and expensive to triage later. Configure with
# -DTASKPILOT_WERROR=OFF to keep building while investigating one.
option(TASKPILOT_WERROR "Treat warnings as errors" ON)
if(TASKPILOT_WERROR)
    target_compile_options(taskpilot_warnings INTERFACE -Werror)
endif()

# ---------------------------------------------------------------------------
# Libraries, layer by layer
# ---------------------------------------------------------------------------
# The pure layers are split so a UI can link the error vocabulary without the
# ranking engine, or the ranking engine without SQLite and asio:
#
#   result  — errors only
#   model   — tasks, ranking, sync format; nlohmann + threads-free pure code
#   store   — SQLite persistence
#   client  — the blocking control-socket client (asio), no store
#
# taskpilot_core stays an INTERFACE target with the old alias, so every existing
# consumer keeps linking taskPilot::core and sees the same headers.

add_library(taskpilot_result STATIC
    src/core/Result.cpp
)
add_library(taskPilot::result ALIAS taskpilot_result)
target_include_directories(taskpilot_result
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(taskpilot_result PUBLIC cxx_std_20)
target_link_libraries(taskpilot_result
    PRIVATE
        taskpilot_warnings)

add_library(taskpilot_model STATIC
    src/core/Task.cpp
    src/core/Weights.cpp
    src/core/Clock.cpp
    src/core/Uuid.cpp
    src/core/TaskSync.cpp
    src/core/PriorityEngine.cpp
)
add_library(taskPilot::model ALIAS taskpilot_model)
target_include_directories(taskpilot_model
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(taskpilot_model PUBLIC cxx_std_20)
target_link_libraries(taskpilot_model
    PUBLIC
        taskPilot::result
        nlohmann_json::nlohmann_json
        Threads::Threads
    PRIVATE
        taskpilot_warnings)

add_library(taskpilot_store STATIC
    src/core/TaskStore.cpp
)
add_library(taskPilot::store ALIAS taskpilot_store)
target_include_directories(taskpilot_store
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(taskpilot_store PUBLIC cxx_std_20)
target_link_libraries(taskpilot_store
    PUBLIC
        taskPilot::model
        SQLite::SQLite3
        Threads::Threads
    PRIVATE
        taskpilot_warnings)

add_library(taskpilot_core INTERFACE)
add_library(taskPilot::core ALIAS taskpilot_core)
target_include_directories(taskpilot_core
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(taskpilot_core INTERFACE cxx_std_20)
target_link_libraries(taskpilot_core
    INTERFACE
        taskPilot::model
        taskPilot::store)

# L2a client — the control-socket client alone. It needs only the error type
# and asio, so a UI can link it without pulling SQLite or the ranking engine.
add_library(taskpilot_client STATIC
    src/control/ControlClient.cpp
)
add_library(taskPilot::client ALIAS taskpilot_client)
target_include_directories(taskpilot_client
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${ASIO_INCLUDE_DIR})
target_compile_features(taskpilot_client PUBLIC cxx_std_20)
target_link_libraries(taskpilot_client
    PUBLIC
        taskPilot::result
        nlohmann_json::nlohmann_json
    PRIVATE
        taskpilot_warnings)

# L2 control — the daemon's control socket, the MCP bridge, the REPL. Depends
# on core and on the now-separate client; core never depends on it.
add_library(taskpilot_control STATIC
    src/control/Services.cpp
    src/control/JsonRpcServer.cpp
    src/control/McpServer.cpp
    src/control/Repl.cpp
)
add_library(taskPilot::control ALIAS taskpilot_control)

target_include_directories(taskpilot_control
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${ASIO_INCLUDE_DIR})
target_compile_features(taskpilot_control PUBLIC cxx_std_20)
target_link_libraries(taskpilot_control
    PUBLIC taskPilot::core taskPilot::client
    PRIVATE taskpilot_warnings)

# ---------------------------------------------------------------------------
# The binary
# ---------------------------------------------------------------------------
# One executable for all four subcommands (serve | mcp | cli | version). The
# MCP bridge and the daemon share the method catalog as compiled-in data, so
# they must ship together — a single binary is the simplest way to guarantee
# the two halves can never be from different versions.
add_executable(taskPilot src/main.cpp)
target_link_libraries(taskPilot
    PRIVATE
        taskPilot::control
        taskpilot_warnings)

# ---------------------------------------------------------------------------
# Optional Qt Quick desktop UI
# ---------------------------------------------------------------------------
# Qt is never a requirement of the daemon build. The version check runs before
# find_package and degrades to a STATUS message, so a CMake 3.20-3.24 machine
# (the root minimum) still configures and tests the daemon. Set
# TASKPILOT_REQUIRE_UI=ON to turn a missing toolchain into a configure error.
option(TASKPILOT_UI "Build the Qt Quick UI when the toolchain is available" ON)
option(TASKPILOT_REQUIRE_UI "Fail configure when the UI cannot be built" OFF)

set(TASKPILOT_UI_ENABLED ${TASKPILOT_UI})
if(TASKPILOT_UI_ENABLED AND CMAKE_VERSION VERSION_LESS 3.25)
    if(TASKPILOT_REQUIRE_UI)
        message(FATAL_ERROR "The taskPilot UI requires CMake 3.25 or newer (Qt 6.12)")
    endif()
    message(STATUS "taskPilot: UI disabled (CMake < 3.25)")
    set(TASKPILOT_UI_ENABLED OFF)
endif()

if(TASKPILOT_UI_ENABLED)
    # Pin the major/minor: the system Qt (6.10 here) must not silently win over
    # the 6.12 kit the UI is developed against.
    find_package(Qt6 6.12 QUIET COMPONENTS
        Core Gui Qml Quick QuickControls2 Test QuickTest)
    if(Qt6_FOUND)
        qt_standard_project_setup()
        add_subdirectory(src/ui)
    elseif(TASKPILOT_REQUIRE_UI)
        message(FATAL_ERROR "TASKPILOT_REQUIRE_UI=ON but Qt 6.12 was not found")
    else()
        message(STATUS "taskPilot: UI disabled (Qt 6.12 not found)")
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "taskPilot ${PROJECT_VERSION} (${CMAKE_BUILD_TYPE})")
message(STATUS "  asio      : ${ASIO_INCLUDE_DIR}")
message(STATUS "  tests     : ${BUILD_TESTING}")
