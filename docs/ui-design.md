# taskPilot cross-platform UI — design

This document specifies one adaptive Qt Quick application for Ubuntu, Windows and
Android. It treats the daemon, the 15-method catalog and the SQLite store as
frozen: the UI is a third frontend, not a second implementation of the product.
Facts marked UNVERIFIED were not observed on the development machine and must be
resolved by the milestone that depends on them.

Status: design for review. No UI code exists yet.

---

## 1. Goals, non-goals, and constraints

### Goals

- Ship a desktop client that makes the ranked queue, the score breakdown and the
  write commands usable without the REPL or an MCP host.
- Ship the same QML application on Android in read-only form — not a second code
  base and not a fork of the queue logic.
- Keep the daemon the only **authoritative online** writer and ranker. The
  desktop UI displays the `get_queue` order and the `score` value verbatim and
  never re-sums the four parts.
- Let the Android offline snapshot rank locally with the compiled default
  weights, and label that order as local. This is the one deliberate exception
  to "the daemon ranks"; it is never presented as the daemon's order.
- Keep Qt optional, so a machine without Qt can still configure, build and test
  the daemon.
- Make the UI structurally unable to open SQLite, re-rank the online queue, or
  reach a non-loopback control socket.
- Add automated tests for the queue model, filter ordering, rendered order and
  an offscreen render smoke, without disturbing the existing CTest gate.

### Non-goals

- No change to method names, parameter schemas, reply shapes, JSON-RPC framing
  or error-code mapping. The catalog is 15 methods (`src/control/Services.cpp:441-449`,
  dispatch at `:1503-1518`, count asserted in `tests/unit/test_services.cpp:178`).
- No authentication, TLS, session or push channel on the existing control
  socket. It is unauthenticated plain TCP and stays loopback-only
  (`src/control/JsonRpcServer.hpp:12-16`; `src/main.cpp:141`).
- No SQLite access from any UI target.
- No client-side ranking of the **online** queue and no re-summing of
  `score_parts`.
- No Android writes, offline merge, or online Android transport in this phase.
- No undo affordance: the daemon has no undo, so the UI does not pretend
  otherwise.
- No server-side text search, tag filtering on the queue, or offset/page-token
  pagination. Both `get_queue` and `list_tasks` do accept a top-N `limit`
  (`src/control/Services.cpp:1095-1106,1195-1206`); what is absent is an offset.

### Constraints

| Constraint | Evidence |
|---|---|
| The method catalog is the single source of truth and has 15 entries. "15 MCP methods" means this tool catalog; the MCP transport itself also serves `initialize`, `server/discover`, `ping`, `tools/list`, `tools/call` and notifications. | `src/control/Services.cpp:441-460`; `tests/unit/test_services.cpp:178`; `src/control/McpServer.cpp:486-649` |
| The daemon is the only writer; clients never open the database. | `README.md:225-226`; `AGENTS.md:29`; `docs/architecture.md:91` |
| The control socket binds `127.0.0.1` and has no authentication. The documented answer to remote access is a tunnel, not a wider bind or a hand-rolled token. | `src/main.cpp:141`; `src/control/JsonRpcServer.hpp:12-16`; `AGENTS.md:359-365` |
| The headless build must keep working: system dependencies only, no Qt requirement, one GTest executable discovered into CTest entries (369 at HEAD `ac623f0`). | `CMakeLists.txt:25-116`; `tests/CMakeLists.txt:13-40`; `run.sh:57-65` |
| Warnings are errors by default with GCC/Clang flags; an MSVC branch must be added rather than loosening the flags. | `CMakeLists.txt:47-56` |
| Documentation, comments, identifiers and commit messages are English only. | `AGENTS.md:142-148` |
| UI sources must live under `src/ui`, because `run.sh` only watches `src`, `tests` and `CMakeLists.txt` for staleness. | `run.sh:51-55` |
| The root project declares CMake 3.20. The installed Qt 6.12 kit declares CMake 3.25 as its minimum, so the UI is enabled only when both conditions hold. | `CMakeLists.txt:1`; `Qt6ConfigExtras.cmake` in the Qt 6.12 kit |
| Task titles and notes are personal data. The export must never be committed or bundled into an APK. | `README.md:195-199`; `docs/sync.md:429-436` |
| Two Qt versions exist on the development machine: the 6.12.0 kit and system 6.10.2. Every UI command must pin the kit. | verified locally 2026-11-05 |

Documentation debt: `docs/mcp.md:292-311` lists 13 tools and omits
`export_tasks`/`import_tasks`. The code is authoritative; the table should be
fixed in a separate documentation commit, not silently copied into this design.

---

## 2. Module and target split

### 2.1 Current graph

```
taskpilot_core (STATIC: Result, Task, Weights, Clock, Uuid,
                TaskSync, PriorityEngine, TaskStore)
    PUBLIC nlohmann_json, SQLite3, Threads; PRIVATE warnings
        |
taskpilot_control (STATIC: Services, JsonRpcServer, ControlClient,
                   McpServer, Repl)
    PUBLIC taskpilot_core; PRIVATE asio include, warnings
        |
        +-- taskPilot (executable)
        +-- taskpilot_tests (GTest, 369 CTest entries)
```

There is no Qt reference today. `ControlClient.cpp` is compiled into
`taskpilot_control`, so the REPL, the MCP bridge and the tests all obtain it
through `taskPilot::control`.

### 2.2 Proposed graph

```
taskpilot_result (STATIC: Result.cpp)                 errors only, no I/O
   |
   +-- taskpilot_model (STATIC: Task, Weights, Clock,
   |       PriorityEngine, TaskSync, Uuid)             pure C++20, nlohmann only
   |       |
   |       +-- taskpilot_store (STATIC: TaskStore.cpp) SQLite
   |
   +-- taskpilot_client (STATIC: ControlClient.cpp)    asio (PRIVATE)

taskpilot_core (INTERFACE) = model + store             keeps alias taskPilot::core
taskpilot_control (STATIC) = Services, JsonRpcServer,
    McpServer, Repl; PUBLIC core + client              keeps alias taskPilot::control
taskPilot (executable)                                 unchanged

taskpilot-ui-core (STATIC)    app controller, queue model, filter model, profile
taskpilot-ui-offline (STATIC) JSONL repository + offline backend (host and Android)
taskpilot-ui-desktop (STATIC) control worker + daemon backend (non-Android only)
taskpilot-ui (EXECUTABLE)     Qt Quick app + QML module TaskPilot.Ui
```

The desktop link graph is `taskpilot-ui -> ui-core -> result` plus
`ui-desktop -> client -> result`. It contains no `PriorityEngine` and no SQLite,
so the desktop binary cannot locally re-rank or open the store. The Android
graph is `taskpilot-ui -> ui-core` plus `ui-offline -> model -> result`: no
asio, no SQLite, no socket code. `taskpilot-ui-offline` is built on the host too,
so its Qt tests run in the normal gate rather than only on a device.

