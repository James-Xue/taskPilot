# taskPilot cross-machine sync

The backlog is one SQLite file on one machine. This document is the
specification of the mechanism that lets two machines keep **one** backlog
between them, through a text export committed to git: the record format, the
properties that make the file mergeable, and the rule that decides what wins
when both machines changed the same task. The implementation is
`src/core/TaskSync.cpp` and `src/core/TaskStore.cpp`; if the two disagree with
this document, the implementation is wrong.

- Why a uuid exists at all: the header comment in `src/core/Uuid.hpp`.
- The format, the decision table, and the reasoning behind each rule: the header
  comment in `src/core/TaskSync.hpp`.
- The commands: `./run.sh export` and `./run.sh import`, plus the data
  repository's own `sync.sh` (section 6).

---

## 1. Why the integer id cannot be the identity

The `tasks` table is keyed by `id INTEGER PRIMARY KEY AUTOINCREMENT`. That is a
**machine-local** notion: it means *"the ninth row this database ever created"*.

That is fine while there is one database, and it becomes silent data loss the
moment there are two. Two machines that have each created nine tasks both have a
row 9 — the same number for two different pieces of work — and the same piece of
work has a different number on each machine. A merge keyed on `id` therefore
cannot tell "the same task" from "a different task", and every plausible
resolution loses work: match on the number and one machine's task overwrites the
other's; match on nothing and the backlog doubles.

So identity moved to `uid`: a version-4 uuid, assigned once by the store at
creation, never changed, and unique across machines because it is **random**
rather than sequenced. Random and not time + hostname on purpose — a clock that
steps backwards and two machines that share a hostname would each break the
uniqueness that is the entire point, and neither is detectable after the fact.

The integer `id` is not gone; it is demoted to what it always really was — a
**local row number**, useful for typing `done 7` at this machine's REPL and
meaningless on any other machine. An export never carries it.

| | `id` | `uid` |
|---|---|---|
| What it is | a local row number | the cross-machine identity |
| Assigned by | SQLite (`AUTOINCREMENT`) on insert | the store, once, at creation |
| Unique on one machine | yes | yes |
| Unique across machines | **no** — two machines both have a row 9 | yes |
| Value for the same task on two machines | differs | identical |
| Used for | `done 7`, `show 7`, this machine's control calls | the export, and every merge comparison |
| In the export | never | every record, live or tombstone |

The integer id is not even the last resort for ordering: `PriorityEngine::rank`
breaks a full tie on the smaller `uid`, never on `id`. `id` is local, so two
machines holding the same backlog would otherwise print different orders
(section 7.1).

### 1.1 Rows that predate `uid` get a derived one — the same one on both machines

A database written before the uid column existed is migrated when it is opened:
the column is added, every row without a uid is given one, and the unique index
is created afterwards (on final, distinct values). For those rows the uid is not
random — it is **derived** from the row itself (`deriveUuidFromText()` in
`src/core/Uuid.hpp`), from a canonical form of the row's fields: fixed field
order, fixed separators, empty values spelled out, and the local `id` and
`created_at` included, because a copied database preserves both while two
independently created tasks do not. The result is stamped into v4 shape, so
nothing else in the format needs a second code path. It is written once, and
from then on behaves exactly like a generated uid.

That determinism is the whole point: it is what makes **copying the database
file** a supported way to move or restore the backlog.

1. Two machines that entered the sync era holding the SAME rows — one database
   copied to the other, a restored backup, an rsync — derive the SAME uid for
   the same row.
2. The first sync therefore matches those tasks by uid and merges them.
3. With a random uid per row instead, each machine would assert its own uid for
   what is one task, the merge would see two unknown uids, and it would insert
   both — duplicating the whole backlog on the first sync after the copy.

Copying the database is supported for exactly that reason, and the residual
collision case is narrow: two rows collide only if every field in that canonical
form is identical — the same local row number, title, notes, status, ranking
fields, tags, `created_at` and `completed_at` on two independently created rows.
When that happens the two rows are indistinguishable to a merge anyway, so they
merge rather than corrupt. The migration is idempotent: a database that has
already been migrated (or one created fresh) is untouched.

