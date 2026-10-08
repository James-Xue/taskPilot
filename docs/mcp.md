# taskPilot MCP interface

taskPilot speaks the Model Context Protocol over stdio: newline-delimited
JSON-RPC 2.0 on stdin/stdout. The `mcp` subcommand is a **bridge and nothing
else** — it owns no store and no ranking logic. Every `tools/call` is forwarded
to the running daemon through `ControlClient`, so the daemon stays the single
writer and a restarted bridge cannot diverge from it.

This document is the contract: both protocol eras, the version lists, the error
codes, and the tools. Every wire shape below is the one the reference
implementation uses (`@modelcontextprotocol/server` 2.0.0), with
`DiscoverResultSchema` and the `UnsupportedProtocolVersionError` payload as the
authority for the two shapes that matter here.

- Implementation: `src/control/McpServer.cpp` (protocol) and
  `src/control/ControlClient.cpp` (the hop to the daemon).
- Design rationale for the dual-era choice: the header comment in
  `src/control/McpServer.hpp`.
- Architecture, including where this bridge sits: [architecture.md](architecture.md).

---

## 1. The one rule that breaks everything: stdout

**stdout carries the protocol stream and nothing else.** One stray byte — a log
line, a warning, a debug print — corrupts the framing and the host drops the
server. Therefore:

- Diagnostics go to `stderr`, never `std::cout`.
- `McpServer::handleLine()` returns a response with no trailing newline;
  `run()` appends exactly one and flushes it. A buffered reply is
  indistinguishable from a hung server, because the host blocks on each reply.
- Notifications produce no line at all.
- `run.sh` sends all of its own build output to stderr, so a rebuild triggered
  by `./run.sh mcp` cannot pollute the stream.

Framing tolerances, for completeness: a trailing `\r` is stripped (CRLF hosts),
and blank or whitespace-only lines are skipped silently rather than answered
with a parse error the host could not correlate with anything.

The bridge's only stderr output is the `server/discover` reply trace (see
section 5): it records the version list the server advertised, so a live
client's probe can be observed without a packet capture, and it is on stderr
precisely so it cannot corrupt the protocol stream.

---

## 2. Two eras, one process

MCP revision **2026-07-28** is the largest breaking change in the protocol's
history. It removed the `initialize` handshake, removed protocol-level sessions,
and made the core stateless: each request declares its protocol version, client
identity, and capabilities in `_meta` instead of negotiating them once. It
removed `ping` and requires servers to implement `server/discover`.

The spec names the two eras:

| Aspect | **legacy** | **modern** |
|---|---|---|
| Versions | 2025-11-25 and earlier | 2026-07-28 and later |
| Session | established by a handshake | none; every request stands alone |
| Version declared | once, in `initialize` params | per request, in `params._meta` |
| Discovery | `initialize` | `server/discover` |
| `ping` | supported | removed |
| Session state | held server-side | must not be |

**taskPilot is dual-era**: it implements both, and nothing is gated on session
state. Three properties make that work:

1. `server/discover` replies with the full version list, so a modern client can
   proceed without a handshake.
2. `initialize` still negotiates the legacy set, so a legacy client behaves
   exactly as it always did.
3. `tools/list` and `tools/call` answer identically whether or not a handshake
   happened. That is what lets (1) and (2) coexist in one dispatch path — and it
   is also simply true to the modern stateless model, where there is no session
   to key on.

### Why both, rather than picking one

A server that implements only the legacy handshake works exactly as long as the
client is willing to negotiate down to a version it speaks; a sibling project
takes that approach and pins a single version. A server that implements only the
modern era breaks every client that has not migrated. Supporting both costs
roughly fifty lines — a version list, a discovery reply, and one gate — and
removes the question. There is no state to keep, so the dual-era server is not
more complex in the way that matters; it is one dispatch path with one optional
check in front of it.

---

## 3. Version tables

**Legacy versions** (`legacyProtocolVersions()`, ascending — newest last):

- `2024-10-07` — the initial revision.
- `2024-11-05`
- `2025-03-26`
- `2025-06-18`
- `2025-11-25` — the newest legacy version, and the cap for a legacy
  `initialize` reply.

**Modern versions** (`modernProtocolVersions()`):

- `2026-07-28` — the stateless era.