### 2.3 Files, headers, and what moves

No existing source file changes directory in phase 1; that keeps every include
path valid for `src/control/Repl.hpp`, `src/main.cpp` and the tests.

| Item | Change |
|---|---|
| `src/control/RpcTypes.hpp` | New. Declares `using RpcResult = Result<nlohmann::json>` and includes **both** `<nlohmann/json.hpp>` and `core/Result.hpp`, so the header is self-contained. `Services.hpp` and `ControlClient.hpp` include it instead of each other. |
| `src/control/Services.hpp` | Remove the alias, include `RpcTypes.hpp`. |
| `src/control/ControlClient.hpp` | Include `RpcTypes.hpp` instead of `Services.hpp`; the client no longer drags `Clock.hpp`/`TaskStore.hpp` into UI translation units. |
| `src/control/ControlClient.cpp` | Target membership moves from `taskpilot_control` to `taskpilot_client`; no file move, no include change. |
| `src/core/*` | No file moves; only the target membership split described above. `TaskStore.cpp` remains the only file under `src/` that includes `<sqlite3.h>` (the tests include it directly as well). `Uuid.cpp` is POSIX-only (`src/core/Uuid.cpp:32-39,248-295`); the NDK compiles it, MSVC does not. |
| `src/ui/main.cpp` | New: `QGuiApplication`, backend selection, QML engine setup. |
| `src/ui/app_controller.{h,cpp}` | New: QML-facing state machine and queue snapshots. |
| `src/ui/control_worker.{h,cpp}` | New, desktop only: owns the blocking `ControlClient` calls. |
| `src/ui/daemon_backend.{h,cpp}` | New, desktop only: the online backend implementation. |
| `src/ui/offline_repository.{h,cpp}` | New, platform-neutral: JSONL parse plus `PriorityEngine::rank`; no sockets. |
| `src/ui/offline_backend.{h,cpp}` | New, platform-neutral: the read-only backend implementation over the repository. |
| `src/ui/queue_model.{h,cpp}` | New: `QAbstractListModel` in reply order, with no sort API. |
| `src/ui/task_filter_model.{h,cpp}` | New: order-preserving proxy for tags and text. |
| `src/ui/platform_profile.{h,cpp}` | New: shell choice and touch-target constant. |
| `src/ui/qml/` | New: `Main.qml`, `DesktopShell.qml`, `MobileShell.qml`, components. |
| `tests/ui/` | New: Qt Test, Qt Quick Test and offscreen smoke sources. |

### 2.4 CMake sketch

```cmake
# Root — the headless contract is unchanged: CMake 3.20, no Qt required.

add_library(taskpilot_result STATIC src/core/Result.cpp)
target_include_directories(taskpilot_result PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(taskpilot_result PUBLIC cxx_std_20)
target_link_libraries(taskpilot_result PRIVATE taskpilot_warnings)
add_library(taskPilot::result ALIAS taskpilot_result)

add_library(taskpilot_model STATIC
    src/core/Task.cpp src/core/Weights.cpp src/core/Clock.cpp
    src/core/PriorityEngine.cpp src/core/TaskSync.cpp src/core/Uuid.cpp)
target_include_directories(taskpilot_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(taskpilot_model PUBLIC cxx_std_20)
target_link_libraries(taskpilot_model
    PUBLIC taskPilot::result nlohmann_json::nlohmann_json
    PRIVATE taskpilot_warnings)
add_library(taskPilot::model ALIAS taskpilot_model)

add_library(taskpilot_store STATIC src/core/TaskStore.cpp)
target_compile_features(taskpilot_store PUBLIC cxx_std_20)
target_link_libraries(taskpilot_store
    PUBLIC taskPilot::model SQLite::SQLite3 Threads::Threads
    PRIVATE taskpilot_warnings)
add_library(taskPilot::store ALIAS taskpilot_store)

add_library(taskpilot_core INTERFACE)
target_include_directories(taskpilot_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(taskpilot_core INTERFACE cxx_std_20)
target_link_libraries(taskpilot_core INTERFACE taskPilot::model taskPilot::store)
add_library(taskPilot::core ALIAS taskpilot_core)

add_library(taskpilot_client STATIC src/control/ControlClient.cpp)
target_include_directories(taskpilot_client PRIVATE ${ASIO_INCLUDE_DIR})
target_compile_features(taskpilot_client PUBLIC cxx_std_20)
target_link_libraries(taskpilot_client
    PUBLIC taskPilot::result nlohmann_json::nlohmann_json
    PRIVATE taskpilot_warnings)
add_library(taskPilot::client ALIAS taskpilot_client)

# taskpilot_control loses ControlClient.cpp but keeps the asio include for
# JsonRpcServer, and links core + client PUBLIC.
target_include_directories(taskpilot_control
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${ASIO_INCLUDE_DIR})
target_link_libraries(taskpilot_control
    PUBLIC taskPilot::core taskPilot::client
    PRIVATE taskpilot_warnings)

# Qt is optional. The version check runs BEFORE find_package and degrades to a
# STATUS message, so a CMake 3.20-3.24 machine still configures the daemon.
option(TASKPILOT_UI "Build the Qt Quick UI when the toolchain is available" ON)
option(TASKPILOT_REQUIRE_UI "Fail configure when the UI cannot be built" OFF)

set(TASKPILOT_UI_ENABLED ${TASKPILOT_UI})
if(TASKPILOT_UI_ENABLED AND CMAKE_VERSION VERSION_LESS 3.25)
    if(TASKPILOT_REQUIRE_UI)
        message(FATAL_ERROR "The UI requires CMake 3.25 or newer (Qt 6.12)")
    endif()
    message(STATUS "taskPilot: UI disabled (CMake < 3.25)")
    set(TASKPILOT_UI_ENABLED OFF)
endif()

if(TASKPILOT_UI_ENABLED)
    # Pin the major/minor: the system Qt 6.10.2 must not silently win over the
    # 6.12 kit (their Controls styles differ).
    find_package(Qt6 6.12 QUIET COMPONENTS
        Core Gui Qml Quick QuickControls2 LabsPlatform Test QuickTest)
    if(Qt6_FOUND)
        qt_standard_project_setup()
        add_subdirectory(src/ui)
    elseif(TASKPILOT_REQUIRE_UI)
        message(FATAL_ERROR "TASKPILOT_REQUIRE_UI=ON but Qt 6.12 was not found")
    else()
        message(STATUS "taskPilot: UI disabled (Qt 6.12 not found)")
    endif()
endif()
```

`src/ui/CMakeLists.txt` creates the UI libraries above, then:

```cmake
qt_add_executable(taskpilot-ui main.cpp)   # required for Android APK plumbing
qt_add_qml_module(taskpilot-ui
    URI TaskPilot.Ui
    VERSION 1.0
    QML_FILES qml/Main.qml qml/DesktopShell.qml qml/MobileShell.qml ...)
```