---

## 2. The export format

The export is **JSONL**: one record per line, no header, one line per uid,
sorted by uid. A whole record is exactly one line, so a task that changes
changes one line and nothing else moves.

### 2.1 A live record

```
{"v":1,"uid":"9f3c1a2b-4d5e-4f60-8a9b-0c1d2e3f4a5b","title":"Write the monthly report","notes":"","status":"open","importance":4,"due_at":1760000000,"blocks":0,"tags":["work","monthly"],"created_at":1759900000,"updated_at":1759900000,"completed_at":null,"deleted":false}
```

Field by field:

| Key | Type | Meaning |
|---|---|---|
| `v` | int | Format version. Currently `1`, and present on every line, tombstones included (section 2.3). |
| `uid` | string | The v4 uuid: 36 characters, lowercase, hyphenated. The identity. |
| `title` | string | Required, non-empty. A live record with a blank title is rejected. |
| `notes` | string | Free-form detail; may be `""`. |
| `status` | string | `open`, `in_progress`, `done`, or `archived` — the same names the wire uses. |
| `importance` | int | 1..5. |
| `due_at` | int or `null` | Deadline, epoch seconds UTC. `null` means "no deadline". |
| `blocks` | int | >= 0. |
| `tags` | array of strings | May be empty. |
| `created_at` | int | Creation time, epoch seconds. |
| `updated_at` | int | Last modification, epoch seconds. **This is the stamp the merge compares.** |
| `completed_at` | int or `null` | Set when the status became `done`. |
| `deleted` | bool | `false` on a live record. |

Two details are deliberate. **`id` is absent**: a transported integer id would
invite a reader to match on it, which is precisely the bug this design exists to
avoid. And **every key is always present**, with `null` rather than an omitted
key for the two optional timestamps, so a reader never has to distinguish
"absent" from "null".

### 2.2 A tombstone

```
{"v":1,"uid":"9f3c1a2b-4d5e-4f60-8a9b-0c1d2e3f4a5b","updated_at":1760100000,"deleted":true}
```

That is the whole record:

| Key | Meaning |
|---|---|
| `v` | Format version, as above. |
| `uid` | The task that was deleted. |
| `updated_at` | When it was deleted, epoch seconds. The key name deliberately matches the live record's last-modification stamp — the store's column is `deleted_at`, but on the wire there is **one** stamp key, so the merge compares two records without having to know which kind it is looking at. |
| `deleted` | `true`. |

A tombstone carries no task fields at all, so a reader cannot accidentally read
stale content out of one.

### 2.3 Versioning

The format version travels on every line as `v`, so no line is "the version
line" (this matters for git — section 3.2). A reader that meets a version it
does not know **refuses the file**, naming the line number, rather than guessing:
a newer format may have changed what a field means, and a guess would corrupt
the merge quietly. The opposite tolerance applies to extra unknown keys, which
are ignored — a newer writer that adds a field must not break an older reader.

### 2.4 The stamp is epoch seconds, and its unit is checked

Every record carries exactly one stamp: `updated_at` for a live record, the
tombstone's `updated_at` for a deletion. It is the **only** thing the merge
compares, which makes its unit load-bearing: seconds, never milliseconds
(`Task.hpp`). The wire format cannot enforce a unit — `1760000000` and
`1760000000000` are both integers — so the parser checks the value against a
deliberately wide window of plausibility (`TaskSync.hpp`: `kMinPlausibleStamp`,
`kMaxFutureSkewSeconds`):

1. A stamp **below 1000000000** (2001-09-09 in epoch seconds) is refused.
2. A stamp **more than a year ahead of the reading machine's clock** is refused.

A stamp outside the window fails the whole parse, naming the line number. The
window is wide on purpose: the goal is to catch a wrong **unit**, not to police
a clock.

