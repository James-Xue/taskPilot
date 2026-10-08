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
# L1 core    — tasks, ranking math, SQLite persistence. Knows nothing about
#              JSON-RPC, MCP, or sockets.
add_library(taskpilot_core STATIC
    src/core/Result.cpp
    src/core/Task.cpp
    src/core/Weights.cpp
    src/core/Clock.cpp
    src/core/Uuid.cpp
    src/core/TaskSync.cpp
    src/core/PriorityEngine.cpp
    src/core/TaskStore.cpp
)
add_library(taskPilot::core ALIAS taskpilot_core)

target_include_directories(taskpilot_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(taskpilot_core PUBLIC cxx_std_20)
target_link_libraries(taskpilot_core
    PUBLIC
        nlohmann_json::nlohmann_json
        SQLite::SQLite3
        Threads::Threads
    PRIVATE
        taskpilot_warnings)

# L2 control — the daemon's control socket, the client, the MCP bridge, the
#              REPL. Depends on core; core never depends on it.
add_library(taskpilot_control STATIC
    src/control/Services.cpp
    src/control/JsonRpcServer.cpp
    src/control/ControlClient.cpp
    src/control/McpServer.cpp
    src/control/Repl.cpp
)
add_library(taskPilot::control ALIAS taskpilot_control)

target_include_directories(taskpilot_control
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${ASIO_INCLUDE_DIR})
target_compile_features(taskpilot_control PUBLIC cxx_std_20)
target_link_libraries(taskpilot_control
    PUBLIC taskpilot_core
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