`qt_add_executable` (not plain `add_executable`) is what makes the target a
Qt Android application module and generates the Gradle/APK targets. The QML
module is attached to the executable rather than a separate library; the test
executable hosts its own module from the same QML file list and registers the
same C++ types, so no plugin or import-path tricks are needed.

### 2.5 Why the result library is separate

`ControlClient` needs `Error`, `ErrorCode`, the error factories and
`toString()`, all implemented in `Result.cpp`. If `taskpilot_client` linked
`taskpilot_model` for them, the desktop UI would also link `PriorityEngine` and
`TaskSync`, creating a second library that could rank. Splitting
`taskpilot_result` keeps the online UI down to the transport and the error
vocabulary. It also lets the Android offline build depend on `model` without
pulling the client or asio. Each `.cpp` is compiled into exactly one target.

---

## 3. Shared application architecture

### 3.1 The blocking boundary

`ControlClient::call` is synchronous, opens one fresh TCP connection per call,
and bounds connect, write and read at 5000 ms each, so one wedged call can block
for roughly 15 s (`src/control/ControlClient.hpp:29-38,58-71`). It has no
cancellation, subscription or asynchronous API. It must never run on the GUI
thread.

```
QML (GUI thread)
  |  invoke: refresh(), completeTask(id), saveWeights(map)
  v
AppController (GUI thread)
  |  queued signal, FIFO, at most one request in flight
  v
ControlWorker (dedicated QThread)
  |  ControlClient::call (blocking; 5 s per step)
  v
127.0.0.1:8091 (daemon)
  |
  v
reply JSON -> compact UTF-8 QByteArray -> queued signal
  -> AppController parses once with QJsonDocument -> model reset
```

Rules:

- Exactly one worker thread and one worker object. Requests are serialized,
  which gives deterministic write-then-refresh ordering and bounds connection
  and daemon session-thread churn.
- Every request carries a generation number. A stale reply is dropped; a
  refresh already in flight sets a single pending flag rather than queueing
  duplicates.
- The worker converts `nlohmann::json` to a compact UTF-8 `QByteArray` before
  emitting; the GUI thread parses it once. No JSON object crosses the thread
  boundary.
- Interactive calls use the client's default 5 s per-step budget. **Only
  `export_tasks`** uses a dedicated client with the 64 MiB reply cap
  (`src/main.cpp:633-644,739-740`); its reply is the whole backlog. `import_tasks`
  keeps the default reply cap because its reply is a bounded merge report; its
  large payload is the **request**. The import file is read and serialized on
  the worker thread, and M1 must measure the largest generated backlog case.
  `JsonRpcServer` reads one newline-terminated line with `asio::streambuf` and
  has no application-level request cap, so no protocol change is made here.
- On shutdown the controller asks the worker to stop between queued items and
  waits a bounded time; a call already inside the socket can delay exit by up
  to its per-step budget.

### 3.2 The queue model

`QueueModel` is a snapshot of the exact array returned by `get_queue`
(`src/control/Services.cpp:1091-1160`). Each entry is the task JSON plus
`score` (a number), `score_parts` (`importance`, `urgency`, `age`, `blocks`),
`urgency_factor` and `age_days` (`src/core/PriorityEngine.hpp:40-50,113-115`;
`src/core/PriorityEngine.cpp:278-306`). There is no `score.total` key on the
wire; the `scoreTotal` role maps to `score`.

- Rows are stored and exposed in reply order. There is no comparator, no sort
  role and no `sort()` method.
- The four parts are displayed as explanation only and are never re-summed;
  floating-point rounding can differ from the delivered total.
- The queue header says "Ranked by the daemon". The word "sort" does not appear
  in QML; a CTest grep enforces that.
- A refresh builds a fresh row vector and calls
  `beginResetModel()`/`endResetModel()`. The selected `id` (online) or `uid`
  (offline) is captured **before** the reset and the current index is restored
  **after** `endResetModel()`, when row indexes are valid again. A deleted row
  clears the selection.
- An empty queue is not an empty backlog: done and archived tasks can exist
  while the queue is empty. The UI distinguishes "no actionable tasks" from
  "backlog is empty" using the tallies from `get_stats`.
- Relative-time rendering uses the `now` value returned by `get_queue` /
  `get_stats`; `get_task` has no `now` key, so it is not used alone to derive
  "due in N hours".

### 3.3 Detail, filter and selection state

- Selecting a queue row renders immediately from the snapshot; each row already
  carries every task field.
- The detail panel shows title, notes, status, importance, due, blocks, tags,
  `score`, `score_parts`, `urgency_factor` and `age_days` verbatim.
- History is a separate page. `list_tasks` accepts exactly one status value
  (`open`, `in_progress`, `done`, `archived`) — a pipe-separated value is
  rejected with `kInvalidArgument` (`src/control/Services.cpp:1175-1181`). The
  page therefore issues one call for the selected status and re-issues when the
  selector changes; it does not merge two statuses. A history row has no score
  fields, so selecting it calls `get_task(id)` for the ranked view. There is no
  offset, so the page labels its limit as "showing first N" rather than
  pretending to paginate.
- `TaskFilterModel` is an order-preserving proxy. Tag filtering is OR semantics
  over selected tags; text search is a case-insensitive title/notes substring.
  It implements `filterAcceptsRow` only, never `sort()`, and counts are computed
  from the unfiltered source model.
- Filtering is client-side because `get_queue` has no tag or text parameter.
  For the history page the single-tag `list_tasks` parameter may be used, but
  the queue pane filters locally.
- Selected id, filter tags, search text, pane widths and window geometry persist
  in `QSettings`.

### 3.4 Error presentation

All failures are values, not exceptions: `kInvalidArgument`, `kNotFound`,
`kConflict`, `kStorageFailure`, `kInternal` (`src/core/Result.hpp:33-37`).
`ControlClient` maps wire codes back to this family and preserves the daemon's
message (`src/control/ControlClient.cpp:384-417,551-598`).

| Condition | Detection | UI state |
|---|---|---|
| Daemon not running | call fails `kStorageFailure`, then `ping()` returns false | Offline banner: "Daemon not running", `./run.sh serve` hint, Retry |
| Real storage fault | call fails `kStorageFailure` but `ping()` is true | Storage-error banner with the daemon's message; Retry |
| Bad user input | `kInvalidArgument` | Inline field error; no banner |
| Stale selection | `kNotFound` after delete/complete elsewhere | Refresh, clear selection, toast |
| State conflict | `kConflict` | Refresh and toast |
| Anything else | `kInternal` | Error banner with message and Retry |

`ping()` is a connect-only probe; it proves the port is listening, not that the
store is healthy (`src/control/ControlClient.hpp:91-94`). The `kStorageFailure`
code alone is ambiguous because the client synthesizes it for cannot-connect
while the domain uses the same family for SQLite and filesystem faults; the ping
preflight is the documented discriminator. No message-string parsing is used.