**What a millisecond stamp would do if it were accepted.** It is roughly 1000
times too large, so it wins every merge that ever touches that uid, and every
edit made on any machine afterwards is skipped as "older" — permanently. The
task freezes: still visible, still editable at the REPL, but no edit can ever
travel, because the stored stamp is always the newest thing in the file. That is
a failure with no error message anywhere, which is why the check is at parse
rather than left to the comparison.

**A stamp never moves backwards either.** A local write takes its stamp from the
wall clock in whole seconds, and the merge applies an incoming record only when
its stamp is strictly newer (a deletion may tie — asymmetry 1 in section 4). So
a second edit inside the same second keeps the stamp rather than lowering it,
and no write path replaces a recorded stamp with an older one. The two halves
fit together: the parse check stops a stamp jumping far ahead, the write rules
stop it slipping back, and the stamp keeps meaning "when this last changed". A
stamp that could move backwards would hand the next merge a false ordering —
the other machine's older record would look newer, and overwrite a real edit.

---

## 3. The four properties that make the file mergeable

The file is committed to git, and **git merges text line by line**. The four
properties below are what let it merge two machines' exports automatically
instead of conflicting on every sync. Each exists because its absence causes a
specific failure, and each failure ends with a person hand-resolving a merge —
which is exactly the moment work gets dropped.

**1. One record per line.** A whole task fits on one line, and lines are the
unit git compares.

> Without it — if the file were one JSON object holding an array of tasks — a
> new task on each machine edits the same structure, so both sides rewrite the
> same region and git reports a conflict on **every** sync, including syncs
> where the two machines touched unrelated work. Every "resolution" of such a
> conflict picks a side, and picking a side of a whole-array conflict drops the
> other machine's tasks wholesale.

**2. No header line.** There is no first line describing the file. The version
travels on every record instead.

> Without it, a header (`{"format":1,"exported_at":…,"count":…}`) sits at line 1
> of both machines' files and differs between them — the timestamp and the count
> differ by construction. Both sides have rewritten line 1, so git conflicts on
> every single sync, on the one part of the file that carries no task data.

**3. Exactly one line per uid.** A uid appears once, as the live task **or** as
its tombstone — never both, never twice. The file is therefore a complete
**snapshot** keyed by uid, and a merge is a per-uid comparison: for each record,
ask what the store already holds for that uid.

> Without it, "the task is in the file" and "the task was deleted" can both be
> true, so neither a reader nor a merge knows what the file means without
> replaying it line by line — and the answer then depends on where the lines
> happen to sit rather than on the data. A file that can say two things about
> one uid is not a snapshot; it is a log, and a merge over it is a replay.

**4. Sorted by uid.** The same uid sits in the same place in every export of
every machine, so a change to one task moves exactly one line and git resolves
per line rather than per hunk. The sort key is `uid` and not a date because a
date can tie (epoch seconds) and can differ between two clocks, while a uuid is
unique by construction and needs no tie-break — the order is a function of the
record **set**, not of the machine's history.

> Without it, a task's position would depend on something machine-local —
> insertion order, SQLite's row order, or a date — so two machines holding the
> same tasks would produce different files and a sync that changed nothing would
> produce a huge diff. Worse, adding one task on one machine would move the
> lines around it, so git sees a whole region as rewritten and reports
> conflicts on work that nobody edited; a hand resolution of that region can
> drop any line inside it.

Git also needs the file to stay the text it was written as: no editor or
`.gitattributes` rule should rewrite the line endings, because a CRLF conversion
in one clone makes every line differ from every other clone's and turns the
per-line merge back into a wholesale conflict.

---

## 4. The merge rule

A merge compares **one uid at a time**. For each record in the incoming file the
store asks what it already holds for that uid — never held, a live task, or a
tombstone — and the table below says what to do. The rule is a total function of
`(local state, incoming record)`; it is pure code with no database and no clock,
which is what makes the whole decision table testable, and it lives in
`src/core/TaskSync.hpp` / `TaskSync.cpp`. `TaskStore` only carries the decision
out.

The comparison uses the record's **stamp**: `updated_at` for a live record, the
tombstone's own `updated_at` for a tombstone. One comparison, whichever kind of
record arrived.

