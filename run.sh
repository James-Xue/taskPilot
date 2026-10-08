#!/usr/bin/env bash
# run.sh — build and run taskPilot
#
#   ./run.sh                 build if needed, then `serve` (the default)
#   ./run.sh build [--clean] configure + compile
#   ./run.sh test            build, then run the unit tests via ctest
#   ./run.sh serve           run the backend daemon (foreground)
#   ./run.sh mcp             run the stdio MCP bridge (used by Claude Code)
#   ./run.sh cli             attach an interactive REPL to a running daemon
#   ./run.sh version         print the version
#
# The daemon and the MCP bridge are the same binary, so a rebuild can never
# leave them from different versions.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

BUILD_DIR="${TASKPILOT_BUILD_DIR:-build}"
BIN="${BUILD_DIR}/taskPilot"

# Default database location. Kept inside the repo's data/ directory so a fresh
# clone is self-contained; override with TASKPILOT_DB for a different store.
export TASKPILOT_DB="${TASKPILOT_DB:-$(pwd)/data/taskpilot.db}"

# Colors only when stderr is a terminal, so piping stays clean.
if [ -t 2 ]; then
    C_DIM=$'\033[2m'; C_RED=$'\033[31m'; C_OFF=$'\033[0m'
else
    C_DIM=''; C_RED=''; C_OFF=''
fi

log() { printf '%s==> %s%s\n' "$C_DIM" "$*" "$C_OFF" >&2; }
die() { printf '%serror: %s%s\n' "$C_RED" "$*" "$C_OFF" >&2; exit 1; }

needs_configure() { [ ! -f "${BUILD_DIR}/CMakeCache.txt" ]; }

# Rebuild only when something actually changed, so `./run.sh mcp` stays fast
# enough to be spawned on every Claude Code session without being noticed.
needs_build() {
    [ ! -x "$BIN" ] && return 0
    # Any source, header, or build file newer than the binary.
    [ -n "$(find src tests CMakeLists.txt -newer "$BIN" -print -quit 2>/dev/null)" ]
}

do_build() {
    needs_configure && {
        log "configuring (${BUILD_DIR})"
        cmake -B "$BUILD_DIR" -S . -G Ninja -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Debug}" \
            >&2 || die "cmake configure failed"
    }
    log "building"
    cmake --build "$BUILD_DIR" >&2 || die "build failed"
}

cmd="${1:-serve}"
shift || true

case "$cmd" in
    build)
        if [ "${1:-}" = "--clean" ]; then
            log "removing ${BUILD_DIR}"
            rm -rf "$BUILD_DIR"
        fi
        do_build
        log "ok: ${BIN}"
        ;;
    test)
        do_build
        log "running tests"
        ctest --test-dir "$BUILD_DIR" --output-on-failure
        ;;
    clean)
        log "removing ${BUILD_DIR}"
        rm -rf "$BUILD_DIR"
        ;;
    serve|mcp|cli|version)
        needs_build && do_build
        exec "$BIN" "$cmd" "$@"
        ;;
    *)
        die "unknown command: ${cmd} (try: build|test|serve|mcp|cli|version|clean)"
        ;;
esac
