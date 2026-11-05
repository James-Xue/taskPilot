# taskPilot desktop UI — design

Status: implementation started. Scope is the Linux desktop client; Android is
deferred (section 6). The daemon protocol, the 15-method catalog and the SQLite
store are frozen.

## 1. Goals and non-goals

Goals:

- A desktop client for the ranked queue: list, detail, tag filter, search,
  add/complete/reopen/delete, live connection state and error banners.
- The daemon stays the only writer and the only ranker. The UI renders the
  `get_queue` order and the `score` value verbatim and never re-sums the four
  score parts.
- `ControlClient` becomes a reusable `taskpilot-client` library shared by the
  REPL and the UI.
- The headless build keeps working on a machine without Qt.

Non-goals for this phase: protocol changes, client-side ranking, undo, server
push, import/export UI, weights editing, tray icon, Android, Windows runtime
verification.

## 2. Module split

```
taskpilot_result   Result.cpp                     errors only
taskpilot_model    Task, Weights, Clock,
                   PriorityEngine, TaskSync, Uuid pure C++20, nlohmann only
taskpilot_store    TaskStore.cpp                  SQLite
taskpilot_core     INTERFACE = model + store      alias taskPilot::core kept

taskpilot_client   ControlClient.cpp              asio, no SQLite
taskpilot_control  Services, JsonRpcServer,
                   McpServer, Repl                PUBLIC core + client
taskPilot          executable                     unchanged
taskpilot-ui       Qt Quick app                    PUBLIC ui-core + client
```

`src/control/RpcTypes.hpp` is new and self-contained (`Result<nlohmann::json>`
plus `<nlohmann/json.hpp>`); `Services.hpp` and `ControlClient.hpp` include it,
so the UI does not inherit `Clock.hpp` or `TaskStore.hpp` through the client.
Each `.cpp` is compiled into exactly one target.

The UI targets:

- `taskpilot-ui-core` — `AppController`, `QueueModel`, `TaskFilterModel`
  (Qt Core/Gui, links `taskPilot::result` only).
- `taskpilot-ui-desktop` — `ControlWorker` (Qt Core, links `taskPilot::client`).
- `taskpilot-ui` — `main.cpp` + QML resources, links the two above; `ldd` must
  show no SQLite.

Qt is optional: `TASKPILOT_UI` defaults ON but degrades to a STATUS message when
CMake < 3.25 or Qt 6.12 is missing; `TASKPILOT_REQUIRE_UI=ON` turns that into a
configure error. The UI build pins `CMAKE_PREFIX_PATH=$HOME/Qt/6.12.0/gcc_64` so
system Qt 6.10.2 never wins.

## 3. Data flow

`ControlClient::call` is blocking (5 s per connect/write/read step) and must not
run on the GUI thread. One `ControlWorker` lives on a dedicated `QThread`;
`AppController` invokes it through queued signals, at most one request in
flight. The worker converts the reply to compact UTF-8 `QByteArray`; the GUI
thread parses it once with `QJsonDocument`.

`get_queue` returns `{now, weights, queue:[...]}`; each row is the task fields
plus `score`, `score_parts`, `urgency_factor`, `age_days`. `QueueModel` stores
rows in reply order and exposes id/uid/title/status/importance/due/blocks/tags/
notes/score/scoreParts/urgencyFactor/ageDays/dueText/hasDue roles, plus
`Q_INVOKABLE QVariantMap get(row)`. There is no sort method.

`TaskFilterModel` is an order-preserving `QSortFilterProxyModel`: OR semantics
over selected tags, case-insensitive title/notes search, counts from the source
model. Selection is restored by id after `endResetModel()`.

Mutations are one RPC each; success triggers a refresh, failure leaves the model
untouched. Errors are values (`kInvalidArgument`, `kNotFound`, `kConflict`,
`kStorageFailure`, `kInternal`). A failed call is classified with the
connect-only `ping()`: `kStorageFailure` + ping false is "daemon not running";
`kStorageFailure` + ping true is "storage error".

## 4. Desktop layout

```
+----------------------------------------------------------------------------+
| [Add]  [Refresh]  [Search.................]   Connected / Offline  [Tags]  |
+---------------------+--------------------------------+---------------------+
| Queue · N           | Detail                         | Filters             |
| Ranked by the daemon| Title                          | [search]            |
| 1. Task A   77.00   | status · importance · due      | [x] work       4    |
| 2. Task B   47.71   | notes                          | [ ] home       3    |
| 3. Task C   41.57   | Score 77.00                    | [ ] errand     1    |
|                     | parts: imp/urg/age/blocks      | Weights: 5/30/2/8   |
|                     | [Complete] [Delete]            |                     |
+---------------------+--------------------------------+---------------------+
| open 12 · in progress 3 · overdue 2 · updated 12:04:11 · top: Task A       |
+----------------------------------------------------------------------------+
```

Horizontal `SplitView`: queue 380/280, detail fill/360, tags 260/200. Shortcuts:
Ctrl+N add, F5/Ctrl+R refresh, Ctrl+F focus search, Space complete/reopen,
Delete delete with confirmation, Esc clear search. Material dark theme.

Write flow: add dialog (title, importance, due date `yyyy-MM-dd`, blocks, tags,
notes) -> `add_task`; Complete -> `complete_task`; Reopen -> `reopen_task`;
Delete -> confirmation -> `delete_task`. Every action goes through one method
call and one refresh.

## 5. Tests and milestones

Preserved gates: `./run.sh build`, `./run.sh test` (369 CTest entries), and a
headless configure with `-DTASKPILOT_UI=OFF`.

New tests, registered in `tests/CMakeLists.txt` only when the UI target exists:

- `tst_queue_model` — parse a `get_queue` fixture; assert row order equals reply
  order, role mapping, `score` is displayed as delivered and not re-summed, an
  empty queue yields zero rows.
- `tst_filter_model` — tag OR semantics, search, order preservation, counts.
- `tst_queue_page.qml` (Qt Quick Test, offscreen) — a stub model with A, B, C
  renders in exactly A, B, C order; `grabImage` of the shell is non-null, has
  the expected size and more than one colour (offscreen render smoke).
- Static guards: no `sort`/`sortBy`/`localeCompare` under `src/ui/qml`; no
  `ControlClient` outside `control_worker`; `ldd taskpilot-ui` has no sqlite3.

M0 — library split, Qt guard, empty window. Acceptance: headless build/test
green; UI build produces `taskpilot-ui`; `ldd` clean.

M1 — desktop MVP. Acceptance: with the daemon running, the queue order matches
`./run.sh cli` `queue`; with it stopped, the offline banner appears and the
window stays interactive; add/complete/reopen/delete work; tests pass.

M2 — Windows MSVC compile verification (report only). MSVC 2022 and Qt 6.12
`msvc2022_64` are required; this machine has neither, and the daemon also uses
`poll()` and `localtime_r()`, which need portability wrappers. No code is
claimed to compile there until that work is done.

## 6. Deferred: Android

Android is out of scope for this phase. The portability seams kept now are the
`taskpilot-client` library, the pure `taskpilot-model` library, and QML files
separate from platform code. A future phase will choose between an offline
read-only app over `tasks.jsonl` and an authenticated remote transport; the
control socket stays loopback-only and unauthenticated in either case. No Qt for
Android kit, SDK, NDK or remote-auth design is part of this work.