| local state | incoming record | action | why |
|---|---|---|---|
| never held | live | **insert** | Work from the other machine that this store has never seen. |
| never held | tombstone | **record the tombstone** | A deletion for a uid this store never held must be adopted, not skipped (section 5.1). |
| live, stamp S | live, stamp **>** S | **update** | The other machine's copy is newer; last write wins. |
| live, stamp S | live, stamp **==** S, same content | **skip** | The two records are equivalent; there is nothing to change. |
| live, stamp S | live, stamp **==** S, **different** content | **update the record with the smaller digest** | Two edits in the same second; the larger `contentDigest()` wins (asymmetry 3). The machine whose own copy has the larger digest skips; the other updates. |
| live, stamp S | live, stamp **<** S | **skip** | Ours is newer. |
| live, stamp S | tombstone, stamp **>=** S | **delete** | A delete beats a same-second edit (asymmetry 1). The row goes; the tombstone keeps the deletion's stamp. |
| live, stamp S | tombstone, stamp **<** S | **skip** | Our edit is newer than their deletion. |
| tombstoned, stamp S | live, stamp **>** S | **resurrect** | The other machine edited the task after we deleted it; the edit stands (asymmetry 2). The tombstone is removed. |
| tombstoned, stamp S | live, stamp **<=** S | **skip** | Our deletion is the latest write. |
| tombstoned, stamp S | tombstone, stamp **>** S | **revise the tombstone** | Both machines deleted the same task, in different seconds; the newer stamp is adopted so the two converge on one value (section 5.2). |
| tombstoned, stamp S | tombstone, stamp **<=** S | **skip** | Already gone, at a stamp at least as new. |

Note that a local uid is in exactly one of the three states, never two: the
store's own export writes one line per uid (section 3.3), and applying this
table maintains that invariant — a delete leaves a tombstone and no row, a
resurrect leaves a row and no tombstone.

### The three asymmetries, and why each is deliberate

Each one picks the outcome that is less surprising, and each is justified by
what the alternative does to a real sequence of machine actions rather than by
taste.

**1. A delete wins a tie.** `kDelete` fires on `>=`, while an update needs a
strict `>`. Resurrecting something the user deliberately deleted is far more
startling than losing an edit made in the same second — the second is
recoverable by hand, and the first looks like the tool ignoring an instruction.
Timestamps are epoch **seconds**, so "the same second" is not a rare edge case:
it is what two machines syncing around the same time produce.

**2. A post-delete edit resurrects.** Delete is not permanent; it is just the
latest write. Making delete absolute would need a second, separately
synchronised "this uid is banned forever" channel, and its failure mode — an
edit silently vanishing — is worse than the one it prevents. Note that the edit
must be **strictly** newer: an edit made in the same second as the delete does
not resurrect (asymmetry 1).

**3. An equal-stamp disagreement is broken by content, not by side.** "Equal
stamp means agreement" is false for two different edits made in the same second:
the records carry the same `updated_at` and different content. An implausible
race it is not — stamps are wall clock at **one-second resolution**, so any
pair of machines reaches this case routinely. And the cost of getting it wrong
is not a tie-break quibble: skipping on equality leaves each machine holding its
own copy forever, a permanent fork that the merge reports as "skipped" —
i.e. clean. Nothing ever raises an error, and the two machines simply stop
agreeing.

So when the stamps are equal and the content differs, the merge compares
`contentDigest()` of the two records and takes the **larger** digest.
`contentDigest()` is canonical: it depends on the record's content and not on
how it was serialized (fixed field order, fixed separators, an explicit marker
for an absent optional), so two machines holding byte-different JSON for the
same logical record still compute the same digest. Both machines compute the
same pair of digests from the same two records and therefore pick the SAME
winner — no third machine, no tie-break server, no extra round trip. The loser's
edit is discarded, which is unavoidable: with no ordering information left,
something must lose, and losing deterministically on both sides is the only
outcome that converges. (The digest is deliberately not a cryptographic hash and
makes no collision-resistance claim — a collision would only treat two records
that already disagree at the same second as agreeing, which is the behaviour
this rule replaces, not a new failure.)