**Everything this server speaks** (`supportedProtocolVersions()`), newest first
throughout — this is the list advertised by `server/discover` and returned with
an `-32022` error:

| # | Version | Era |
|---|---|---|
| 1 | `2026-07-28` | modern |
| 2 | `2025-11-25` | legacy |
| 3 | `2025-06-18` | legacy |
| 4 | `2025-03-26` | legacy |
| 5 | `2024-11-05` | legacy |
| 6 | `2024-10-07` | legacy |

The ordering is deliberate: a client that simply takes the first entry it
recognises lands on the newest version both sides speak, without having to
understand the era split.

### Legacy negotiation (`initialize`)

`negotiateLegacyVersion()` has exactly two outcomes, and only one of them is a
counter-offer:

1. The client requested a version in the **legacy** list -> it is echoed
   verbatim, as the spec requires.
2. Anything else (an unknown string, a modern version, nothing at all) -> the
   server counter-offers `2025-11-25`, the newest legacy version. This is not an
   error; the client decides whether it can speak it.

What is deliberately absent: no modern version is ever returned from
`initialize`, even though this server speaks one. A legacy client cannot parse a
modern session, so answering a handshake with `2026-07-28` is the one failure
mode that would make dual-era support worse than picking a single era.

---

## 4. A legacy conversation

```
client -> {"jsonrpc":"2.0","id":1,"method":"initialize","params":{
             "protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{...}}}
server <- {"jsonrpc":"2.0","id":1,"result":{
             "protocolVersion":"2025-06-18",
             "capabilities":{"tools":{"listChanged":false}},
             "serverInfo":{"name":"taskpilot","version":"0.1.0"}}}

client -> {"jsonrpc":"2.0","method":"notifications/initialized"}     (no reply)
client -> {"jsonrpc":"2.0","id":2,"method":"tools/list"}             (or ping)
client -> {"jsonrpc":"2.0","id":3,"method":"tools/call","params":{
             "name":"add_task","arguments":{"title":"write the report","due_at":1760000000}}}
client -> {"jsonrpc":"2.0","method":"notifications/exit"}            (no reply; loop stops)
```

`ping` is answered here with an empty result, because a legacy client uses it as
a liveness probe and refusing would look like a dead server to a client that is
otherwise negotiating correctly. `notifications/initialized` is answered with
silence — there is no session to record.

## 5. A modern conversation

```
client -> {"jsonrpc":"2.0","id":1,"method":"server/discover"}
server <- {"jsonrpc":"2.0","id":1,"result":{
             "supportedVersions":["2026-07-28","2025-11-25","2025-06-18",
                                  "2025-03-26","2024-11-05","2024-10-07"],
             "capabilities":{"tools":{"listChanged":false}},
             "serverInfo":{"name":"taskpilot","version":"0.1.0"},
             "ttlMs":0,
             "cacheScope":"private"}}
             (and one line on stderr recording the reply — see section 1)

client -> {"jsonrpc":"2.0","id":2,"method":"tools/call","params":{
             "_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"},
             "name":"get_queue","arguments":{"limit":5}}}
```

The discover result, field by field. Every key is spelled exactly as the
reference `DiscoverResultSchema` spells it:

| Key | Required? | Value |
|---|---|---|
| `supportedVersions` | yes | every version this server speaks, newest first (the six-row table above) |
| `capabilities` | yes | `{"tools":{"listChanged":false}}` |
| `instructions` | no | omitted: taskPilot has no prose to inject into the model's context |
| `ttlMs` | cacheability pair | `0` — the reply is specific to this build, so it must not be reused |
| `cacheScope` | cacheability pair | `"private"` — the answer belongs to this client's connection, not to a shared cache |
| `serverInfo` | taskPilot's own addition | `{"name":"taskpilot","version":"0.1.0"}` |

`ttlMs` and `cacheScope` — the SEP-2549 cacheability pair — are stated
explicitly rather than left to the client's defaults: the discover reply
describes this binary, so defaulting it to something cacheable would be wrong.

A modern request may also skip discovery entirely and carry `_meta` on its first
call. Nothing is ordered and nothing is remembered between requests.

The version key is one flat string that happens to contain a slash —
`io.modelcontextprotocol/protocolVersion` — not a nested path, so it is looked up
verbatim inside `params._meta`.