Writes are never optimistic. Every mutation is exactly one method call; success
triggers a refresh, failure leaves the model untouched. Delete requires an
explicit confirmation dialog. There is no undo.

### 3.5 Offline source abstraction

The QML layer sees one backend contract with two implementations:

- `DaemonBackend` (desktop): wraps `ControlWorker`, exposes `state`,
  `lastUpdated`, `readOnly=false`, `sourceLabel`, `errorText`; invokables
  `refresh()`, `addTask()`, `updateTask()`, `completeTask()`, `reopenTask()`,
  `deleteTask()`, `saveWeights()`; signals `queueChanged`, `statsChanged`,
  `errorRaised`.
- `OfflineBackend` (Android and host tests): exposes the same properties with
  `readOnly=true`; the only invokables are `reload()` and `loadOfflineFile(url)`.
  Write invokables are absent from the mobile shell, not merely disabled.

`OfflineBackend` parses the JSONL with the free function `parseJsonl`
(`src/core/TaskSync.hpp:301`; `src/core/TaskSync.cpp:646`), converts records
keyed by `uid`, and ranks actionable tasks with `PriorityEngine::rank` and the
compiled default weights (`src/core/PriorityEngine.hpp:104-109`;
`src/core/Weights.hpp:51-60`). Offline records always carry `id = 0` because the
local row id never travels; `uid` is the only identity
(`src/core/TaskSync.cpp:622-642`). The mobile UI labels its order "local
ranking, default weights" and never presents it as the daemon's rank.

---

## 4. Three-platform layouts

### 4.1 One app, two shells

`Main.qml` hosts one `ApplicationWindow` and loads `DesktopShell` or
`MobileShell` from a `PlatformProfile` singleton. The runtime test is
`Qt.platform.os === "android"`; a compile-time `TASKPILOT_MOBILE` switch selects
the backend and the link graph and is the authoritative platform switch. A
`TASKPILOT_UI_SHELL` override lets offscreen tests render either shell. Shared
pages, components, models and formatting live once.

### 4.2 Desktop (Ubuntu and Windows)

```
+--------------------------------------------------------------------------------+
| [Add]  [Refresh]  [Filter.....................]   (Connected)  [Settings]     |
+--------------------+-------------------------------------+---------------------+
| Queue · N          | Detail                              | Tags                |
| Ranked by the      | Title / status / importance / due   | [x] work       4    |
| daemon             | Notes                               | [ ] home       3    |
|                    |                                     | [ ] errand     1    |
| 1. Task A   score  | Score  <delivered number>           | Clear filters       |
| 2. Task B   score  | parts: importance urgency age blocks| Weights: 5/30/2/8   |
| 3. Task C   score  | urgency_factor / age_days           | (read-only summary) |
|                    | [Complete] [Edit] [Delete]          |                     |
+--------------------+-------------------------------------+---------------------+
| open 12 · in progress 3 · overdue 2 · updated 12:04:11 · top task ...          |
+--------------------------------------------------------------------------------+
```

Horizontal `SplitView` with three panes: queue (`preferredWidth 380`, minimum
280), detail (fill, minimum 360), tag filter (`preferredWidth 260`, minimum
200). Pane widths and window geometry persist in `QSettings`.

Keyboard shortcuts, declared on `Action` objects rather than inside delegates:

| Shortcut | Action |
|---|---|
| `Ctrl+N` | New task |
| `F5` / `Ctrl+R` | Refresh |
| `Ctrl+F` / `/` | Focus filter |
| `J` / `K`, `Down` / `Up` | Move selection |
| `Space` | Complete or reopen selected |
| `E` | Edit selected |
| `Delete` | Delete with confirmation |
| `Ctrl+1` / `Ctrl+2` / `Ctrl+3` | Focus queue / detail / tag pane |
| `Ctrl+,` | Settings |
| `Ctrl+E` / `Ctrl+I` | Export / import |
| `Esc` | Clear filter or close dialog |

Tray: desktop-only `Qt.labs.platform` `SystemTrayIcon` with Show/Hide, Refresh,
Add task, Quit. If `available` is false, no icon is created and the status bar
explains why; close-to-tray is off by default. Tray availability on
GNOME/Wayland is UNVERIFIED.

Responsive fallback: below about 900 px width the tag pane becomes a right
`Drawer`; below about 700 px the detail pane is pushed as a `StackView` page.
Platform, not width, remains the primary branch.

### 4.3 Android

```
+-------------------------------+
| <  taskPilot             [v]  |
+-------------------------------+
| Read-only offline snapshot    |
| tasks.jsonl · updated 12:04   |
|-------------------------------|
| Task A               score    |
| due chip · tags · status      |
|-------------------------------|
| Task B               score    |
| due chip · tags · status      |
|                               |
|      (pull to refresh)        |
+-------------------------------+
|  Queue      |   Tags   | Info |
+-------------------------------+
```

`ApplicationWindow` with a `StackView` root. The first page is `QueuePage`;
tapping a row pushes `DetailPage`; back pops the stack. The footer is a bottom
`TabBar` with Queue, Tags and Info. Each tab and every interactive control has
`implicitHeight >= 48`; list delegates are at least 72 dp tall; spacing is
12-16 dp. Material style, system light/dark, no hover-only affordances.
Pull-to-refresh is a small custom component because Qt Quick Controls 2 has
none; it re-reads the imported file and shows "Updated <time>" or the error.
Safe-area margins cover the gesture bar. If the Android back key does not reach
the `StackView` handler, the on-screen back button is the fallback (UNVERIFIED
until M3).

The mobile shell compiles no write controls and no host/port editor. The Info
tab shows the app version, the imported file, its timestamp and the offline
limitations.

### 4.4 Windows deltas

Windows uses the same QML and the same `DesktopShell`; there is no third layout.
The layout sketch in 4.2 is the Windows layout. Differences are build and
platform integration only:

- A native menu bar may wrap the same `Action` objects; the tray uses the
  platform integration of `Qt.labs.platform`. Fonts and High-DPI scaling follow
  the system.
- MSVC 2022 x64, Release, no debug symbols. The UI target is portable Qt/C++,
  but the daemon needs more portability work than the earlier draft assumed:
  `src/core/Uuid.cpp` entropy, `src/control/JsonRpcServer.cpp:62,469-473`
  (`<poll.h>`, `pollfd`, `::poll`) and `src/control/Repl.cpp:132,652,699`
  (`localtime_r`), plus the POSIX `mkstemp` use in
  `tests/integration/test_daemon_roundtrip.cpp`.
- The GCC warning set is unconditional; add an MSVC branch with equivalent
  strictness instead of dropping `-Werror`.
- Dependency provisioning for asio, nlohmann_json, SQLite and GTest needs an
  explicit decision; the repository forbids vcpkg and vendored `third_party`
  (`AGENTS.md:66,326`). M2 is conditional on that decision (see 8.2).
- Qt 6.12 for Windows is `msvc2022_64`, obtained through the Qt Online
  Installer. Compilation is UNVERIFIED because no MSVC toolchain exists on the
  development machine. Windows is compile verification only; ctest is not
  required there.