### Two consequences worth knowing

- **A merge is idempotent.** Merging the same file twice is clean the second
  time: every live record now meets a local copy with an equal stamp and equal
  content (skip), and every tombstone meets its tombstone (skip). A sync that is
  run twice changes nothing the second time, which is what makes a re-run a safe
  response to doubt.
- **A merge is all-or-nothing.** The store applies the whole file inside one
  transaction, and a malformed line fails the entire call with the line number —
  no partial merge. "Some records could not be read" and "the file was genuinely
  short" become indistinguishable once the line numbers are gone, and a
  half-applied merge would be treated as truth by the next sync.

---

## 5. Tombstones

A deletion leaves a **tombstone**: a record that says "this uid was deleted, at
this time". It travels in the export like any other record.

**Why deletion needs a record.** Without one, a delete cannot travel at all.
Machine A deletes a task; A's export simply lacks the line. Machine B, which
still has the task, exports it; the merge on A sees a uid it does not know and
inserts it — and the thing you deliberately deleted comes back. So absence from
one side is never read as evidence of deletion, and the only thing that carries
a deletion is the tombstone.

**Why they never expire.** A deleted uid can reappear in any file that predates
the deletion: an old clone, a stale branch, a third machine that has not synced
in months. A tombstone that expired at some time T would let any such file win
after T, and the deletion you made would quietly undo itself. Tombstones are
tiny — one line, no task content — so there is no space argument for expiry,
and no garbage-collection window for a resurrection to slip through. A uid that
is gone is gone.

Two things a tombstone does **not** do:

- It does not make the deletion absolute. A strictly newer edit on another
  machine still resurrects the task (asymmetry 2 in section 4) — delete is the
  latest write, not a ban.
- It is not a backup. The task's fields are gone; only the fact and time of its
  deletion remain.

### 5.1 A machine adopts a tombstone for a uid it never held

The table's second row is the one with teeth: a tombstone naming a uid this
store has never seen — not as a task, not as a tombstone — is **recorded**, not
skipped. This entry is the most important rule in the format, because skipping
it destroys the record and the loss is invisible from the machine that causes
it:

1. A machine that skips the tombstone records nothing.
2. Its export writes only what the store holds, so that machine's export
   **omits the uid's line entirely**.
3. A file with no line for a uid is indistinguishable from a file where the task
   was never deleted — and git propagates the omission as a deletion of the
   tombstone.
4. Any machine still holding that task live (it had not synced since the
   delete) pulls that file, learns nothing about the uid, keeps the task, and
   republishes it live.

The deleted task comes back — on every machine — and the machine that caused it
reported "skipped: 1, clean". The whole point of tombstones, that absence is
never evidence of deletion, is defeated by a receiving machine declining to
store the evidence it was handed.

So the rule is absolute, and the cost is one line of bookkeeping on a machine
that never had the task. That looks wasteful the first time you see it, which is
exactly why it is written down here: the tombstone is the only evidence that the
deletion exists, and a machine that has merged the file becomes one more place
the evidence is held. There is no "there was nothing to protect" case — the
thing the tombstone protects is on the other machine.

### 5.2 Two tombstones for one uid converge on the newer stamp

The eleventh row covers the other way a tombstone is written: both machines
hold a tombstone for the uid and the incoming one is strictly newer, so its
stamp is adopted.

This is not a nicety. A tombstone's stamp is only ever written by deleting a
LIVE row, and a second delete on the same machine is `kNotFound` — so without
this branch, a tombstone's stamp could never be revised. Two machines that each
delete the same task locally, at their own second, would keep different stamps
forever: they never converge, their exports differ on that line permanently, and
git conflicts on it at every sync.

### The tombstone count

`get_status` reports a tombstone count. It grows with each local deletion, and
with each deletion another machine made for a task this one never held — the
adoption rule above, not a bug — and the one thing that can shrink it is a
resurrect (section 4). So a drop with no corresponding edit on another machine
means a merge has rewritten history, which is worth looking at.

