// PriorityEngine.cpp — the ranking math.
//
// This is the one thing taskPilot does that a text file plus discipline could
// not reproduce, so it is also the part with the least room for a silent
// mistake. Two properties are load-bearing and are restated at their
// definitions below:
//
//   1. Every function here is PURE — (task, now, weights) in, numbers out.
//      No clock, no storage, no I/O, no global state. Determinism is
//      therefore free, and every number in docs/scoring.md is exactly
//      testable (see tests/unit/test_priority_engine.cpp).
//
//   2. The queue order is TOTAL — score, then created_at, then id. A
//      comparator that looked only at the score would leave the order of
//      equal-scoring tasks up to the sort implementation and the caller's
//      input order, which makes "why did this task move?" unanswerable.
//
// docs/scoring.md is the specification for every rule implemented here. If
// this file and that document disagree, this file is wrong.

#include "core/PriorityEngine.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace taskpilot
{

namespace
{

/// One day in seconds, as a double.
///
/// kSecondsPerDay (Clock.hpp) is the canonical int64 value, but every use
/// here divides by days, so carrying a double mirror keeps the arithmetic in
/// a single type and avoids a narrowing conversion at each call site.
constexpr double kSecondsPerDayAsDouble{ 86400.0 };

/// The queue's total order, written once and shared by rank() and
/// sortByScore() so the two can never drift apart.
///
/// The three keys, in priority order:
/// 1. Higher score first.
/// 2. On a tie, older created_at first — the same fairness instinct as the
///    aging term: the task that has waited longer wins.
/// 3. On a full tie, lower id first, so the result never depends on the input
///    order or on which sort algorithm ran.
///
/// Returns true when `lhs` must come before `rhs`.
///
/// Precondition: every total is finite — Weights::validate() rejects
/// non-finite weights and every remaining input is an integer — which is what
/// makes this comparator a strict weak ordering (and therefore safe to hand
/// to std::sort).
[[nodiscard]] bool precedesByKeys(double lhs_total, std::int64_t lhs_created_at,
                                  std::int64_t lhs_id, double rhs_total,
                                  std::int64_t rhs_created_at, std::int64_t rhs_id)
{
    // 1. Higher score first. Spelled as two ordered comparisons rather than
    //    `!=` followed by `>` so that the pair (a > b), (a < b) can never
    //    both be false for two genuinely different totals in the way a lone
    //    `!=` test would leave ambiguous.
    if (lhs_total > rhs_total)
    {
        return true;
    }
    if (lhs_total < rhs_total)
    {
        return false;
    }

    // 2. Older created_at first (smaller epoch seconds = created earlier).
    if (lhs_created_at != rhs_created_at)
    {
        return lhs_created_at < rhs_created_at;
    }

    // 3. Lower id first — the final discriminator, so two distinct tasks
    //    never compare equal and the output order is fully determined.
    return lhs_id < rhs_id;
}

} // namespace

double PriorityEngine::urgencyFactor(std::optional<std::int64_t> due_at, std::int64_t now,
                                     double horizon_days)
{
    // The four documented cases, checked in this order (docs/scoring.md,
    // section "urgency"):
    //
    // 1. NO DEADLINE -> 0.0. An undated task is not urgent. The tempting
    //    alternative — treating "no date" as maximum urgency — makes every
    //    vague someday-item outrank real deadlines and destroys the queue.
    if (!due_at.has_value())
    {
        return 0.0;
    }

    // 2. OVERDUE OR DUE NOW -> 1.0, saturating. A task 30 days late is not
    //    three times as urgent as one 10 days late, and an unbounded term
    //    would grow until it drowned out the other three.
    //
    //    The conversion to double happens BEFORE the subtraction: epoch
    //    seconds are far below 2^53 so the values are exact, and doing the
    //    arithmetic in floating point sidesteps signed overflow on a
    //    pathological due_at near INT64_MIN.
    const double seconds_remaining = static_cast<double>(*due_at) - static_cast<double>(now);
    if (seconds_remaining <= 0.0)
    {
        return 1.0;
    }

    const double days_remaining = seconds_remaining / kSecondsPerDayAsDouble;

    // 3. BEYOND THE HORIZON -> 0.0. A deadline three weeks out is not
    //    actionable this morning and must not crowd out today's work.
    //
    //    Written as `!(days_remaining < horizon_days)` rather than
    //    `days_remaining >= horizon_days` on purpose: a NaN horizon makes
    //    every ordered comparison false, so the negated form still rejects
    //    it here instead of letting NaN poison the ramp and, with it, every
    //    comparison in the queue. A non-positive horizon lands here too
    //    (nothing is inside an empty horizon), which keeps the division in
    //    case 4 away from zero. Weights::validate() already rejects such a
    //    horizon before it can reach production.
    if (!(days_remaining < horizon_days))
    {
        return 0.0;
    }

    // 4. WITHIN THE HORIZON -> 1 - days_remaining / horizon_days: a linear
    //    ramp, so urgency grows steadily and visibly as the deadline
    //    approaches. Exactly at the horizon this is 0.0 (handled above); one
    //    second inside it is tiny but strictly positive.
    return 1.0 - (days_remaining / horizon_days);
}

double PriorityEngine::ageDays(std::int64_t created_at, std::int64_t now)
{
    // Fractional days, and CLAMPED at 0.0. The clamp is the whole point of
    // this function's pre-checks: without it a task whose created_at is in
    // the future (clock skew, a hand-edited row) would earn NEGATIVE age and
    // therefore SINK in the queue. An aging term that can subtract is worse
    // than no aging term at all.
    //
    // Subtraction happens in double for the same reason as urgencyFactor:
    // it avoids signed overflow, and epoch seconds are exactly representable.
    const double seconds_since_creation = static_cast<double>(now) - static_cast<double>(created_at);
    if (seconds_since_creation < 0.0)
    {
        return 0.0;
    }

    return seconds_since_creation / kSecondsPerDayAsDouble;
}

ScoreBreakdown PriorityEngine::score(const Task &task, std::int64_t now, const Weights &weights)
{
    // The four weighted terms, in the order the formula in docs/scoring.md
    // declares them. Each is computed exactly once, from raw inputs.
    const double urgency_factor = urgencyFactor(task.due_at, now, weights.urgency_horizon_days);
    const double age_days = ageDays(task.created_at, now);

    ScoreBreakdown breakdown;
    breakdown.importance = weights.importance * static_cast<double>(task.importance);
    breakdown.urgency = weights.urgency * urgency_factor;
    breakdown.age = weights.age * age_days;
    breakdown.blocks = weights.blocks * static_cast<double>(task.blocks);

    // total is the sum of the four parts AS COMPUTED, not a second
    // independent evaluation of the formula: a client that re-sums the parts
    // must land on the same number it was shown, or the evidence contradicts
    // the verdict.
    breakdown.total = breakdown.importance + breakdown.urgency + breakdown.age + breakdown.blocks;
    return breakdown;
}

std::vector<RankedTask> PriorityEngine::rank(std::span<const Task> tasks, std::int64_t now,
                                            const Weights &weights)
{
    // 1. Discard everything that is not actionable (kDone, kArchived). Note
    //    that a done task keeps its score; it is excluded because finished
    //    work is not a queue candidate, not because it ranks badly.
    // 2. Score each survivor and record the raw inputs behind the score, so
    //    a client can see WHY the task landed where it did.
    std::vector<RankedTask> ranked;
    ranked.reserve(tasks.size());

    for (const Task &task : tasks)
    {
        if (!task.isActionable())
        {
            continue;
        }

        RankedTask entry;
        entry.task = task;
        entry.score = score(task, now, weights);

        // The raw inputs are recomputed rather than plumbed out of score():
        // both are pure functions of (task, now, weights), so the values are
        // identical to the ones the parts were built from, and score() stays
        // the single source of truth for the four parts.
        entry.urgency_factor = urgencyFactor(task.due_at, now, weights.urgency_horizon_days);
        entry.age_days = ageDays(task.created_at, now);

        ranked.push_back(std::move(entry));
    }

    // 3. Sort by the total order. A total order matters here for more than
    //    tidiness: with equal-scoring tasks the queue would otherwise depend
    //    on the caller's input order and on the sort implementation's
    //    internals, so the same backlog could produce two different "next
    //    task" answers on two runs. The three keys make that impossible.
    std::sort(ranked.begin(), ranked.end(),
              [](const RankedTask &lhs, const RankedTask &rhs)
              {
                  return precedesByKeys(lhs.score.total, lhs.task.created_at, lhs.task.id,
                                        rhs.score.total, rhs.task.created_at, rhs.task.id);
              });

    return ranked;
}

void PriorityEngine::sortByScore(std::vector<Task> &tasks, std::int64_t now, const Weights &weights)
{
    // In place, same total order as rank(), and deliberately WITHOUT the
    // actionable filter: this overload sorts whatever the caller handed over
    // (that is how a caller reorders a mixed view without losing the done
    // rows it is still displaying).
    //
    // The score is recomputed inside the comparator instead of being
    // decorated onto a copy of each task: score() is a handful of flops over
    // immutable inputs, so the extra evaluations cost nothing at task-list
    // scale, and recomputing keeps this function honest about using the same
    // pure rules as rank() rather than a second, parallel implementation of
    // the key that could drift.
    std::sort(tasks.begin(), tasks.end(),
              [&now, &weights](const Task &lhs, const Task &rhs)
              {
                  return precedesByKeys(score(lhs, now, weights).total, lhs.created_at, lhs.id,
                                        score(rhs, now, weights).total, rhs.created_at, rhs.id);
              });
}

nlohmann::json toJson(const RankedTask &ranked)
{
    // Start from the task's own serialization so a RankedTask is a strict
    // superset of a Task on the wire: a client that already understands the
    // task shape needs to learn only the four added keys, and the two
    // serializers cannot drift because the task half is not re-implemented
    // here.
    nlohmann::json json = toJson(ranked.task);

    // `score` is the verdict; `score_parts` is the evidence behind it, kept
    // in the same four terms the formula is written with. A client should
    // display `score` — the parts are float-rounded and re-summing them can
    // differ from `total` in the last bits.
    json["score"] = ranked.score.total;
    json["score_parts"] = {
        { "importance", ranked.score.importance },
        { "urgency", ranked.score.urgency },
        { "age", ranked.score.age },
        { "blocks", ranked.score.blocks },
    };

    // The raw inputs, before the weights were applied. They are what makes
    // "A is first because it is due today and blocks two others" sayable
    // from the payload alone.
    json["urgency_factor"] = ranked.urgency_factor;
    json["age_days"] = ranked.age_days;

    return json;
}

} // namespace taskpilot