---

## 5. Android connectivity decision

### 5.1 Decision

Android is **offline and read-only** in this phase. The app reads a
user-supplied copy of the JSONL export produced by the desktop daemon and opens
no socket at all. Option (b), an authenticated remote transport, is deferred to
a separate design.

### 5.2 Rationale

1. There is no authenticated transport to reuse. `ControlClient` accepts a
   host, port, timeout and reply cap only; there is no TLS, token or session
   option (`src/control/ControlClient.hpp:69-71`). The server header states that
   exposing the unauthenticated socket beyond loopback would hand anyone on the
   network write access to the backlog (`src/control/JsonRpcServer.hpp:12-16`),
   and repository policy says the answer is a tunnel (`AGENTS.md:359-365`).
2. Option (b) is a server and protocol project, not a UI feature: handshake,
   token issuance, storage, rotation, revocation, replay protection, pairing,
   TLS termination, rate limiting, audit and Android OpenSSL packaging per ABI.
   It would add authenticated transport semantics to a daemon whose 15-method
   surface is frozen for this delivery.
3. Option (a) reuses an already versioned, specified format. The export format
   version is 1 (`src/core/TaskSync.hpp:61`); the file has one newline-terminated
   record per uid, including tombstones, and is written by `./run.sh export`
   (`README.md:199-216`; `docs/sync.md:431-446`).
4. The pure model already compiles on Android without SQLite or asio: `Task`,
   `Weights`, `PriorityEngine`, `TaskSync`, `Clock` and `Result` depend only on
   nlohmann; `Uuid.cpp` is POSIX and NDK-compatible.
5. Offline is provable. The APK declares no `INTERNET` permission, so airplane
   mode is a complete functional test, and there is no listener or credential
   that can be misconfigured.

### 5.3 Behavior and data refresh

- First run offers "Open tasks.jsonl" through `QtQuick.Dialogs.FileDialog`
  (Storage Access Framework). The selected file is copied into
  `QStandardPaths::AppDataLocation` so it survives a lost content-URI grant; the
  copy path is remembered.
- Parsing uses `parseJsonl`. The last good snapshot stays in memory and is
  persisted; a corrupt line or a `v` other than 1 shows the 1-based line number
  or an upgrade message and keeps the previous snapshot instead of blanking the
  backlog (`src/core/TaskSync.cpp:646-693`).
- Ranking uses `PriorityEngine::rank` with the compiled default weights and the
  device clock. The header says "Local ranking - default weights"; a persistent
  banner says the order may differ from a retuned desktop daemon.
- Refresh story: "Reload" re-parses the imported copy; "Choose file" replaces
  the copy with a newly exported `tasks.jsonl`. Desktop export, file transfer
  (USB, Syncthing, or any out-of-band file-sync tool) and the phone import are
  three explicit user actions. taskPilot performs no network I/O and no
  background sync.
- Identity is `uid` everywhere. Offline records have `id = 0`; there is no
  online get-by-uid method, so the mobile app never sends a write and never
  needs a local id.
- The mobile MVP shows status tallies only (open, in-progress, done, archived),
  labeled "from snapshot". The precise `overdue` and `due_within_24h` boundaries
  live in SQL today (`src/core/TaskStore.cpp:2361-2421`); they are omitted until
  a pure model helper is extracted, rather than reimplemented a second time in
  QML.
- The APK declares no `INTERNET` permission through a custom
  `AndroidManifest.xml` supplied via `QT_ANDROID_PACKAGE_SOURCE_DIR`. There is
  no listening socket and no outbound connection. The merged manifest is
  checked with `aapt2 dump permissions` once the SDK exists.

### 5.4 Limitations

- The JSONL export is a snapshot, not a stream; there is no push or subscription.
- The export does not carry ranking weights (`docs/sync.md:444-446`), so offline
  scores use default weights and can diverge from a retuned daemon.
- Read-only: add, edit, complete, delete, import and weights remain
  desktop-only.
- The export contains personal data (titles, notes, deadlines). It must not be
  bundled into the APK, committed to this repository, or written to shared
  storage.
- A newer export format version is refused by design, so an older app fails
  whole-file with an upgrade message; that must be a first-class UI state.
- A `content://` URI may not be readable by `QFile`; the safe path is a JNI
  `ContentResolver` copy into app-private storage, which is UNVERIFIED until the
  Android milestone spikes it.

### 5.5 Guarded path to an authenticated transport

If remote access is ever required, it gets its own design and review:

- **B0 — transport seam.** The phase-1 split puts `RpcResult` in
  `src/control/RpcTypes.hpp`, makes `taskpilot_client` build without
  `taskpilot_store` or SQLite, makes UUID entropy portable and leaves a small
  transport seam inside the client so TLS is additive rather than a fork.
- **B1 — server.** A new `RemoteServer` component, not a change to
  `JsonRpcServer`. It refuses to start without a token file, compares tokens in
  constant time, requires TLS 1.3, binds only an explicitly configured
  interface, caps request size, rate-limits, logs authentication failures
  without payloads, and reuses `Services::registry()` so the 15 method
  semantics are unchanged.
- **B2 — pairing.** Pairing needs a **new** local-only channel or method; the
  frozen 15-method catalog has none. That future phase explicitly amends the
  invariant: the desktop issues a one-time pairing code and a per-device token,
  the phone stores it in Android Keystore, the desktop can revoke devices, and
  tokens are stored hashed at rest. It is not done "over the existing loopback
  socket" without a protocol addition.
- **B3 — network choice.** Prefer Tailscale with the authenticated listener
  bound to the tailnet interface, so network and application authentication are
  independent; otherwise HTTPS with a public listener and strict TLS plus
  token. Qt 6.12 Android does not bundle OpenSSL, so any HTTPS choice adds
  per-ABI OpenSSL packaging (UNVERIFIED).

Hard invariants for any future work: the existing socket stays `127.0.0.1` by
default; `--bind` remains an explicit operator flag and is never exposed in a
UI; there is never an unauthenticated public listener; the new listener fails
closed; malformed or missing credentials are rejected before dispatch.

### 5.6 Forbidden

- Removing or changing the daemon's loopback default (`src/main.cpp:141`).
- Binding the daemon to `0.0.0.0`, `::`, a LAN address, or a tailnet address to
  serve the phone.
- Adding an unauthenticated public listener or proxy in front of the control
  socket.
- Pointing `ControlClient` at a non-loopback host from the UI, or offering a
  user-editable host field. The Settings dialog accepts `127.0.0.1`, `::1` and
  `localhost` only.
- Tailscale-only direct connection as a substitute for application
  authentication: it authenticates the path, not the caller, and would require a
  non-loopback bind.
- Sending tokens or task content in clear text, embedding credentials in the
  APK, or logging task text.
- Bundling or committing personal export data.

---

## 6. Test strategy

### 6.1 Preserved gates

