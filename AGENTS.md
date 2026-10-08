# AGENTS.md — AI Coding Assistant Guidelines

> This file tells AI coding assistants (Claude Code, GitHub Copilot, Cursor, etc.)
> how to work effectively within the taskPilot codebase.

---

## Project in 30 Seconds

**taskPilot** is a C++20 work-priority backend. You put tasks into it; it answers
one question well — *what should be worked on next?* — and an LLM can drive the
whole thing over MCP.

It is deliberately small in scope. A task carries exactly four ranking inputs
(importance 1..5, an optional deadline, its age, and how many downstream things
it blocks). There are no projects, no subtasks, no dependency graph, no
recurring tasks, and no notifications. Reading `docs/scoring.md` is the fastest
way to understand what the product actually does.

```
L2 control   Services (catalog + handlers)  JsonRpcServer  ControlClient
             McpServer  Repl
                 |
                 v   depends on (never the reverse)
L1 core      Task  Weights  PriorityEngine  TaskStore (SQLite)  TaskSync
             Uuid  Result/Error  Clock
```

**Key property**: the ranking math is pure and the daemon is the only writer.
`PriorityEngine` is a set of static functions over `(task, now, weights)` — no
clock, no I/O, no storage — so it is deterministic and testable to the last
float. `TaskStore` is the only thing that touches SQLite, and it knows nothing
about ranking, JSON-RPC, or MCP.

**Second key property**: there is exactly one binary. `serve` (the daemon) and
`mcp` (the stdio bridge) are the same executable, so the tool list the MCP host
sees can never be from a different version than the daemon answering it.

**Identity rule**: a task has two identities and they are not interchangeable.
`uid` — a version-4 uuid, assigned once by the store and never changed — is the
**cross-machine** identity: it is what an export carries and what a merge
matches on. `id` (`INTEGER AUTOINCREMENT`) is a **local row number** — *"the
ninth row this database created"* — so two machines both have a task 9, and the
same task has a different number on each. Use `id` for this machine's REPL and
control calls (`done 7`), `uid` for anything that crosses machines. No export
carries `id`, deliberately: a transported integer id invites a reader to match
on it, which is the bug the uuid exists to prevent.

**Cross-machine sync**: the backlog travels as a JSONL export — one record per
line, sorted by uid, deletions kept as tombstones — committed to a separate
**private** data repository (`taskPilot-data`), and merged back in with
last-write-wins over the record's stamp. `TaskSync` holds the format and the
complete decision table as pure code (no database, no clock); `TaskStore`
applies it inside one transaction. The specification is `docs/sync.md`.

---

## Build & Run

Dependencies are **system packages only** — no vcpkg, no `third_party/`:

```bash
sudo apt install nlohmann-json3-dev libasio-dev libsqlite3-dev libgtest-dev
```

```bash
./run.sh build            # configure + compile (cmake -G Ninja)
./run.sh test             # build, then ctest --output-on-failure
./run.sh serve            # the daemon, foreground (also the default subcommand)
./run.sh cli              # interactive REPL attached to a running daemon
./run.sh mcp              # stdio MCP bridge (what an MCP host spawns)
./run.sh version
```