### 5.1 Why the key names matter, and what a wrong one costs

This is the part that fails with no diagnostic anywhere. A client that probes
with `server/discover` and cannot parse the reply does not report a bad result:
it concludes the server is not modern, falls back to `initialize`, and says
nothing. The `-32022` payload has the same property — a client that cannot read
`supported` gives up on negotiation instead of retrying with a version both
sides speak. So a server whose discovery reply is one key name wrong still
*works*, as a legacy server, with nothing in any log explaining why the modern
path never engaged. That is why the two shapes are pinned here and asserted by
`tests/unit/test_mcp_server.cpp`:

- The discover result key is `supportedVersions`. It is **not**
  `protocolVersions`, and no second spelling is emitted beside it: a client
  looks up the one name it knows, so an alias is a second contract with no
  reader — and one a future edit could get wrong on its own.
- The `-32022` data is exactly `{"supported": [...], "requested": "..."}`.

### The version gate

If a request **declares** a version in `_meta` and that version is not in the
supported list, it is refused with `-32022`, the supported list under `supported`,
and the offending version under `requested`:

```json
{"jsonrpc":"2.0","id":2,"error":{
   "code":-32022,
   "message":"Unsupported protocol version: 2027-01-01",
   "data":{"supported":["2026-07-28","2025-11-25","2025-06-18",
                        "2025-03-26","2024-11-05","2024-10-07"],
           "requested":"2027-01-01"}}}
```

Notes on the gate:

- It is not a dead end. Per the spec, `-32022` **is** the negotiation step: the
  client reads `supported`, picks a version both sides speak, and retries with
  `requested` explaining what caused the mismatch.
- The data object carries exactly those two keys. A version list under a second
  name is not a harmless alias: it is a second contract whose reader does not
  exist, and a client that picks the wrong one abandons negotiation silently.
- A request that declares **nothing** sails through. That is how a legacy client
  — which states its version in `initialize` instead — reaches the same dispatch
  path, and it is why the gate can never stand between a legacy client and its
  handshake.
- A **notification** that declares an unsupported version is dropped in silence
  rather than provoking an illegal reply. Every response path shares one funnel
  that refuses to answer a message with no id.
- `-32022` is the only error that carries `data`. Ordinary protocol faults keep
  the minimal two-field shape.

---

## 6. Tools

The tool list is **derived from `Services::methodSpecs()`** — the same catalog
that builds the control socket's dispatch registry and `describe_methods`. It is
not written out by hand anywhere in the MCP layer, so a method added to the
catalog appears on both transports at once and cannot be advertised on one and
missing on the other.

Because the catalog is static data compiled into this binary, `tools/list`
answers even when the daemon is stopped. The user sees the available tools and a
clear connection error on each call, instead of an empty tool list that looks
like a broken installation.

Names are the control-plane method names **verbatim** — one vocabulary across
MCP, the control socket, and the REPL.

| Tool | Parameters | What it does |
|---|---|---|
| `get_status` | — | Daemon version, uptime, database path, method count, and the backlog tallies (`open`, `in_progress`, `done`, `archived`, `overdue`, `due_within_24h`). |
| `describe_methods` | — | The full catalog: every method with its description and parameter JSON Schema. |
| `add_task` | `title` (required), `importance` 1..5, `due_at`, `blocks`, `tags`, `notes` | Create a task; returns it with its assigned id. |
| `update_task` | `id` (required), `title`, `notes`, `importance`, `due_at`, `clear_due_at`, `blocks`, `tags`, `status` | Change any subset of fields; omitted fields keep their values. |
| `complete_task` | `id` | Mark done and stamp `completed_at`. |
| `reopen_task` | `id` | Put a done or archived task back in the queue as open. |
| `delete_task` | `id` | Permanently delete; returns `{"deleted": true, "id": N}`. |
| `get_queue` | `limit`, `include_in_progress` (default `true`) | The ranked queue with each entry's `score` and `score_parts`. |
| `list_tasks` | `status`, `tag`, `limit`, `order` (`score` \| `created` \| `due`) | Browse and filter; returns an array of tasks. |
| `get_task` | `id` | One task with its current score and the breakdown behind it. |
| `get_stats` | — | Counts by state, overdue and due-within-24h tallies, plus `top_title` and `top_score`. |
| `get_weights` | — | The current ranking weights and urgency horizon. |
| `set_weights` | `importance`, `urgency`, `age`, `blocks`, `urgency_horizon_days` | Partial retune; validated before it is stored. |