Every milestone runs the existing gate unchanged, plus an explicit headless
configure:

```bash
./run.sh build
./run.sh test
cmake -B build-headless -S . -DTASKPILOT_UI=OFF
cmake --build build-headless
ctest --test-dir build-headless --output-on-failure
```

The headless configure proves Qt stayed optional; it must print
"taskPilot: UI disabled" only when Qt is absent or `TASKPILOT_UI=OFF`, and still
build `taskPilot` and pass the existing 369 CTest entries. The UI build pins the
kit:

```bash
cmake -B build-ui -S . -DCMAKE_PREFIX_PATH="$HOME/Qt/6.12.0/gcc_64" \
  -DTASKPILOT_REQUIRE_UI=ON
cmake --build build-ui
ctest --test-dir build-ui --output-on-failure
```

`tests/unit/test_control_client.cpp` keeps its client failure-mode tests
against the extracted client (10 `TEST()` cases; failure mode 4, a JSON-RPC
error reply, is also covered end to end in the roundtrip suite).
`tests/unit/test_services.cpp:178` keeps asserting `method_count == 15`.

New UI tests are registered in `tests/CMakeLists.txt` only, and only when
`TARGET taskpilot-ui-core` exists:

```cmake
if(TARGET taskpilot-ui-core)
    add_subdirectory(ui)
endif()
```

On a Qt-less machine the test set is exactly the current one.

### 6.2 Qt Test (C++)

- `tests/ui/tst_queue_model.cpp`: populate from a sanitized `get_queue` fixture
  captured once from a real daemon. Assert row count equals queue length, row
  order equals reply order, every role maps (`id`, `uid`, `title`, `status`,
  `importance`, `dueAt`, `blocks`, `tags`, `score`, `scoreParts`,
  `urgencyFactor`, `ageDays`), null `due_at` becomes `hasDue=false`, `score` is
  displayed as delivered and not re-summed, a reset preserves selection after
  `endResetModel()`, and an empty queue yields zero rows without error.
- `tests/ui/tst_task_filter_model.cpp`: single-tag and multi-tag OR semantics,
  order preservation under filtering, counts from the source model,
  case-insensitive search, empty result, clearing filters restores everything.
- `tests/ui/tst_control_worker.cpp`: fake newline-delimited JSON-RPC listener on
  port 0, reusing the pattern in `tests/unit/test_control_client.cpp`. Cases:
  success; `-32001` maps to `kNotFound`; refused port maps to daemon-offline
  (`ping` false); a `-32003` reply with a healthy ping maps to storage-error;
  malformed reply maps to `kInternal`; FIFO with one in-flight request;
  coalescing produces at most one follow-up refresh; event-loop latency stays
  under 100 ms while a fake server stalls 1 s per reply.
- `tests/ui/tst_offline_repository.cpp`: parse a fixture JSONL, rank with
  default weights, compare with a golden order from `PriorityEngine::rank`;
  tombstones are excluded from the queue; a corrupt line reports the 1-based
  line number; a `v=2` file is refused with an upgrade message; every parsed
  `id` is 0 and identity is `uid`.
- `tests/ui/tst_export_import_caps.cpp`: with a fake server returning more than
  1 MiB, the default-capped client fails and the 64 MiB export client succeeds;
  an import request above 1 MiB is written and answered under the default reply
  cap, proving the request path.

### 6.3 Qt Quick Test

- `tests/ui/qml/tst_queue_page.qml`: inject a stub queue model with titles A, B,
  C in that order and distinct scores; instantiate `QueuePage`; assert the
  delegates render in exactly A, B, C order and the score text equals the
  delivered values. This is the "sorting rendering" case in the requested
  sense: the daemon's ranked order renders unchanged, proving no JavaScript
  reordering. There is no client-side sort to test because `QueueModel`
  deliberately has no sort API.
- `tests/ui/qml/tst_tag_panel.qml`: selecting a tag reduces visible rows while
  preserving relative order; clearing restores all rows.
- Harness: `QUICK_TEST_MAIN`, `QUICK_TEST_SOURCE_DIR` pointing at the test QML
  directory, `QT_QPA_PLATFORM=offscreen`, `QT_QUICK_BACKEND=software`,
  `TIMEOUT 60`. The test executable hosts its own `TaskPilot.Ui` module built
  from the same QML file list and calls the same `registerUiTypes()`.

### 6.4 Offscreen render smoke

`tests/ui/ui_render_smoke.cpp` loads `Main.qml` with a stub backend, renders
`DesktopShell` at 1280x800 and `MobileShell` at 412x915, calls
`QQuickWindow::grabWindow()`, and asserts a non-null image of the expected size,
more than one distinct color, and the presence of stable `objectName`s for the
queue list, detail pane and status banner. It runs under the same offscreen
environment and is its own CTest entry with `TIMEOUT 60`.

### 6.5 Static and link guards

- `grep -rnE 'sort\\(|sortBy\\(|localeCompare' src/ui/qml` fails on any hit;
  the daemon owns the order.
- `grep -rn 'ControlClient' src/ui` fails outside `control_worker.*`; one
  blocking choke point only.
- `ldd build-ui/taskpilot-ui` contains no `libsqlite3`; the UI links no store.
- The Android manifest check asserts no `INTERNET` permission, and the Android
  link graph contains neither asio nor SQLite.
- A grep gate fails on task-content logging patterns; the export fixture lives
  outside the Qt resource system and is never packaged.

### 6.6 Manual checklist and exclusions

Manual release checks: daemon up, down and restarted mid-poll; coalesced
refreshes; export above 1 MiB and import above 1 MiB; import dry-run then apply;
tag filter and search; three-pane resize and narrow-window collapse; tray
available and unavailable; HiDPI 100/150 percent; every shortcut; Android
airplane mode, corrupt file, version mismatch, rotation and 48 dp targets.

Golden-image screenshot diffs are deliberately not automated: both Qt 6.10.2
(system packages) and Qt 6.12.0 (kit) are installed, and their Controls styles
differ, so pixel baselines would be a flake source. Structural assertions and the
render smoke cover the same risk.

---

## 7. Phased milestones

### 7.1 M0 — target split, Qt guard, empty window (Linux)

Work: add `src/control/RpcTypes.hpp`; switch `Services.hpp` and
`ControlClient.hpp` to it; split `taskpilot_result`, `taskpilot_model`,
`taskpilot_store`, the `taskpilot_core` interface and `taskpilot_client`; remove
`ControlClient.cpp` from `taskpilot_control` and link the client publicly; add
the guarded UI option and an empty `taskpilot-ui` window; add the
`tests/CMakeLists.txt` hook.

Acceptance:

- `./run.sh build` and `./run.sh test` are green with the existing 369 CTest
  entries.
- The headless configure above with `-DTASKPILOT_UI=OFF` builds the daemon and
  passes ctest. A second configure with CMake 3.20-3.24 must also survive
  without Qt.
