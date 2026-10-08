# taskPilot architecture

How the pieces fit, why the boundaries are where they are, and what happens to a
single tool call from the moment an LLM asks for it to the moment the answer
comes back.

For the ranking model itself, see [scoring.md](scoring.md) — that document is a
specification, this one is a map. For the protocol contract, see
[mcp.md](mcp.md).

---

## 1. The two layers

taskPilot is two layers with a single direction of dependency.

```
L2 control  ┌───────────────────────────────────────────────────────────────┐
            │ Services     the API surface: method catalog + handlers       │
            │ JsonRpcServer  the daemon's control socket (TCP, JSON-RPC)    │
            │ ControlClient  blocking client for that socket                │
            │ McpServer      stdio MCP bridge (a client of the daemon)      │
            │ Repl           interactive console (a client of the daemon)   │
            └───────────────────────────┬───────────────────────────────────┘
                                        │  depends on
            ┌───────────────────────────v───────────────────────────────────┐
L1 core     │ Task        the record, plus its JSON/tags encoding           │
            │ Weights     the tunable coefficients                          │
            │ PriorityEngine  the ranking math: pure, static, no clock      │
            │ TaskStore   SQLite: the only code that speaks SQL             │
            │ Result      Result<T> / Status / Error / ErrorCode            │
            │ Clock       injectable time source (SystemClock, FixedClock)  │
            └───────────────────────────────────────────────────────────────┘
```

**The rule**: `control` includes `core/...`; `core` never includes `control/...`.
CMake links `taskpilot_control` against `taskpilot_core` and not the other way
round, but that only stops a link-time cycle — a stray `#include "control/..."`
inside `core` would still compile, so the direction is enforced by review, not by
the build.

The reason the boundary is worth defending is that both halves of it are
independently valuable:

- The ranking math can be tested to an exact float with no database, no socket,
  and a frozen clock (`FixedClock`).
- The storage layer can be tested with no scoring, no JSON-RPC, and no MCP.
- `PriorityEngine` takes `now` as a parameter instead of reading a clock, which
  is the difference between a deterministic test and a flaky one.

### What each layer must never do

| Layer | Must not |
|---|---|
| `TaskStore` | Rank, order for presentation, know what a "queue" is, or know that JSON-RPC exists. It returns rows in unspecified order. |
| `PriorityEngine` | Read the clock, touch storage, allocate per call for no reason, or depend on the order its input arrived in. |
| `Task` | Carry behaviour that belongs to the engine or the store. It is a plain struct with one trivial predicate (`isActionable`). |
| `Services` | Open a socket, format a protocol frame, or hold mutable state that would break concurrent `invoke()`. |
| Transports (`JsonRpcServer`, `McpServer`, `Repl`) | Contain domain rules. They frame, validate shape, and forward. |
| `McpServer` | Own a store. It is a bridge; the daemon is the single writer. |

---

## 2. Process model

Three processes, two of them short-lived.

```
        ┌──────────────────────────┐
        │ MCP host (Claude Code)   │
        └────────────┬─────────────┘
                     │ stdio (newline-delimited JSON-RPC)
        ┌────────────v─────────────┐          ┌──────────────────────────┐
        │ taskPilot mcp            │  TCP     │ taskPilot serve          │
        │ short-lived, stateless   ├─────────>│ long-lived daemon        │
        └──────────────────────────┘ loopback │ owns data/taskpilot.db   │
        ┌──────────────────────────┐          └──────────────────────────┘
        │ taskPilot cli (REPL)     │  TCP            ▲
        │ short-lived, stateless   ├─────────────────┘
        └──────────────────────────┘
```

| Process | Lifetime | Holds state | Who starts it |
|---|---|---|---|
| `serve` | long-lived, one per control port | the SQLite handle, the listening socket, the weights | the user (or a systemd unit) |
| `mcp` | one per MCP host session | nothing between calls | the MCP host, on demand |
| `cli` | as long as the terminal is open | nothing between commands | the user |

Design consequences worth stating outright:

