#pragma once
// PriorityEngine.hpp — the ranking math
//
// This is the heart of taskPilot and the only place that decides what "next"
// means. It is a set of pure functions over (task, now, weights): no clock,
// no storage, no I/O. Everything here is deterministic and directly testable.
//
//   score = importance_w * importance
//         + urgency_w    * urgencyFactor(due_at, now, horizon_days)
//         + age_w        * ageDays(created_at, now)
//         + blocks_w     * blocks
//
// See docs/scoring.md for the rationale behind each term and worked examples.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Task.hpp"
#include "core/Weights.hpp"

namespace taskpilot
{

/// Per-term contribution to a task's score, retained so a client can see WHY
/// something ranked where it did. The four term fields sum to `total` up to
/// floating-point rounding; always display `total`, never the re-summed parts.
struct ScoreBreakdown
{
    double importance{ 0.0 };
    double urgency{ 0.0 };
    double age{ 0.0 };
    double blocks{ 0.0 };
    double total{ 0.0 };
};

/// A task together with the evidence for its position in the queue.
struct RankedTask
{
    Task task;
    ScoreBreakdown score;

    /// Raw inputs to the score, kept for display. urgency_factor is the
    /// 0..1 ramp value before the weight; age_days is fractional.
    double urgency_factor{ 0.0 };
    double age_days{ 0.0 };
};

/// The priority engine's scoring rules.
///
/// Thread-safety: every member is a pure static function over its arguments
/// and touches no shared state, so all methods are safe to call concurrently.
class PriorityEngine
{
  public:
    /// Map a deadline onto 0..1 urgency.
    ///
    /// 1. No deadline      -> 0.0. An un-dated task is not urgent; treating a
    ///                        missing date as maximum urgency would let every
    ///                        undated task outrank real deadlines.
    /// 2. Overdue (or due now) -> 1.0. Urgency saturates: a task 30 days late
    ///                        is not three times as urgent as one 10 days
    ///                        late, and unbounded growth would swamp the
    ///                        other three terms.
    /// 3. Within the horizon -> 1 - days_remaining / horizon_days, a linear
    ///                        ramp so urgency grows steadily as a deadline
    ///                        approaches.
    /// 4. Beyond the horizon -> 0.0. Distant deadlines are not yet actionable
    ///                        and must not crowd out present work.
    [[nodiscard]] static double urgencyFactor(std::optional<std::int64_t> due_at,
                                              std::int64_t now,
                                              double horizon_days);

    /// Fractional days since creation. Clamped at 0.0 so a task whose
    /// created_at is in the future (clock skew, hand-edited row) cannot earn
    /// NEGATIVE age and thereby sink in the queue — an aging term that can
    /// subtract is worse than no aging term at all.
    [[nodiscard]] static double ageDays(std::int64_t created_at, std::int64_t now);

    /// Full score with per-term contributions.
    [[nodiscard]] static ScoreBreakdown score(const Task &task, std::int64_t now,
                                              const Weights &weights);

    /// Rank `tasks`, discarding anything not actionable (see Task::isActionable).
    ///
    /// Ordering is a total order, so the queue is stable across calls:
    /// 1. Higher score first.
    /// 2. On a tie, older created_at first — a task that has waited longer
    ///    wins, which is the same fairness instinct as the aging term.
    /// 3. On a full tie, the smaller `uid` first, so the result never depends
    ///    on the input order or on the sort implementation.
    ///
    /// Step 3 deliberately uses `uid` rather than the integer `id`. The id is a
    /// LOCAL row number and is not transported by the sync format (see Task.id
    /// and docs/sync.md), and a merge assigns ids in uid order — which is
    /// random with respect to creation order. Tying on it would therefore make
    /// the queue order differ between two machines holding identical backlogs,
    /// contradicting the guarantee docs/sync.md makes that the order IS
    /// comparable across machines. `uid` is the one field every machine agrees
    /// on, so it is the only correct final tie-break.
    [[nodiscard]] static std::vector<RankedTask> rank(std::span<const Task> tasks,
                                                      std::int64_t now,
                                                      const Weights &weights);

    /// Sort in place, using the same total order as rank().
    static void sortByScore(std::vector<Task> &tasks, std::int64_t now,
                            const Weights &weights);
};

/// Serialize a ranked task: the task's own fields, flattened, plus `score`
/// (total) and `score_parts` (the four contributions).
[[nodiscard]] nlohmann::json toJson(const RankedTask &ranked);

} // namespace taskpilot