- With `-DCMAKE_PREFIX_PATH="$HOME/Qt/6.12.0/gcc_64"
  -DTASKPILOT_REQUIRE_UI=ON`, the build produces `build-ui/taskPilot` and
  `build-ui/taskpilot-ui`, and the empty window opens.
- `ldd build-ui/taskpilot-ui | grep -i sqlite3` is empty.
- The diff is limited to the new header, `Services.hpp`, `ControlClient.hpp`
  and the build files; `Services.cpp`, `ControlClient.cpp` and
  `tests/unit/test_control_client.cpp` are untouched.
- Commits: the `RpcTypes` header, the CMake split and the Qt skeleton are three
  separate commits.

### 7.2 M1 — desktop MVP (Linux)

Work: `ControlWorker`/`QThread`, `AppController`, `QueueModel`,
`TaskFilterModel`, `DesktopShell` three-pane `SplitView`, detail with score
explanation, tag pane, search, add/update/complete/reopen/delete, history,
export/import dialogs, weights editor, diagnostics page (`get_status`,
`describe_methods`), connection and error states, tray, shortcuts, `QSettings`
geometry, and the Qt Test/Quick Test/offscreen smoke suites.

Acceptance:

- With the daemon running, queue titles appear in exactly the same order as
  `./run.sh cli` `queue` for the same snapshot; a fixture test pins the order.
- With the daemon stopped, the offline banner appears within one poll and the
  window stays interactive (the event-loop latency test is the evidence).
- Tag filter and search remove rows without reordering them; counts come from
  the unfiltered model.
- Every one of the 15 methods is reachable through a documented mapping, with
  no UI-only action. `import_tasks` is exposed as a two-step flow: a dry run
  renders the merge report, and only then can the user apply it.
- Each mutation issues exactly one RPC and one follow-up refresh; a failed call
  leaves the model unchanged; delete requires confirmation; there is no undo
  affordance.
- Export of a generated backlog above 1 MiB succeeds; import of a request above
  1 MiB succeeds and shows the dry-run merge report before apply.
- Changing weights re-ranks the queue because the daemon reply changes; the UI
  never recomputes a score.
- A refused connection shows "daemon not running"; a fake `-32003` reply with a
  healthy ping shows "storage error"; the two states are visually and testably
  distinct.
- Qt Quick tests for queue mapping and rendered order pass offscreen.

### 7.3 M2 — Windows MSVC compile verification (conditional)

Work: add an MSVC warning branch; port `src/core/Uuid.cpp` entropy behind
`_WIN32`; replace `poll()` in `src/control/JsonRpcServer.cpp:469-473` with a
portable wait (WSAPoll/select or an asio synchronous wait); add a
`localtime_r`/`localtime_s` wrapper for `src/control/Repl.cpp`; guard the POSIX
`mkstemp` use in `tests/integration/test_daemon_roundtrip.cpp`; settle
dependency provisioning for asio, nlohmann_json, SQLite and GTest; install Qt
6.12 `msvc2022_64` through the Qt Online Installer.

Acceptance:

- Visual Studio 2022 x64 Release configure and build produce `taskPilot.exe` and
  `taskpilot-ui.exe`.
- The UI launches and renders the queue against a daemon on the same machine.
- No POSIX header is reachable from the Windows build; the tests are guarded or
  the build uses `-DBUILD_TESTING=OFF`.
- Compile and launch only; ctest is not required. The milestone is UNVERIFIED
  until actually run, and it cannot start until the dependency-policy decision
  in 8.2 is made.

### 7.4 M3 — Android toolchain and empty window

Work: install a supported JDK (`openjdk-17-jdk` or `openjdk-21-jdk`; JDK 25 is
not Qt-tested), the Android command-line tools, `sdkmanager` packages and
licences, the NDK, CMake 3.25+, and the Qt 6.12 `android_arm64_v8a` kit; export
`ANDROID_SDK_ROOT`; add a custom manifest without `INTERNET`; pin the SDK
platform because the Qt helper's default may be newer than the installed one.

Candidate versions from the toolchain research (all UNVERIFIED until resolved
against the Qt 6.12 Android documentation at M3): SDK platform `android-36`,
build-tools `36.0.0`, NDK `27.2.12479018`, CMake `3.31.6`. The installed kit's
Android config currently declares only `Java 1.8`, so the exact JDK requirement
must be verified rather than assumed.

Acceptance:

- An arm64-v8a APK is produced (`cmake --build build-android --target apk`),
  installs on a device or emulator, opens a window showing the app name, and
  does not crash.
- `aapt2 dump permissions` shows no `INTERNET`; `adb logcat` is clean.
- An arm64 APK needs an arm64 device; an x86_64 emulator needs the x86_64 Qt kit
  or a multi-ABI build.
- End-to-end configure, build and run is UNVERIFIED until performed.

### 7.5 M4 — Android offline read-only features

Work: `MobileShell` with `StackView`, bottom tabs, custom pull-to-refresh, 48 dp
sizing, SAF file import copied into app-private storage, `OfflineRepository`,
queue model reuse, tags page, detail page, error and version-mismatch states,
rotation and packaging.

Acceptance:

- A fixture `tasks.jsonl` loads; live records appear and tombstones do not.
- Order equals `PriorityEngine::rank(records, default weights, now)` from a
  host-side reference vector.
- A corrupt line and a `v=2` file each produce an error state with the line
  number or upgrade message while the last good snapshot stays visible.
- Pull-to-refresh re-reads the imported copy; bottom tabs navigate; back pops
  the stack; all interactive controls are at least 48 dp; airplane mode works.
- There is no write control anywhere, no network permission, no asio and no
  SQLite in the link.
- Host-side Qt tests for `OfflineRepository` pass; on-device behavior is a
  manual checklist.

### 7.6 Feasibility and effort

Environment verified locally on 2026-11-05: Qt 6.12.0 at
`$HOME/Qt/6.12.0/gcc_64` with Qt6Quick, Qt6QuickControls2, Qt6QuickTest and
Qt6LabsPlatform; `qmltestrunner` and the offscreen platform plugin are present;
CMake 4.2.3; JDK 25.0.4.1; 423 GB free; system Qt 6.10.2 is also installed, so
commands must pin `CMAKE_PREFIX_PATH` or use `qt-cmake`. Android SDK, NDK and the
Qt Android kit are absent; aqtinstall is not on PATH; there is no
MaintenanceTool; `openjdk-17-jdk` and `openjdk-21-jdk` are apt candidates.

| Milestone | Scope | Engineer-days | Confidence |
|---|---|---|---|
| M0 | Toolchain check, library split, Qt guard, empty window | 2-3 | High |
| M1 | Desktop MVP (Linux), read and write | 8-12 | High |
| M2 | Windows MSVC compile verification | 4-6 | Low: no MSVC locally, dependency policy unresolved (UNVERIFIED) |
| M3 | Android toolchain and empty window | 2-3 | Medium: toolchain absent, but a Qt Android kit resolves it |
| M4 | Android offline read-only features | 5-8 | Medium-low: SAF and device behavior (UNVERIFIED) |
| **Total** | Phase 1 | **21-32** | |