1. **The daemon is the only writer.** `mcp` and `cli` hold no store; they hold a
   host and a port. If the bridge wrote to SQLite itself, the daemon's weights
   and the file could diverge with nothing to reconcile them.
2. **Clients reconnect per call.** `ControlClient::call` connects, writes one
   line, reads one line, and closes. There is no session to recover and no
   half-open state to detect; if the daemon restarts, the next call just works.
3. **A missing daemon is a clear error, not a broken install.** Because the tool
   catalog is static data compiled into the binary, `tools/list` answers even
   with the daemon down; only `tools/call` fails, with a message that names the
   connection failure.
4. **`mcp` and `serve` are the same executable.** The MCP bridge and the daemon
   share the catalog and the wire vocabulary as compiled-in data, so one binary
   is the simplest guarantee that the two halves cannot be from different
   versions.
5. **Only one daemon may hold the control port.** The bind is the lock: a second
   `serve` on the same address fails in `JsonRpcServer::start()`, which reports
   the address it could not take, and that reads as "already running" rather
   than as a raw errno. Nothing enforces one daemon per *database file* — a
   second daemon started on another port would open the same SQLite file and
   write it — so keep the default configuration, or give a second daemon its
   own `--db` and its own `--port` together.

---

## 3. One tool call, end to end

The example: the user asks "what should I work on next?", and the host calls
`get_queue`.

```
LLM ──"what's next?"──> MCP host
                          │  {"jsonrpc":"2.0","id":2,"method":"tools/call",
                          │   "params":{"name":"get_queue","arguments":{}}}
                          v  (one line on the mcp process's stdin)
                    McpServer::handleLine                [control/McpServer.cpp]
                          │  parse, resolve the tool against Services::methodSpecs(),
                          │  normalise arguments, then hand off
                          v
                    Backend (injected std::function)     [control/main.cpp]
                          v
                    ControlClient::call                  [control/ControlClient.cpp]
                          │  connect to 127.0.0.1:<port>, write one JSON-RPC line,
                          │  read one line, 5 s timeout on both directions
                          v
                    JsonRpcServer::dispatchLine          [control/JsonRpcServer.cpp]
                          │  parse the envelope, look up the registry entry,
                          │  frame result/error as JSON-RPC 2.0
                          v
                    Services::invoke                     [control/Services.cpp]
                          │  1. resolve the method in the dispatch table
                          │  2. reject non-object params
                          │  3. sample the clock ONCE and stamp it into this
                          │     call's params as `__taskpilot_now`
                          v
                    Services::handleGetQueue
                          │  1. read the limit / include_in_progress params
                          │  2. TaskStore::weights()
                          │  3. TaskStore::listTasks({})   <- unordered by design
                          │  4. drop in_progress if asked
                          │  5. PriorityEngine::rank(tasks, now, weights)
                          │  6. trim to the limit, serialize with toJson(RankedTask)
                          v
                    TaskStore (SQLite)                   [core/TaskStore.cpp]
                          │  one mutex for the whole statement
                          v
                    data/taskpilot.db
                          │
                          v  Result<nlohmann::json> travels back up
                    {"jsonrpc":"2.0","id":2,"result":{"content":[{"type":"text",
                     "text":"{...pretty-printed queue...}"}],"isError":false}}
                          v  (one line on stdout, flushed)
                    MCP host ──> LLM answers the user
```

Points along the path that are easy to get wrong:

1. **`now` is sampled once per call, in `Services::invoke`, not per task.** A
   slow query that read the clock per task could rank two tasks against two
   different instants, which is exactly the non-reproducible ordering the total
   order exists to prevent. The sampled instant travels in the params object
   (`__taskpilot_now`) rather than in a member or a static, because every handler
   shares one `Services` across connection threads — a member would be a data
   race or a lie about the class's thread-safety. A client-supplied value under
   that key is always overwritten, so the instant cannot be forged.
2. **The store returns rows unordered and the ranking happens above it.** No
   `ORDER BY score` exists and none should be added: the engine owns ordering,
   and a second ordering authority would silently disagree with the documented
   tie-breakers.