---

## 6. The two-machine workflow

### 6.1 The data lives in a separate, private repository

The export is your actual backlog — titles, notes, deadlines. **This repository
is public**, so the export must never be committed here; the sync target is a
separate **private** repository, conventionally named `taskPilot-data`.

`export/` is gitignored in this repository as a backstop, so a stray
`./run.sh export --out export/tasks.jsonl` cannot be committed by accident —
but a backstop is not a destination. The file belongs in the data repository and
nowhere else.

Both machines must write the **same path** inside the data repository. The data
repository used here keeps the export at `export/tasks.jsonl` (the path its
`sync.sh` writes), and that is the path to use: git merges by path, so two
machines writing `export/tasks.jsonl` and `export/tasks-mine.jsonl` would never
merge those files, and the line-by-line property in section 3 would buy nothing.

One more thing to know about the scope: this moves tasks and tombstones. It does
not move ranking weights, and it is not a background service — a sync is
something you run, not something that happens by itself.

### 6.2 Setup, once per machine

1. Create the private repository (once, from whichever machine), for example
   `taskPilot-data`, with an `export/` directory, and push it.
2. Clone it on each machine — the examples below use `~/1_Code/taskPilot-data`.
3. Make sure each machine has its own clone of **this** repository, built, with
   its own `data/taskpilot.db`. The two databases stay independent; the export
   is the only thing that travels.

`./run.sh export` and `./run.sh import` talk to the daemon over the control
socket, exactly as `cli` and `mcp` do, so the daemon must be running. That is
deliberate: everything that touches the backlog goes through one writer, so a
second process never opens the database file behind the daemon's back.

### 6.3 A sync, step by step

The data repository used here carries a `sync.sh` that runs the whole sequence
in one command, with a preview mode:

```bash
cd ~/1_Code/taskPilot-data
./sync.sh              # pull -> merge -> export -> commit -> push
./sync.sh --dry-run    # show what the merge would change, write nothing
```

By hand, the same sequence — on **each** machine, in this order:

```bash
cd ~/1_Code/02_taskPilot
DATA=~/1_Code/taskPilot-data
EXPORT="$DATA/export/tasks.jsonl"

# 1. Take the other machine's latest records. Do this FIRST (section 7.2).
git -C "$DATA" pull

# 2. Dry run: see exactly what the merge would change, and change nothing.
./run.sh import --file "$EXPORT"

# 3. Apply it, once the report looks right.
./run.sh import --file "$EXPORT" --apply

# 4. Write this machine's merged state back out.
./run.sh export --out "$EXPORT"

# 5. Publish.
git -C "$DATA" add export/tasks.jsonl
git -C "$DATA" commit -m "sync: $(hostname) $(date -Iseconds)"
git -C "$DATA" push
```

The two machines never need to run this at the same moment. Each one's database
only ever moves forward, merging whatever the file contains at the time.

`export` with no `--out` prints the JSONL to stdout — it is a data pipe — and
with `--out` it writes the file (creating parent directories) and reports the
record count on stderr. `import` with no `--file` reads stdin.

### 6.4 The dry run is the default, on purpose

`./run.sh import` **without `--apply` performs a dry run**: it decides every
record and reports what it *would* do, writing nothing. A merge is the one
operation in this program that can destroy work — it can overwrite a task with a
newer copy and delete one on a tombstone — so the destructive form has to be
requested explicitly (`--apply`), never received by omitting a flag.

The report gives the counts of inserted, updated, deleted, resurrected and
skipped records, with example titles for the first four (capped at 50 examples;
the counts stay exact). Read it before applying, especially on the first sync of
a machine. A deletion adopted from another machine (section 5.1) counts like the
insertion of new state, and a revised tombstone stamp (section 5.2) like an
update; neither carries a title, so those two show up in the counts without an
example line.

### 6.5 If git reports a conflict

A conflict in the data file is possible, and it is not a data-loss event. Two
machines inserting lines near each other, or editing the same task, can leave git
unable to pick a line.

