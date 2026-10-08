# taskPilot scoring model

The ranking formula is the only thing taskPilot does that you could not do
with a text file and discipline. This document is the specification; the
implementation is `src/core/PriorityEngine.cpp`. If the two disagree, the
implementation is wrong.

## The formula

For each actionable task (status `open` or `in_progress`):

```
score = importance_w × importance
      + urgency_w    × urgencyFactor(due_at, now, horizon_days)
      + age_w        × ageDays(created_at, now)
      + blocks_w     × blocks
```

| Term | Input | Range | Default weight | Contribution range |
|---|---|---|---|---|
| `importance` | 1..5, set by hand | 1..5 | `5.0` | 5 .. 25 |
| `urgency` | deadline proximity | 0..1 | `30.0` | 0 .. 30 |
| `age` | days since creation | 0..∞ | `2.0` | 0 .. unbounded |
| `blocks` | downstream dependents | 0..∞ | `8.0` | 0 .. unbounded |

Default `urgency_horizon_days` is `7`.

## Term by term

### importance — what it is worth

A plain 1..5 judgement. Multiplied by `5.0`, it spans 5 to 25 points, so the
widest possible importance gap is worth 20 points. That is deliberately
*smaller* than the urgency term's full range: knowing that something matters
a lot does not tell you to do it *now*, and the queue should reflect that.

### urgency — when it is due

`urgencyFactor` maps a deadline onto 0..1:

1. **No deadline → `0.0`.** Not urgent. The tempting alternative — treating an
   undated task as maximally urgent — makes every vague someday-item outrank
   real deadlines, which destroys the queue's usefulness.
2. **Overdue or due now → `1.0`.** Urgency *saturates*. A task 30 days late is
   not three times as urgent as one 10 days late, and an unbounded term would
   grow until it drowned out the other three.
3. **Within the horizon → `1 − days_remaining / horizon_days`.** A linear ramp,
   so urgency grows steadily and visibly as a deadline approaches.
4. **Beyond the horizon → `0.0`.** A deadline three weeks out is not actionable
   this morning and must not crowd out today's work.

At the default weight this spans 0..30 points — the largest single term, by
design. A deadline today outranks almost anything.

### age — how long it has waited

Fractional days since creation, clamped at `0.0`. The clamp matters: without
it, a task with a `created_at` in the future (clock skew, a hand-edited row)
would earn *negative* age and sink in the queue. An aging term that can
subtract is worse than no aging term at all.

This is the anti-starvation term. Without it, a low-importance, no-deadline
task is permanently outranked and never surfaces — which is exactly the
failure mode that makes people abandon priority systems. At weight `2.0`, a
week of neglect adds 14 points: enough to lift a stale chore above fresh
trivia, not enough to beat a real deadline.

### blocks — what it unblocks

A count of downstream tasks or people waiting on this one. At weight `8.0`, one
blocked dependent is worth 8 points — more than a single step of importance
(`5.0` per level). Two dependents (16 points) rival the widest possible
importance gap (20 points) without exceeding it, and three (24 points) exceed
it outright. So freeing other work does outrank a better-judged task, but only
once a few dependents are waiting. This encodes the intended semantics: **work
that frees other work goes first.**

`blocks` is a plain number the caller supplies, not a dependency graph.
taskPilot stores no edges between tasks. That is a deliberate scope limit —
a graph needs cycle detection, transitive resolution, and a UI to maintain it,
and all of that serves the "simple version" poorly.

## Tie-breaking

Scores are floating point, so ties are possible. The order is a **total**
order, which means the queue is reproducible and does not depend on input
order or on which sort algorithm ran:

1. Higher score first.
2. On a tie, **older `created_at` first** — a task that has waited longer
   wins. Same fairness instinct as the aging term.
3. On a full tie, **lower `id` first**.

## Worked example

Defaults: `importance=5.0, urgency=30.0, age=2.0, blocks=8.0, horizon=7`.

`now = T` for the duration of one call — `now` is sampled once per request,
not per task, so a slow query cannot rank two tasks against different
instants.

| # | Task | imp | due | age | blocks |
|---|---|---|---|---|---|
| A | Fix the WS reconnect bug | 5 | today | 3d | 2 |
| B | Write the monthly report | 4 | +1d | 1d | 0 |
| C | Tidy the kline data | 3 | +5d | 9d | 0 |
| D | Answer email | 2 | +2d | 1d | 0 |

| Task | importance | urgency | age | blocks | **total** |
|---|---|---|---|---|---|
| A | 25.00 | 30.00 | 6.00 | 16.00 | **77.00** |
| B | 20.00 | 25.71 | 2.00 | 0.00 | **47.71** |
| C | 15.00 | 8.57 | 18.00 | 0.00 | **41.57** |
| D | 10.00 | 21.43 | 2.00 | 0.00 | **33.43** |

Queue order: **A, B, C, D**.

Note what the aging term did. Without it, C would score 23.57 and land
*below* D's 31.43 — a task that has been quietly rotting for nine days would
sit behind an email that arrived this morning. With it, C is lifted two
places. That flip is the entire point of the term, and
`tests/unit/test_priority_engine.cpp` asserts it directly so a future
"simplification" that drops aging fails loudly.

## Retuning

Weights are data, not code. Change them at runtime:

```
set_weights  {"urgency": 0.0}          # ignore deadlines entirely
set_weights  {"age": 10.0}             # aggressive anti-starvation
set_weights  {"blocks": 0.0, "importance": 20.0}   # pure importance, no graph
```

They persist in the store's `settings` table and survive restarts. Validation
is strict: every weight must be finite, `>= 0`, and at most `1e9`, and
`urgency_horizon_days` must be `> 0`. An invalid set is rejected with an
error rather than silently clamped — a weight change that is silently
ignored is indistinguishable from one that failed, and you would tune the
queue for an hour wondering why nothing moved.

The `1e9` ceiling exists because a weight never reaches the wire on its own:
what the payload carries is the weight's **product** with a task field
(`importance_w × importance`, `blocks_w × blocks`, and so on). A weight of
`1e308` is a legal finite double, so a finiteness check alone lets it through,
but `1e308 × 5` is `+inf` — and JSON has no infinity, so the serializer writes
the score as `null` and every client that parses `score` or `top_score` as a
number breaks. `1e9` is orders of magnitude away from that failure, and more
than seven orders of magnitude above the largest default weight (`30`), so
anything over the bound is a typo rather than a tuning.
`urgency_horizon_days` has no upper bound because it is not multiplied into
the score at all: it divides into the urgency ramp, so a huge horizon produces
a nearly flat ramp (tiny urgency), never a non-finite score.

Setting a term's weight to `0.0` is legal and is the supported way to switch a
term off. Note that setting **all four** to zero is also legal and produces a
queue ordered entirely by the tie-breakers — oldest first. That is a
defensible "pure FIFO" mode, not an error.