3. **A domain failure is not a protocol failure.** `Services` returns
   `Result<json>`; the JSON-RPC layer maps `ErrorCode` onto a wire code; the MCP
   layer turns a tool failure into `isError: true` content inside a *successful*
   JSON-RPC response, so the model receives a readable message it can act on
   instead of a transport fault it cannot interpret.

### Error mapping

Domain failures are `ErrorCode` values. Each maps to exactly one JSON-RPC code
(`rpcErrorCode` in `Services.cpp`), and `ControlClient::fromRpcErrorCode` maps
them back for clients that need the family rather than the number:

| `ErrorCode` | Wire code | Meaning |
|---|---|---|
| `kInvalidArgument` | `-32602` | Caller input failed validation (shared with the standard "invalid params"). |
| `kNotFound` | `-32001` | No such task id. |
| `kConflict` | `-32002` | Request conflicts with current state. |
| `kStorageFailure` | `-32003` | SQLite or filesystem failed. |
| `kInternal` | `-32603` | A bug; not the caller's fault. |

Protocol-level codes (`-32700` parse, `-32600` invalid request, `-32601` method
not found) come from the transport layers and never carry a domain meaning.
`ControlClient` reports a failure to connect as a storage failure with
"daemon not running" guidance, since on a single-user machine that is by far the
most likely cause.

---

## 4. Threading model

```
serve process
┌────────────────────────────────────────────────────────────────────────┐
│ accept thread            accepts non-blockingly, polls for shutdown    │
│   │                                                                    │
│   ├── session thread 1   reads lines, calls the registry, writes lines │
│   ├── session thread 2                                                 │
│   └── session thread N                                                 │
│                                                                        │
│ all session threads ──> Services::invoke  (stateless, no locking)      │
│                     ──> TaskStore          (one std::mutex)            │
│                     ──> PriorityEngine     (pure static functions)     │
└────────────────────────────────────────────────────────────────────────┘
```

- **One thread per connection.** `JsonRpcServer` accepts on its own thread and
  spawns a `std::thread` per session, tracking it in `m_sessions` under
  `m_sessionMutex`. A session ends when its socket closes; `stop()` closes the
  acceptor, drops the sessions, and joins every thread. It is idempotent.
- **A finished session thread is retained until `stop()` joins it.** The accept
  loop never reaps: it has no per-session completion state, so a spent thread is
  left in `m_sessions` for the daemon's lifetime. That is not free. A joined
  `std::thread` releases its handle and its stack; an unjoined one keeps both,
  so every connection that has ever been served still holds a thread stack —
  8 MB of address space with the default Linux `ulimit -s`. The pages are
  faulted in lazily, so resident memory grows far more slowly than that mapping
  suggests, but the retention is real and unbounded in the number of
  connections. It matters here because both clients connect per call: the MCP
  bridge and the REPL open a fresh connection for every tool call or console
  command, so a long-lived daemon accumulates one such thread per call. Restart
  `serve` (or `systemctl --user restart taskpilot`) rather than running one
  process for months.
- **`TaskStore` serializes everything.** SQLite can be built in any threading
  mode, so the store does not rely on the library's own locking: every public
  method takes one `mutable std::mutex` for the duration of its statement, and
  `PRAGMA busy_timeout = 5000` covers the case where another process (a hand-run
  `sqlite3` shell) holds the file. This trades read concurrency for the removal
  of an entire class of hard-to-reproduce corruption bugs — the right trade at
  the scale of a personal backlog.
- **`Services::invoke` needs no lock** because it holds no mutable state. The
  per-call instant is passed by value through the params object, not stored.
- **`PriorityEngine` needs no lock** because every method is a pure static
  function over its arguments.
- **`McpServer` is single-threaded.** It reads one line, handles it, writes one
  line, flushes, and repeats, until EOF, `notifications/exit`, or a closed
  stream. There is no concurrency in the bridge at all, which is also why it has
  no notification ordering rules to get wrong.

The system is deliberately boring here: no lock-free structures and no thread
pools. Shared mutable state exists in exactly two places — the store's mutex,
and the server's own session bookkeeping (a mutex and a running flag that guard
the accept loop and the thread list, not any task data).