Resolve by **keeping both sides**: delete the conflict markers and leave both
lines in place. A file left with two lines for one uid is a transient state, not
a broken one — "exactly one line per uid" (section 3.3) is a property of an
*export*, and the next export makes the file canonical again. So run the normal
sequence — `import --apply`, then `export --out`, then commit. Where the two
lines were two versions of the same task, the merge rule (section 4) decides
which one wins, and it decides the *same* way on both machines once both have
seen both records — so the two exports converge on byte-identical files rather
than forking.

### 6.6 What the file looks like after a full sync

Once both machines have imported each other's records, both hold the same uids
in the same states with the same stamps, so each machine's export is the same
set of records, in the same order, one line each — the two files are
byte-identical. That equality is the point of the design, and it is what makes
the *next* commit from either machine a diff of just the tasks that changed.

Two conditions are worth stating plainly, because they are what a "sync" has to
actually achieve for the equality to hold:

- Each machine must have **merged the other's latest export**. A machine that
  has only pulled the file — or only run the dry run — has not changed anything.
- An equal-stamp disagreement converges only once **both** machines have seen
  both records. That is exactly what the content-digest tie-break (asymmetry 3)
  buys, and why a record that differs at an equal stamp must never be resolved
  as "keep ours" — doing so leaves each machine holding its own copy forever
  while every report says "skipped", i.e. clean.

---

## 7. The two rules to remember

### 7.1 The local id is not portable

`id` means "the Nth row this database created", so it is **not** the same task on
two machines. If you compare two machines by id, you will conclude that tasks
have changed that have not, and you may delete or edit the wrong one.

- `done 7`, `show 7`, and the ids in one machine's queue refer to that machine's
  rows only.
- The queue **order** is comparable across machines: the ranking inputs
  (`importance`, `due_at`, `created_at`, `blocks`) travel with the task, and the
  final tie-break is the smaller `uid` (`PriorityEngine::rank`) — a key both
  machines share — so two machines holding the same tasks rank them the same
  way. (The scores themselves are measured against each machine's own clock,
  which is why the tie-break, not the score, is what carries the comparison.)
- The identity that means the same thing everywhere is `uid`, which is the one
  field an export matches on.

### 7.2 Always pull before you push

The export is a **snapshot of your database**, not a diff. Your database only
knows what you have merged, so a file exported before the other machine's
records were pulled and merged **omits those records** — and pushing it
publishes that omission, in a file the other machine will pull and treat as the
truth.

Git's rejection of a non-fast-forward push is the backstop, not the protection.
The order in section 6.3 is the rule: **pull, import --apply, export, commit,
push** — and if a pull ever brings in records you have already exported, export
again before pushing.

---

## 8. Where this is implemented

| Concern | Where |
|---|---|
| The format, the decision table, version and stamp checks | `src/core/TaskSync.hpp` (the contract), `src/core/TaskSync.cpp` |
| Why a uuid, the shape check, the deterministic backfill | `src/core/Uuid.hpp`, `src/core/Uuid.cpp` |
| `uid` and `id` on the task itself | `src/core/Task.hpp` |
| The schema, the migration, tombstones, export and merge | `src/core/TaskStore.hpp` (the contract), `src/core/TaskStore.cpp` |
| `export_tasks` / `import_tasks` (control socket and MCP) | `src/control/Services.cpp` |
| The `export` / `import` subcommands and their flags | `src/main.cpp`; forwarded by `run.sh` |

Over MCP and the control socket the same two operations are `export_tasks` and
`import_tasks`; `import_tasks` takes the JSONL plus an optional `dry_run`, and
**defaults to a dry run exactly as the CLI does**.

The tests that pin this document: `tests/unit/test_task_sync.cpp` (the whole
decision table, one case per row, plus the framing invariant),
`tests/unit/test_task_store.cpp` (the version-0 migration, export sorting, and
the merge inside one transaction) and `tests/integration/test_two_machine_sync.cpp`
(two databases, colliding integer ids, and the acceptance test that a task
deleted on one machine stays deleted after a full round trip).
