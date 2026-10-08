# taskPilot

A work priority ranking backend with an MCP interface.

taskPilot keeps a flat list of tasks and answers one question well: **what should
be worked on next?** Every task carries four ranking inputs — importance (1..5),
an optional deadline, how long it has been waiting, and how many other things it
blocks — and the queue is a deterministic function of those plus the weights.

It is a long-lived daemon that owns a SQLite file, plus a stdio MCP bridge so an
LLM can add tasks, rank them, and close them out without a UI in the way.
There are no projects, no subtasks, no dependency graph, and no notifications.
`blocks` is a number you supply, not a graph the tool maintains.

Several machines can share one backlog: taskPilot exports the whole list as
JSONL and merges an export back in, so the file can live in a private git
repository and be reconciled line by line. See
**[docs/sync.md](docs/sync.md)**.

---

## Quickstart (60 seconds)

```bash
# 1. Dependencies (Ubuntu/Debian). No vcpkg, no vendored third-party.
sudo apt install nlohmann-json3-dev libasio-dev libsqlite3-dev libgtest-dev

# 2. Build
cd ~/1_Code/02_taskPilot
./run.sh build

# 3. Start the daemon (leave it running). It creates ./data/taskpilot.db on
#    first start, including the data/ directory SQLite will not make itself.
./run.sh serve
```

In a second terminal, attach the REPL and put some work in:

```bash
./run.sh cli
```

```
taskPilot> add "Fix the websocket reconnect bug" imp=5 due=today blocks=2
taskPilot> add "Write the monthly report" imp=4 due=+1d
taskPilot> add "Tidy the kline export scripts" imp=3 due=+5d
taskPilot> add "Reply to the vendor email" imp=2 due=+2d
taskPilot> queue
```

`queue` prints the ranked list, best first. Those four tasks score like this:
the numbers are the worked example from
[docs/scoring.md](docs/scoring.md), where the tasks were created a few days
apart — which is what the `age` column shows, and why a backlog seeded seconds
ago would score lower on that column:

| # | id | score | importance | urgency | age | blocks | title | due |
|---|----|-------|-----------|---------|-----|--------|-------|-----|
| 1 | 1 | 77.00 | 25.00 | 30.00 | 6.00 | 16.00 | Fix the websocket reconnect bug | today |
| 2 | 2 | 47.71 | 20.00 | 25.71 | 2.00 | 0.00 | Write the monthly report | +1d |
| 3 | 3 | 41.57 | 15.00 | 8.57 | 18.00 | 0.00 | Tidy the kline export scripts | +5d |
| 4 | 4 | 33.43 | 10.00 | 21.43 | 2.00 | 0.00 | Reply to the vendor email | +2d |

The first entry is first because it is due today, matters most, and two other
things are waiting on it. Note the third row: it is the lowest-importance task
in the list and its deadline is five days out, yet it beats the email due in two
days — because it has been sitting there for nine days and the aging term lifted
it. That flip is the point of the aging term; without it, the quiet long-running
work never surfaces. See **[docs/scoring.md](docs/scoring.md)** for the formula,
the rationale for each term, and the numbers worked through in full.

Close something out:

```
taskPilot> done 1
taskPilot> stats
```

---

## MCP setup for Claude Code

The daemon must be running (`./run.sh serve`). Then register the bridge:

```bash
claude mcp add taskpilot -- /home/joey/1_Code/02_taskPilot/run.sh mcp
```

Or put it in a project or user `.mcp.json`:

```json
{
  "mcpServers": {
    "taskpilot": {
      "command": "/home/joey/1_Code/02_taskPilot/run.sh",
      "args": ["mcp"]
    }
  }
}
```

`run.sh mcp` builds if anything changed, then execs the binary's `mcp`
subcommand. All of `run.sh`'s own output goes to stderr, so it can never
corrupt the protocol stream on stdout.

Then just ask:

- "What should I work on next?"
- "Add a task: review the migration plan, due Friday, and two people are blocked on it."
- "Task 7 is done."
- "The queue keeps burying the small stuff — make importance count for more."

The bridge publishes tools whether or not the daemon is up (the catalog is
compiled into the binary), so a call made while the daemon is stopped fails with
a clear connection error rather than an empty tool list. The MCP protocol
details — both supported eras, the version lists, the tools, the error codes —
are in **[docs/mcp.md](docs/mcp.md)**.