With 25 percent contingency the full scope is roughly 26-40 engineer-days. The
Linux desktop MVP (M0+M1) is 10-15 days, or 2-3 weeks of calendar time including
review and gate runs. The full scope is 5-8 weeks for one developer. Two
developers can overlap after M0: one continues desktop work while the other
installs the Windows and Android toolchains.

### 7.7 Commit plan

| Milestone | Commits |
|---|---|
| M0 | 1) `RpcTypes` header; 2) CMake library split with the existing gate green; 3) Qt guard and empty window |
| M1 | 1) worker/controller; 2) queue and filter models; 3) desktop shell and detail; 4) write commands; 5) export/import flow; 6) tests and guards |
| M2 | 1) portability wrappers; 2) MSVC warning branch; 3) dependency provisioning documentation; 4) build fixes |
| M3 | 1) toolchain setup script/documentation; 2) Android manifest and Qt project plumbing; 3) empty window |
| M4 | 1) offline repository; 2) mobile shell; 3) SAF import; 4) tests and packaging |

Each commit runs the gate and does not mix refactoring with behavior changes.

---

## 8. Risks and open questions

### 8.1 Risks

- **GUI freeze.** `ControlClient::call` can block for about 15 s with no
  cancellation. Mitigation: one worker thread, FIFO queue and an event-loop
  latency test against a stalling fake server.
- **Header and SQLite leakage.** `ControlClient.hpp` currently includes
  `Services.hpp` for `RpcResult`, dragging `Clock.hpp` and `TaskStore.hpp` into
  every UI translation unit; linking `taskpilot_core` would pull SQLite PUBLIC.
  Mitigation: the self-contained `RpcTypes.hpp`, the `taskpilot_result` split
  and the `ldd` guard.
- **Link-line breakage.** Splitting `taskpilot_control` changes a link line used
  by both the executable and the single test target. Mitigation: keep alias
  names, link the client PUBLIC from control, and run the 369-test gate before
  and after the CMake-only commit.
- **Polling churn.** There is no server push; each refresh is a new TCP
  connection and a daemon session thread, with roughly 8 MB of address space
  retained until reaped. Mitigation: 10 s visible / 30 s hidden polling, one
  in-flight request, coalescing, and pause when the application is inactive.
- **`kStorageFailure` ambiguity.** Cannot-connect and a real storage fault share
  the same family. Mitigation: the connect-only `ping()` preflight and two
  distinct UI states.
- **Export cap.** The default 1 MiB cap makes `export_tasks` fail permanently
  once the backlog grows past it. Mitigation: a dedicated 64 MiB client plus a
  regression test. Import is bounded on the reply side but unbounded on the
  request side; it is read and serialized on the worker thread and measured.
- **Client-side ranking temptation.** Mitigation: `QueueModel` has no sort API,
  the desktop UI links no `PriorityEngine`, a QML grep fails on
  `sort`/`sortBy`/`localeCompare`, and a Quick Test asserts rendered order equals
  reply order.
- **Missing filter features.** `get_queue` has no tag parameter, text search or
  offset. Mitigation: order-preserving client-side filtering, counts from the
  source model, and a "showing first N" label instead of fake pagination.
- **Windows blockers.** POSIX `poll` in `JsonRpcServer.cpp`, `localtime_r` in
  `Repl.cpp`, POSIX entropy in `Uuid.cpp`, `mkstemp` in an integration test, the
  GCC-only warning flags, and the unresolved dependency policy. Mitigation: the
  M2 portability work and an explicit dependency decision; UNVERIFIED until
  compiled.
- **Android toolchain absence.** No SDK, NDK, Android Qt kit, aqtinstall or
  MaintenanceTool; JDK 25 is installed. Mitigation: install a supported JDK and
  the pinned SDK/NDK; treat configure/build/run as UNVERIFIED until M3.
- **Qt version ambiguity.** Plain `find_package` resolves system Qt 6.10.2 while
  the project targets 6.12.0. Mitigation: require 6.12 in `find_package`, pin
  `CMAKE_PREFIX_PATH` in every documented command, and keep golden images out of
  the test plan.
- **Offline divergence.** The export carries no weights and is a snapshot; the
  parser is all-or-nothing. Mitigation: label offline scores as
  local/default-weight, keep the last good snapshot, and surface line numbers
  and upgrade messages.
- **Tray availability.** GNOME/Wayland tray support is UNVERIFIED. Mitigation:
  `SystemTrayIcon.available`, no icon when false, close-to-tray off by default.
- **`run.sh` scope.** It builds all targets on every invocation and only watches
  `src`, `tests` and `CMakeLists.txt`. Mitigation: UI sources live under
  `src/ui`; consider a separate build directory for UI work.
- **Personal data.** Titles and notes are personal. Mitigation: synthetic
  fixtures only, no export in the repository or APK, no task text in logs,
  app-private Android storage with `allowBackup=false`.
- **Licensing.** The repository is GPL-3.0 and Qt is LGPL-3/GPL-3. Mitigation:
  state the distribution assumption and keep Qt dynamically linked for LGPL
  compliance.

### 8.2 Open questions

1. **Windows dependency provisioning.** The repository forbids vcpkg and
   vendored `third_party`. Is a developer-only dependency prefix outside the
   repository acceptable, or must prebuilt system packages be used? This blocks
   M2.
2. **Windows portability scope.** Should the `poll`/`localtime_r` wrappers and
   the `Uuid.cpp` split land in M2 only, or should the portable text helpers be
   split in M0 so the model library is already portable? Deferring keeps M0
   smaller but leaves MSVC broken longer.
3. **Offline stats.** Extract pure tally helpers from `TaskStore.cpp:2361-2421`
   into `taskpilot_model`, or ship only the four status tallies in the mobile
   MVP? Extracting helpers creates one authority; the MVP can ship without them.
4. **Android refresh UX.** Is manual file replacement enough, or should the app
   read from a synced private folder (for example a Syncthing-managed directory)
   through SAF? The latter adds no network code to taskPilot but changes the
   import flow.
5. **Tray behavior on GNOME/Wayland.** If `available` is false, hide the setting
   entirely or show a disabled control with an explanation?
6. **Interactive per-step timeout.** Keep the client default 5 s, or use 2.5 s
   for UI calls and a longer budget for export/import? The choice affects
   perceived responsiveness versus false timeouts.
7. **Android ABI and minimum API.** arm64-v8a only, or a multi-ABI build for
   x86_64 emulators, and what minSdk? Qt 6.12's supported API range must be
   verified at M3.
8. **Import exposure.** This design resolves it in favor of exposing
   `import_tasks` behind a dry-run report and explicit apply, keeping the
   15-method mapping complete. If reviewers prefer import to stay CLI-only, the
   M1 acceptance criterion must drop to 14/15.