Parameter names, ranges, and descriptions are defined once, in
`Services::methodSpecs()` — read them there, or ask the server with
`describe_methods` / `tools/list`, rather than trusting a copy.

### Response shapes worth knowing

- `get_queue` returns an object: `{"now", "weights", "queue": [...]}`.
- `list_tasks` returns a bare JSON array of tasks.
- Every ranked entry is a strict superset of a task: the task's own fields plus
  `score`, `score_parts` (the four weighted contributions),
  `urgency_factor`, and `age_days`. Display `score`; do not re-sum
  `score_parts`, which are float-rounded.
- Nullable fields (`due_at`, `completed_at`) are emitted as `null` rather than
  omitted, so the schema is stable.

---

## 7. Failure semantics

The distinction that matters for a model: **a tool that fails is a successful
call.**

| What happened | Wire result |
|---|---|
| Tool ran, returned a value | `{"result":{"content":[{"type":"text","text":"<pretty JSON>"}],"isError":false}}` |
| Tool ran, domain failure (`no task with id 7`) | `{"result":{"content":[{"type":"text","text":"error (code=1): no task with id 7"}],"isError":true}}` |
| Malformed JSON | `-32700` "Parse error", `id: null` |
| Not a JSON-RPC object, or no string `method` | `-32600` "Invalid request", `id: null` |
| Valid request, unknown method | `-32601` "Method not found" |
| `tools/call` with no string `name` | `-32602` "Invalid params: name required" |
| `tools/call` naming a tool not in the catalog | `-32602` "Unknown tool: <name>" |
| Request declares an unsupported protocol version | `-32022` with `{supported, requested}` |

Why the tool-failure case is not a JSON-RPC error: the call itself succeeded —
it reached the tool and the tool said no. Reporting it as transport failure
would present an interpretable domain message ("no task with id 7") as a
protocol fault the model cannot act on. The failure text is therefore
`isError: true` content inside a normal response.

The `code=` in that text is the **`ErrorCode` enum ordinal**, not a JSON-RPC
code — it is the domain category the daemon attached:

| Ordinal | `ErrorCode` | Meaning |
|---|---|---|
| 0 | `kInvalidArgument` | Caller input failed validation. |
| 1 | `kNotFound` | Referenced entity does not exist. |
| 2 | `kConflict` | Request conflicts with current state. |
| 3 | `kStorageFailure` | SQLite or filesystem failed. |
| 4 | `kInternal` | A bug; not the caller's fault. |

Over the raw control socket the same failures map onto JSON-RPC codes
(`-32602`, `-32001`, `-32002`, `-32003`, `-32603` respectively) — see
[architecture.md](architecture.md) section 3.

### Notifications

A message with no `id` (or a null one) is a notification: its side effect
happens, nothing is written back. Two are handled by name:

- `notifications/initialized` — the legacy handshake confirmation. Silence, by
  design: this server keeps no session state to record.
- `notifications/exit` — the host is shutting down. The run loop stops before
  reading another line.

Every other notification is dropped in silence rather than answered, which is
the JSON-RPC rule and the reason the reply funnel computes "is this a
notification?" once, before any method is dispatched.

---

## 8. Registering the server

```bash
./run.sh serve                                        # the daemon must be running
claude mcp add taskpilot -- /home/joey/1_Code/02_taskPilot/run.sh mcp
```

Or in a `.mcp.json`:

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

### Debugging the bridge

Run it by hand and watch stderr, which is where every diagnostic lives:

```bash
./run.sh mcp 2>/tmp/taskpilot-mcp.log
```

Then type requests at it one line at a time — a `tools/list` costs nothing and
proves the framing:

```json
{"jsonrpc":"2.0","id":1,"method":"tools/list"}
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_status","arguments":{}}}
```

If stdout shows anything that is not a single-line JSON object per request, that
is a framing bug and the host will drop the connection. Never merge stderr into
stdout while diagnosing.