---

## Scoring summary

```
score = importance_w × importance          # 1..5, default weight 5.0  ->  5 .. 25
      + urgency_w    × urgencyFactor(...)   # 0..1, default weight 30.0 ->  0 .. 30
      + age_w        × ageDays(...)         # days, default weight 2.0  ->  0 .. unbounded
      + blocks_w     × blocks               # count, default weight 8.0 ->  0 .. unbounded
```

- **urgency** ramps linearly from 0 to 1 over `urgency_horizon_days` (default 7),
  saturates at 1 when overdue, and is **0.0 for a task with no deadline** — an
  undated task is not urgent, or it would outrank everything real.
- **age** is the anti-starvation term: without it, low-importance work is
  permanently outranked and never surfaces.
- **blocks** encodes "work that frees other work goes first".
- Ties break on older `created_at` first, then lower `id` — a **total** order, so
  the queue is reproducible across calls.

Weights are data, not code: change them at runtime with `set_weights` (or the
REPL's `weights urgency=45`) and they persist in the database. Validation is
strict — non-finite, negative, and a zero horizon are rejected rather than
clamped, because a weight change that is silently ignored is indistinguishable
from one that failed.

Full specification, term-by-term rationale, and the worked example:
**[docs/scoring.md](docs/scoring.md)**.

---

## Subcommands

| Command | What it does | Notes |
|---|---|---|
| `./run.sh` | `serve` — the daemon. | Default when no subcommand is given. Foreground. |
| `./run.sh serve` | Opens the store, applies the schema, binds the control socket, serves JSON-RPC. | One per control port. A second one fails to bind and says so. |
| `./run.sh mcp` | stdio MCP bridge. | Owns no store; forwards every call to the daemon. Spawned by the MCP host. |
| `./run.sh cli` | Interactive REPL attached to a running daemon. | Commands: `queue`, `ls`, `add`, `show`, `done`, `reopen`, `rm`, `weights`, `stats`, `status`, `methods`, `help`, `quit`. |
| `./run.sh export [--out <path>]` | Writes the whole backlog as one JSONL document. | Needs the daemon. Without `--out`, prints to stdout; with it, writes the file (creating parent directories) and reports the record count on stderr. |
| `./run.sh import [--file <path>] [--apply]` | Merges a JSONL export back into the backlog. | Needs the daemon. **Dry run by default** — without `--apply` it only reports what would change. Reads `--file`, or stdin when omitted. Exits 2 on a usage error. |
| `./run.sh version` | Prints the version. | Exits immediately. |
| `./run.sh build [--clean]` | Configure and compile. | `--clean` removes the build directory first. |
| `./run.sh test` | Build, then run the test suite. | `ctest --output-on-failure`. |
| `./run.sh clean` | Remove the build directory. | Does not touch `data/`. |

The subcommands pass the rest of the command line through to the binary. Three
flags configure the store and the socket; each has an environment fallback, and
the flag wins:

| Flag | Env | Default | Meaning |
|---|---|---|---|
| `--db <path>` | `TASKPILOT_DB` | `data/taskpilot.db` | The SQLite file to open (`serve` only). `run.sh` exports the default itself. |
| `--port <n>` | `TASKPILOT_PORT` | `8091` | Control socket port, 1..65535. |
| `--bind <addr>` | `TASKPILOT_BIND` | `127.0.0.1` | Bind address for `serve`; connect address for `mcp`, `cli`, `export` and `import`. |

`export` and `import` add their own flags — `--out <path>`, `--file <path>`,
`--apply` — which belong to those subcommands alone and have no environment
fallback.

So `./run.sh serve --port 9000 --db /tmp/backlog.db` is a second backlog on its
own port, and `./run.sh cli --port 9000` attaches to it.

The REPL's `help` command lists the exact grammar for each command, including
the deadline forms `due` accepts: `+3d`, `+6h`, `+30m`, `today`, `tomorrow`, or
an absolute `YYYY-MM-DD`.

`serve` and `mcp` are **the same executable**. That is deliberate: the tool list
the MCP host sees and the daemon answering the calls are compiled from one
`Services::methodSpecs()` catalog, so a rebuild can never leave them disagreeing.
`export` and `import` are subcommands of that same binary; `run.sh` forwards
them like the rest.

## Syncing across machines

One backlog can live on several machines. The database file stays local — it is
personal data and this repository is public — so what travels is a **JSONL
export** (one record per line) committed to a separate, **private** repository,
conventionally `taskPilot-data`. Git can then merge two machines' files line by
line; identity is a `uid` on every task, because the integer `id` means "the
ninth row *this* database created" and is not portable.

```bash
cd ~/1_Code/02_taskPilot
DATA=~/1_Code/taskPilot-data

git -C "$DATA" pull                                   # the other machine's records
./run.sh import --file "$DATA/backlog.jsonl"          # dry run: what would change
./run.sh import --file "$DATA/backlog.jsonl" --apply  # apply it
./run.sh export --out "$DATA/backlog.jsonl"           # this machine's merged state
git -C "$DATA" add backlog.jsonl && git -C "$DATA" commit -m sync && git -C "$DATA" push
```

`import` **defaults to a dry run** and only reports what a merge would change;
the merge happens with `--apply`. That default is deliberate — a merge can
overwrite or delete tasks, so the destructive form has to be asked for. Both
commands need the daemon running, because they go through the control socket
like `cli` and `mcp`: the daemon stays the single writer of the database.

Two rules worth knowing before the first sync:

- **The local `id` is not portable.** `done 7` and a queue's ids mean "row 7 on
  this machine"; the same task has a different id on the other one. `uid` is the
  identity, and it is what an export carries.
- **Pull before you push.** The export is a snapshot of your database, not a
  diff, so a file exported before the other machine's records were merged omits
  them. Order: pull, import, export, push.

Full specification — the record format field by field, the four properties that
make the file mergeable, the complete merge decision table with tombstones, and
the conflict case: **[docs/sync.md](docs/sync.md)**.

## Requirements

| Thing | Version | Note |
|---|---|---|
| CMake | >= 3.20 | what `CMakeLists.txt` requires |
| C++ compiler | C++20 | GCC 11+/Clang 14+ |
| Ninja | any | `run.sh` configures with `-G Ninja`; a manual `cmake` invocation can use any generator |
| nlohmann-json | 3.12 | `nlohmann-json3-dev` |
| standalone asio | 1.30 | `libasio-dev` |
| SQLite | 3.46 | `libsqlite3-dev` |
| GoogleTest | 1.17 | `libgtest-dev` |

## Where the data lives

One SQLite file, `data/taskpilot.db` by default (`TASKPILOT_DB` overrides it).
Three tables: `tasks` (each row carrying both a local `id` and the
cross-machine `uid`), `tombstones` (one row per deleted task, so a deletion can
travel to another machine — see [docs/sync.md](docs/sync.md)), and `settings`
(the ranking weights). Copying that file is a complete backup.
It is gitignored, and it should stay that way — it is personal backlog content,
not source.

## Architecture

Two layers. `core` (tasks, ranking math, SQLite, error type, clock) knows
nothing about sockets, JSON-RPC, or MCP. `control` (the method catalog and
handlers, the daemon's socket, the client, the MCP bridge, the REPL) depends on
it, never the reverse.

Layering, the process model, the exact path one tool call takes from an LLM to
SQLite and back, the threading model, and the database schema:
**[docs/architecture.md](docs/architecture.md)**.

## Scope limits (on purpose)

- **Single user, local only.** The control socket binds to `127.0.0.1` and has
  no authentication, because there is nothing to authenticate against and no
  network to reach it. Do not widen the bind; use a tunnel if remote access is
  ever needed.
- **No task dependencies.** `blocks` is a count you supply. A graph needs cycle
  detection, transitive resolution, and a UI to maintain it, and none of that
  serves the "what's next" question.
- **No recurring tasks, reminders, or notifications.** It ranks; it does not
  nag.
- **No undo**, except for the one thing that matters: `complete_task` keeps the
  record and `reopen_task` brings it back. `delete_task` is permanent on this
  machine — the one thing that can bring a task back is a *strictly newer* edit
  made on another machine after the delete (see [docs/sync.md](docs/sync.md)).

## License

GPL-3.0 — see [LICENSE](LICENSE).
