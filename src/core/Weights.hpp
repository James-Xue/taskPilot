#pragma once
// Weights.hpp — tunable coefficients of the ranking formula
//
// The formula (see docs/scoring.md for the derivation and worked examples):
//
//   score = importance_w * importance
//         + urgency_w    * urgencyFactor(due_at, now, horizon_days)
//         + age_w        * ageDays(created_at, now)
//         + blocks_w     * blocks
//
// Weights are data, not code, so they can be retuned at runtime over MCP
// (set_weights) without a rebuild. They persist in the store's settings
// table and survive restarts.

#include <nlohmann/json.hpp>

#include "core/Result.hpp"

namespace taskpilot
{

/// Inclusive upper bound for every score weight.
///
/// Finiteness alone does not keep a SCORE finite, because a weight never
/// reaches the wire by itself: the payload carries the weight multiplied by a
/// task field. A weight of 1e308 is a legal, finite double, yet 1e308 * 5
/// overflows to +inf, and nlohmann writes a non-finite double as JSON null —
/// so the score silently stops being the number every client parses it as,
/// with no error on any path. 1e9 stays orders of magnitude below that
/// failure (even 1e9 times the largest field a task can carry is many orders
/// below DBL_MAX) while remaining far above any real tuning: the largest
/// documented default is 30, so a weight over this bound is a typo, not an
/// intent.
inline constexpr double kMaxWeight{ 1e9 };

/// Ranking coefficients.
///
/// Defaults are chosen so that the four terms are comparable in magnitude for
/// a typical backlog, which keeps the queue's behaviour legible:
///   - importance spans 1..5, so importance_w = 5 gives a 5..25 range.
///   - urgency is 0..1, so urgency_w = 30 lets a deadline-today task outweigh
///     even a 5/5 importance gap — deadlines are the strongest signal here.
///   - age is in days, so age_w = 2 means a week of neglect adds 14 points:
///     enough to lift a stale chore above fresh trivia, not enough to beat a
///     real deadline.
///   - blocks is a count, so blocks_w = 8 makes one blocked dependent (8
///     points) outweigh a single step of importance (5), two of them (16)
///     rival the widest possible importance gap (20) without exceeding it, and
///     three (24) exceed it outright. That is the intended semantics: work
///     that frees other work goes first.
struct Weights
{
    double importance{ 5.0 };
    double urgency{ 30.0 };
    double age{ 2.0 };
    double blocks{ 8.0 };

    /// Number of days over which urgency ramps from 0 (at or beyond the
    /// horizon) to 1 (due now or overdue). Must be > 0.
    double urgency_horizon_days{ 7.0 };

    /// Validate the invariants the scoring math relies on:
    /// 1. Every weight is finite (a NaN or inf would poison every comparison
    ///    and silently destroy the queue order).
    /// 2. Every weight is >= 0 (negative weights would invert a term's
    ///    meaning and are far more likely a typo than an intent).
    /// 3. Every weight is <= kMaxWeight, so the product with a task field
    ///    stays finite on the wire; see that constant for the full reasoning.
    ///    The horizon carries no upper bound, because it is divided INTO the
    ///    urgency ramp rather than multiplied into the score, so a huge
    ///    horizon yields a small urgency and never a non-finite score.
    /// 4. urgency_horizon_days > 0 (otherwise urgency divides by zero).
    [[nodiscard]] Status validate() const;
};

/// Serialize for the wire and for the settings table.
[[nodiscard]] nlohmann::json toJson(const Weights &weights);

/// Parse weights from JSON. Unknown keys are ignored so a newer client can
/// talk to an older server; invalid values produce kInvalidArgument rather
/// than being silently clamped, because a silently-ignored weight change is
/// indistinguishable from a failed one.
[[nodiscard]] Result<Weights> weightsFromJson(const nlohmann::json &json);

} // namespace taskpilot