Manual equivalent, when you need direct control over the build tree:

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Compiler warnings are strict and on by default:
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`.
A warning is a build failure; use an explicit `static_cast` where a narrowing
conversion is provably safe rather than loosening the flags. Configure with
`-DTASKPILOT_WERROR=OFF` only while investigating an existing warning.

Environment variables:

| Variable | Meaning |
|---|---|
| `TASKPILOT_DB` | Database path. `run.sh` exports `<repo>/data/taskpilot.db` unless it is already set. |
| `TASKPILOT_PORT` | Control socket port, read by the binary. Default `8091`. |
| `TASKPILOT_BIND` | Bind address for `serve` (connect address for `mcp` and `cli`). Default `127.0.0.1`. |
| `TASKPILOT_BUILD_DIR` | Directory `run.sh` builds into. Default `build`. Read by the script only, never by the binary. |

Precedence for the first three, lowest to highest: built-in default, then the
environment, then the `--db` / `--port` / `--bind` flags. A value that is
present but malformed is reported, never silently replaced by the default.

The `data/` directory is **not** created by the build, and `TaskStore::open`
passes the path straight to SQLite, which will not create a missing parent
directory. Nothing has to be done by hand: `main.cpp`'s `serve` path creates the
parent directory before opening the store, so the first `./run.sh serve` makes
`data/` itself. Only code that opens a `TaskStore` without going through `serve`
has to create it.

## Run Modes

| Command | What it is | Lifetime |
|---|---|---|
| `./run.sh serve` | The daemon: opens the store, applies the schema, binds the control socket, serves JSON-RPC. Default when no subcommand is given. | long-lived |
| `./run.sh mcp` | stdio MCP bridge. Owns no store; forwards every `tools/call` to the daemon through `ControlClient`. | one per MCP host session |
| `./run.sh cli` | Interactive REPL over the control socket. | as long as you keep it open |
| `./run.sh export [--out <path>]` | Write the whole backlog as JSONL — stdout by default, or a file with `--out`. Goes through the control socket like `cli`/`mcp`, so the daemon must be running. | one shot |
| `./run.sh import [--file <path>] [--apply]` | Merge a JSONL export (file, or stdin) into the backlog. **Dry run unless `--apply` is given.** Also through the control socket. | one shot |
| `./run.sh version` | Print the version. | one shot |
| `./run.sh build [--clean]` / `test` / `clean` | Build helpers in `run.sh`; not subcommands of the binary. | — |

**One daemon per control port.** `JsonRpcServer::start()` fails with a message
naming the address when the port is already taken, which is the normal way a
second `serve` announces that one is already running. Only the port is locked: a
second daemon on another port would happily open the same database file, so give
it its own `--db` if you ever run two. `mcp` and `cli` are clients and must never
open the store themselves — if either one wrote to SQLite directly, the daemon's
view and the file would drift with no reconciliation.

**Both clients are stateless.** They open a connection per call and hold
nothing. If the daemon restarts, the next call reconnects; there is no recovery
logic to get wrong.

---

## Rule #1 — English Only

**All documentation, code comments, log messages, commit messages, and
identifiers must be written in English.** Examples in documentation included —
no Chinese anywhere in `src/`, `tests/`, `docs/`, or git history.

---

## Rule #2 — The Method Catalog Is the Single Source of Truth

`Services::methodSpecs()` in `src/control/Services.cpp` is the **only** place an
operation is declared. Everything else derives from it:

- `Services::registry()` builds one registry entry per spec, and every entry
  routes back through `Services::invoke()`.
- `McpServer::toolDefinitions()` builds `tools/list` from the same vector.
- `describe_methods` returns it, and the REPL's `methods` command shows it.

Adding a method is therefore three edits, **all inside the control layer**:

1. Append a `MethodSpec` (name, description, JSON Schema) to `methodSpecs()`.
2. Add the handler: declare `handleXxx` in `Services.hpp`, define it in
   `Services.cpp`.
3. Add one row to the dispatch table inside `Services::invoke()`.

It is then live on both transports at once. **Never hand-copy the tool list into
`McpServer.cpp`** — a duplicated list is the standing drift bug that
`Services.hpp` cites from pulseTrader and exists here to avoid. Method names are
the MCP tool names verbatim; one vocabulary everywhere.

Note the deliberate consequence: `tools/list` works even when the daemon is
down, because the catalog is static data compiled into this binary. A call then
fails with a clear connection error instead of the host seeing an empty tool
list that looks like a broken install.

---

## Rule #3 — In the `mcp` Process, stdout Is the Protocol

The stdio MCP transport is newline-delimited JSON on stdin/stdout. **One stray
byte on stdout — a log line, a banner, a debug print — corrupts the framing and
the host drops the server.** Therefore:

- `std::cout` is off-limits. Diagnostics go to `std::cerr`, in every process,
  including the daemon and the REPL where there is no protocol to protect —
  one rule is easier to keep than a rule with exceptions.
- `McpServer::handleLine()` never appends a newline; `run()` appends exactly one
  per answered response and flushes. A notification gets no line at all.
- `run.sh` writes all of its own progress output to stderr for the same reason,
  so a rebuild triggered by `./run.sh mcp` cannot pollute the stream.
- When debugging a bridge, redirect stderr to a file; never merge it into
  stdout.

The one intentional stderr line in `McpServer.cpp` is the `server/discover`
reply trace: it records the version list the server advertised, so a live
client's probe can be observed without a packet capture. The reply's shape is
confirmed against the reference SDK, not guessed — see below and
`docs/mcp.md`.

---

## Protocol Changes — Read the Reference SDK, Do Not Guess

The MCP wire shapes are not written from memory. They are taken from the
reference implementation, which is installed on this machine at
`~/.npm-global/lib/node_modules/mcporter/node_modules/@modelcontextprotocol/`
(packages `core`, `client` and `server`, version 2.0.0; find the tree elsewhere
with `find / -maxdepth 8 -type d -path '*node_modules/@modelcontextprotocol*'`).
`DiscoverResultSchema` in `core`/`client` and `UnsupportedProtocolVersionError`
in `client` are the authority: `src/control/McpServer.cpp` must emit exactly the
key names those schemas name, and `docs/mcp.md` documents them.

Any change to — or review of — the protocol layer starts by reading those
schemas. A guessed key name fails **silently**, which is why this is a rule
rather than a preference: a client that cannot parse the `server/discover`
reply, or cannot read `supported` out of the `-32022` error data, concludes the
server is not modern, falls back to `initialize`, and reports nothing. The bug
then presents as "the modern path never engages" with no line in any log to
explain it — so the shape has to be checked against the SDK, not inferred from
the spec prose.

---

## Coding Conventions

| Rule | Detail |
|------|--------|
| **Language** | C++20. No C++23 — `Result<T>` exists precisely because `std::expected` is C++23 and raising the standard for one type is not worth it. |
| **Namespace** | Everything in `taskpilot`. There are no sub-namespaces; private file-local helpers go in an anonymous namespace inside the `.cpp`. |
| **Headers** | `#pragma once`. |
| **File naming** | Filename matches the primary class: `TaskStore.hpp` / `TaskStore.cpp` for `class TaskStore`. A module that is a coherent set of related types (`Result.hpp`, `Task.hpp`, `Weights.hpp`) keeps the name of the type that gives it its theme. |
| **Include style** | `"core/Result.hpp"` / `"control/Services.hpp"` for project headers (paths are relative to `src/`), `<nlohmann/json.hpp>` for third-party. |
| **Error handling** | Return `taskpilot::Result<T>` / `taskpilot::Status`. An **expected** failure — bad input, not found, I/O — is an `Error` value, never an exception. Exceptions are reserved for genuine programming errors (the standard library's own, e.g. `std::bad_variant_access`). Never swallow an error. |
| **Logging** | There is no logging framework. Diagnostics go to `std::cerr`; `std::cout` is off-limits (Rule #3). |
| **Thread safety** | `PriorityEngine` is pure and stateless; `TaskStore` serializes every public method with one `mutable std::mutex` (mutable so `const` queries can lock too); `Services::invoke()` is safe to call concurrently because it holds no mutable state. Document the guarantee in the header comment of anything shared. |
| **Naming** | Classes `PascalCase`; methods `camelCase`; private members `m_camelCase`; constants `kPascalCase`; enum values `kPascalCase` (`kOpen`, `kNotFound`, `kInvalidArgument`); pure-data struct fields `snake_case` (`importance`, `due_at`, `created_at`). |
| **Braces** | **Always**, even for a single-statement `if`/`for`/`while`. Never `if (x) return;` on one line. |
| **Yoda conditions** | Constant on the **left**: `if (0 == status)`, `if (nullptr == ptr)`. |
| **Comments** | Required, and they must explain **why**. Never restate the code. For multi-step logic use a numbered list with line breaks — copy the house style from any header in `src/core/`. |

### Comment Style

Every non-trivial function and class carries a comment that says what it is for,
what it assumes, and what it must never do. Multi-step logic is a numbered list:

```cpp
// Resolve the deadline the client asked for:
// 1. `due_at` present      -> use it as-is (epoch seconds, UTC).
// 2. `clear_due_at` true   -> set the column to NULL.
// 3. Both present          -> kInvalidArgument: the two together are always a
//                             caller bug, so a silent precedence rule would
//                             hide it instead of reporting it.
// 4. Neither present       -> leave the column untouched.
```

Decision trees with more than two branches, every lock or atomic, and every
error path get the same treatment. A comment that only re-states the code
(`// increment the counter`) is worse than no comment: it invites the reader to
skim past the ones that matter.

---

## Layer Boundaries

Two layers, one direction of dependency.

- **L1 `core`** — `Task`, `Weights`, `PriorityEngine`, `TaskStore`, `TaskSync`,
  `Uuid`, `Result`, `Clock`. It knows nothing about sockets, JSON-RPC, MCP, or
  the daemon. Concretely: `TaskStore` does not rank and does not know what a
  "queue" is (it returns an unordered set); `PriorityEngine` does not store and
  does not read the clock (it takes `now` as a parameter); `TaskSync` is pure —
  no database, no clock, no I/O — so the whole merge decision table is testable
  without a store.
- **L2 `control`** — `Services` (the API surface), `JsonRpcServer` (the daemon's
  socket), `ControlClient`, `McpServer` (the stdio bridge), `Repl`.

`control` includes `core/...`. `core` must **never** include `control/...`.
CMake links the libraries one way (`taskpilot_control` links
`taskpilot_core`), but nothing mechanically stops a stray include from
compiling, so the direction is a review rule, not a build error.

When adding a capability, ask which layer owns it:

- Ranking rule or a new term -> `core/PriorityEngine.cpp`, plus
  `docs/scoring.md` and a unit test that fails if the term is dropped.
- A new column or query -> `core/TaskStore.cpp` (the store owns SQL, and only
  the store).
- A new operation, parameter, or wire shape -> `control/Services.cpp` (see
  Rule #2).
- A transport concern (framing, timeouts, protocol versions) -> the transport
  file, never `Services`.
- A change to the export format or the merge rule -> `core/TaskSync.cpp`, plus
  `docs/sync.md` and a decision-table test. The format is a contract between two
  machines that are never updated at the same moment, so a change must either
  keep an older reader safe or bump `v` — and an older reader then refuses the
  file instead of misreading it.

---

## Testing

- **Unit tests** live in `tests/unit/test_<module>.cpp`: GTest, no network I/O.
  An in-memory SQLite store (`":memory:"`) is allowed and is how the store tests
  run.
- **Integration tests** live in `tests/integration/test_<flow>.cpp` and may bind
  a real socket — on **port 0**, so the OS picks a free port and the suite never
  collides with a running daemon.
- New test files must be added to `tests/CMakeLists.txt`; that list is the only
  registration point.
- `gtest_discover_tests` registers one CTest entry per `TEST()`, so a single
  case is `ctest --test-dir build -R <CaseName>` and a failure names the case.
- **Every new code path needs a unit test.** A scoring change needs a test that
  fails loudly if its term stops contributing — `test_priority_engine.cpp`
  asserts the aging term directly, because a "simplification" that drops it
  would otherwise look fine in every other test.
- The protocol dispatch paths (`JsonRpcServer::dispatchLine`, `McpServer::handleLine`,
  `Repl::executeLine`) are static or take an injected caller/backend precisely so
  they can be tested without a socket. Keep them that way.

---

## Things to Avoid

1. **Don't add a dependency.** The whole set is nlohmann/json, SQLite, standalone
   asio, and GTest — all apt packages. No vcpkg, no vendored `third_party/`, no
   Boost, no new HTTP or JSON library.
2. **Don't write to stdout.** In the `mcp` process it corrupts the protocol
   stream (Rule #3); everywhere else it is still the wrong place for a
   diagnostic — use `std::cerr`.
3. **Don't hand-copy the tool list** into `McpServer.cpp` or anywhere else
   (Rule #2).
4. **Don't reach for C++23.** `std::expected`, `std::print`, and friends are out;
   `Result<T>` and `std::cerr` are the house answers.
5. **Don't let the ranking math touch the world.** No clock reads, no SQL, no
   file I/O inside `PriorityEngine`. If it needs the time, it takes `now`.
6. **Don't add a second ordering authority.** `TaskStore::listTasks` returns rows
   unordered on purpose — resist adding `ORDER BY score` to SQL. Ordering is the
   engine's job, and a storage-level sort would silently disagree with the
   documented tie-breakers.
7. **Don't break the total order.** `PriorityEngine::rank` ties break on
   `created_at` then `id` so the queue is reproducible. A comparator that
   depends on input order is a bug even when it looks stable in practice.
8. **Don't bind anywhere but loopback.** See below.
9. **Don't throw for an expected failure.** Not-found, invalid input, and I/O
   faults are `Error` values; a throw across the RPC boundary would be an
   unhandled exception in the daemon, not an error the caller can read.
10. **Don't commit an export into this repository.** The JSONL export is the
    whole backlog — titles, notes, deadlines, all personal data — and this
    repository is **public**. The sync target is a separate, private data
    repository (`taskPilot-data`); `.gitignore` covers `export/` as a backstop
    for a stray local file, not as a place for it to live. See `docs/sync.md`.

## Binding and Auth

The control socket binds to `127.0.0.1` by default and **there is no
authentication** — deliberately. taskPilot is a single-user personal tool: the
threat model is "the port is on my own machine", and the socket has full write
access to the backlog. Binding it to `0.0.0.0` would hand anyone on the network
the ability to read, edit, and delete tasks. If remote access is ever wanted,
the answer is a tunnel (SSH), not a wider bind and not a hand-rolled token.

The database file itself is the second boundary: `data/*.db` is gitignored, and
should stay that way. It is personal backlog content, not source.

---

## Documentation

| File | Contents |
|---|---|
| `README.md` | What it is, quickstart, MCP setup, subcommand table. |
| `docs/architecture.md` | Layers, process model, the data flow of one tool call, threading, schema. |
| `docs/mcp.md` | The MCP contract: both protocol eras, version lists, error codes, tools. |
| `docs/scoring.md` | **The scoring specification.** If the code and this document disagree, the code is wrong. |
| `docs/sync.md` | **The cross-machine sync specification.** The JSONL record format, the four properties that make the file mergeable, the merge decision table and its two asymmetries, tombstones, and the two-machine workflow. |

When you change behaviour, update the document that describes it in the same
commit. `docs/scoring.md` is the one document that is a specification rather than
an explanation — treat a divergence there as a bug, not a doc task.

---

## Git

- Commit messages: conventional style (`feat:`, `fix:`, `refactor:`, `docs:`,
  `test:`, `chore:`).
- Keep commits atomic — one logical change per commit.
- Never commit the database (`data/*.db`) or anything under `build/`; the
  `.gitignore` covers both.
- Never commit credentials. taskPilot has no secrets of its own, and none should
  be introduced.

---

## When in Doubt

The headers are the design record: each one opens with a comment explaining why
the type exists and what it deliberately does not do. `docs/scoring.md` is the
source of truth for ranking; `docs/architecture.md` for structure. If a header
looks wrong, fix the code — do not edit the header to match a shortcut.