---

## 5. Storage schema

One file, `data/taskpilot.db` by default (`TASKPILOT_DB` overrides it),
`:memory:` in the unit tests. Two tables.

```sql
CREATE TABLE IF NOT EXISTS tasks (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    title        TEXT    NOT NULL,
    notes        TEXT    NOT NULL DEFAULT '',
    status       TEXT    NOT NULL DEFAULT 'open',
    importance   INTEGER NOT NULL DEFAULT 3,
    due_at       INTEGER,             -- nullable: NULL means "no deadline"
    blocks       INTEGER NOT NULL DEFAULT 0,
    tags         TEXT    NOT NULL DEFAULT '[]',   -- a JSON array, not CSV
    created_at   INTEGER NOT NULL,    -- epoch seconds, UTC
    updated_at   INTEGER NOT NULL,
    completed_at INTEGER              -- nullable: set when status becomes done
);

CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status);
CREATE INDEX IF NOT EXISTS idx_tasks_due_at ON tasks(due_at);
```

| Decision | Why |
|---|---|
| Time as `INTEGER` epoch **seconds** | One unit everywhere: SQLite, JSON, and the wire. `Clock` is the only source of it. |
| `status` as `TEXT` | The column stores the same name the wire uses (`open`, `in_progress`, `done`, `archived`). An ordinal would break the moment someone reorders the enum. |
| `tags` as a JSON array in `TEXT` | A delimiter-joined string would need an escaping rule for tags that contain the delimiter. `parseTags` degrades unparseable input to an empty list rather than raising, so a corrupt row cannot take down a list query. |
| Nullable `due_at` / `completed_at` | "Absent" is a real state, and `due_at IS NULL` is a cheap query. JSON emits them as `null` rather than omitting the key, so clients see a stable schema. |
| Indexes on `status` and `due_at` | They are the only two plain columns the hot filters touch: `listTasks` filters on `status`, and the overdue and due-within-24h tallies filter on `due_at`. The tag filter is an `EXISTS` over `json_each(tags)`, which no column index can serve, and at this scale scanning for it is cheaper than an expression index. |
| DDL runs on every open, all `IF NOT EXISTS` | Opening a database written by an older build is a no-op, so there is no migration step to forget. |
| `settings.schema_version` | Written once with `INSERT OR IGNORE` and never overwritten; nothing reads it yet, and it exists so a future migration has a starting point. |
| `settings.weights` | Seeded from the struct defaults only when absent. Re-seeding on every start would silently reset the operator's tuning. |

Column defaults duplicate the `Task` struct defaults on purpose: they are the
safety net for a row inserted by something other than `TaskStore` (a hand-run
`sqlite3` shell), not the primary path.

`TaskStore::open` passes the path straight to SQLite, which does not create a
missing parent directory. Nothing has to create it by hand, though: `main.cpp`'s
`serve` path calls `ensureParentDirectory()` before opening the store, so the
first `./run.sh serve` creates `data/` itself. Only code that opens a
`TaskStore` without going through `serve` has to make the directory first.

The database file is gitignored. It is personal backlog content, not source, and
copying it is a complete backup.

---

## 6. Extending it

**A new operation.** Three edits in `control/Services.cpp`, and nothing else:
append a `MethodSpec` to `methodSpecs()`, write the handler, add one row to the
dispatch table in `invoke()`. The control socket, `describe_methods`, the REPL's
`methods` command, and MCP `tools/list` all pick it up from that one catalog.
The catalog is the single source of truth; hand-copying a tool list anywhere is
the drift bug this design exists to prevent.

**A new ranking term.** `Weights` gains a coefficient, `PriorityEngine::score`
gains a contribution, `docs/scoring.md` is updated in the same commit (it is the
specification, and code that disagrees with it is the bug), and a unit test
asserts the term's effect — including a case that fails if the term stops
contributing.

**A new column.** `TaskStore` owns the SQL, `Task` owns the field, `TaskPatch`
owns how it is updated, `toJson(Task)` owns its wire shape. Ranking code that
needs the field reads it from the `Task` struct, never from SQL.
