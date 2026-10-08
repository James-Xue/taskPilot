// test_priority_engine.cpp — asserts docs/scoring.md, not the implementation.
//
// Every expectation in this file is derived from the specification document:
// the four cases of urgencyFactor, the clamped age, the worked example table,
// the aging flip, the tie-breakers, and the "pure FIFO" degenerate case. A
// refactor of PriorityEngine.cpp that keeps these tests green keeps the queue
// honest; a change that breaks them is a change to the ranking RULES, which
// is a product decision rather than a refactor.
//
// The harness is time-free by construction: `now` is a fixed constant, so
// every assertion below is exact and nothing here can become a flake.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/Clock.hpp"
#include "core/PriorityEngine.hpp"
#include "core/Task.hpp"
#include "core/Weights.hpp"

namespace taskpilot
{
namespace
{

/// Fixed instant for every test. Ranking is a pure function of `now`, so
/// pinning it removes the only source of nondeterminism in the whole file.
constexpr std::int64_t kNow = 1'700'000'000;

/// Tolerance for any comparison against a value the specification states as
/// an exact expression (25.0, 180/7, ...). Loose enough for double rounding,
/// far tighter than any number the document prints.
constexpr double kTolerance = 1e-9;

/// The document's own numbers are printed rounded (two decimals in the table,
/// eight significant figures in the prose), so they are checked at the
/// precision they were written to rather than at kTolerance.
constexpr double kPrintedTableTolerance = 5e-3;
constexpr double kPrintedProseTolerance = 1e-6;

/// Build an open task, exposing only the fields ranking reads so no test can
/// accidentally depend on the rest.
Task makeTask(std::int64_t id, std::string title, int importance,
              std::optional<std::int64_t> due_at, std::int64_t created_at, int blocks)
{
    Task task;
    task.id = id;
    task.title = std::move(title);
    task.status = TaskStatus::kOpen;
    task.importance = importance;
    task.due_at = due_at;
    task.blocks = blocks;
    task.created_at = created_at;
    task.updated_at = created_at;
    return task;
}

/// Titles in queue order — the readable form of "what came first".
std::vector<std::string> titlesOf(const std::vector<RankedTask> &ranked)
{
    std::vector<std::string> titles;
    titles.reserve(ranked.size());
    for (const RankedTask &entry : ranked)
    {
        titles.push_back(entry.task.title);
    }
    return titles;
}

/// nullptr when absent, so a caller can ASSERT before dereferencing.
const RankedTask *findByTitle(const std::vector<RankedTask> &ranked, const std::string &title)
{
    const auto found = std::find_if(ranked.begin(), ranked.end(),
                                    [&title](const RankedTask &entry)
                                    { return entry.task.title == title; });
    if (found == ranked.end())
    {
        return nullptr;
    }
    return &*found;
}

/// Four parts plus their sum, so an expectation can be written as the five
/// numbers the document's table shows.
ScoreBreakdown breakdownOf(double importance, double urgency, double age, double blocks)
{
    ScoreBreakdown expected;
    expected.importance = importance;
    expected.urgency = urgency;
    expected.age = age;
    expected.blocks = blocks;
    expected.total = importance + urgency + age + blocks;
    return expected;
}

void expectBreakdown(const std::vector<RankedTask> &ranked, const std::string &title,
                     const ScoreBreakdown &expected, double tolerance)
{
    const RankedTask *found = findByTitle(ranked, title);
    ASSERT_NE(nullptr, found) << "no ranked task titled " << title;
    EXPECT_NEAR(expected.importance, found->score.importance, tolerance);
    EXPECT_NEAR(expected.urgency, found->score.urgency, tolerance);
    EXPECT_NEAR(expected.age, found->score.age, tolerance);
    EXPECT_NEAR(expected.blocks, found->score.blocks, tolerance);
    EXPECT_NEAR(expected.total, found->score.total, tolerance);
}

/// The worked example from docs/scoring.md, section "Worked example".
///
/// The document titles its four rows after a WebSocket reconnect fix, a
/// monthly report, a K-line data cleanup and an email reply; they are titled
/// A..D here, and the ranking INPUTS are the document's exactly (the titles
/// carry no score, so only the labels differ):
///
///   A: importance 5, due today,     waited 3d, blocks 2
///   B: importance 4, due in 1d,     waited 1d, blocks 0
///   C: importance 3, due in 5d,     waited 9d, blocks 0
///   D: importance 2, due in 2d,     waited 1d, blocks 0
///
/// Ids are deliberately NOT in queue order and disagree with both created_at
/// and title order: an implementation that fell back to "sort by id" (or that
/// happened to be stable) would produce a different sequence and fail the
/// order assertions instead of passing them by accident.
std::vector<Task> workedExampleTasks()
{
    return {
        makeTask(40, "A", 5, kNow, kNow - 3 * kSecondsPerDay, 2),
        makeTask(10, "B", 4, kNow + 1 * kSecondsPerDay, kNow - 1 * kSecondsPerDay, 0),
        makeTask(30, "C", 3, kNow + 5 * kSecondsPerDay, kNow - 9 * kSecondsPerDay, 0),
        makeTask(20, "D", 2, kNow + 2 * kSecondsPerDay, kNow - 1 * kSecondsPerDay, 0),
    };
}

// ---------------------------------------------------------------------------
// urgencyFactor — the four documented cases, including every boundary
// ---------------------------------------------------------------------------

TEST(PriorityEngineUrgency, NoDeadlineScoresZeroNotMaximum)
{
    // Case 1. The tempting inversion — "no date means do it now" — would let
    // every vague someday-item outrank real deadlines, which is the failure
    // mode this rule exists to prevent.
    EXPECT_DOUBLE_EQ(0.0, PriorityEngine::urgencyFactor(std::nullopt, kNow, 7.0));
}

TEST(PriorityEngineUrgency, DueNowAndOverdueSaturateAtOne)
{
    // Case 2. The boundary is inclusive: a task due exactly now is already
    // past the point of ramping.
    EXPECT_DOUBLE_EQ(1.0, PriorityEngine::urgencyFactor(kNow, kNow, 7.0));
    // One second late is still 1.0 ...
    EXPECT_DOUBLE_EQ(1.0, PriorityEngine::urgencyFactor(kNow - 1, kNow, 7.0));
    // ... and 30 days late is still 1.0. Urgency saturates: it does not grow
    // without bound, or it would drown out the other three terms.
    EXPECT_DOUBLE_EQ(1.0, PriorityEngine::urgencyFactor(kNow - 30 * kSecondsPerDay, kNow, 7.0));
}

TEST(PriorityEngineUrgency, HorizonBoundaries)
{
    constexpr double kHorizonDays = 7.0;
    const std::int64_t horizon_seconds = static_cast<std::int64_t>(kHorizonDays) * kSecondsPerDay;

    // Case 4 boundary: exactly at the horizon is already "beyond" it and
    // scores 0.0 — the far end of the ramp is closed, not open.
    EXPECT_DOUBLE_EQ(0.0, PriorityEngine::urgencyFactor(kNow + horizon_seconds, kNow, kHorizonDays));

    // One second past the horizon is 0.0 as well (no negative urgency).
    EXPECT_DOUBLE_EQ(0.0,
                     PriorityEngine::urgencyFactor(kNow + horizon_seconds + 1, kNow, kHorizonDays));

    // Case 3 boundary: one second INSIDE the horizon is tiny but strictly
    // positive. If the last second ever rounded down to "not urgent at all"
    // the ramp would be a step function and the boundary would be invisible.
    const double one_second_inside =
        PriorityEngine::urgencyFactor(kNow + horizon_seconds - 1, kNow, kHorizonDays);
    EXPECT_GT(one_second_inside, 0.0);
    EXPECT_NEAR(1.0 - 604799.0 / 604800.0, one_second_inside, 1e-15);

    // Midpoint of the ramp: half the horizon left is exactly half urgency.
    EXPECT_NEAR(0.5, PriorityEngine::urgencyFactor(kNow + horizon_seconds / 2, kNow, kHorizonDays),
                1e-15);

    // A quarter of the horizon left is three quarters urgency — the ramp is
    // linear, not merely monotone.
    EXPECT_NEAR(0.75,
                PriorityEngine::urgencyFactor(kNow + horizon_seconds / 4, kNow, kHorizonDays),
                1e-15);
}

TEST(PriorityEngineUrgency, HorizonIsAParameterNotAConstant)
{
    // The same 12-hours-out deadline is half urgent under a one-day horizon
    // and 1/14 urgent under the default seven-day horizon. If the horizon
    // were ever hard-coded, one of these two would break.
    EXPECT_NEAR(0.5, PriorityEngine::urgencyFactor(kNow + kSecondsPerDay / 2, kNow, 1.0), 1e-15);
    EXPECT_NEAR(1.0 - 0.5 / 7.0,
                PriorityEngine::urgencyFactor(kNow + kSecondsPerDay / 2, kNow, 7.0), 1e-15);
}

TEST(PriorityEngineUrgency, NonPositiveOrNonFiniteHorizonYieldsZero)
{
    // Weights::validate() rejects a horizon <= 0, so this is a guard rather
    // than a supported mode: urgencyFactor must never divide by zero if one
    // reaches it anyway. A future deadline is by definition not inside an
    // empty horizon.
    EXPECT_DOUBLE_EQ(0.0, PriorityEngine::urgencyFactor(kNow + kSecondsPerDay, kNow, 0.0));
    EXPECT_DOUBLE_EQ(0.0, PriorityEngine::urgencyFactor(kNow + kSecondsPerDay, kNow, -1.0));
    // A NaN horizon would otherwise poison every comparison downstream; the
    // guard rejects it too.
    EXPECT_DOUBLE_EQ(
        0.0, PriorityEngine::urgencyFactor(kNow + kSecondsPerDay, kNow,
                                           std::numeric_limits<double>::quiet_NaN()));
    // Saturation is decided BEFORE the horizon is consulted, so an overdue
    // task stays maximally urgent even under a degenerate horizon.
    EXPECT_DOUBLE_EQ(1.0, PriorityEngine::urgencyFactor(kNow - kSecondsPerDay, kNow, 0.0));
}

// ---------------------------------------------------------------------------
// ageDays — fractional, and clamped at zero
// ---------------------------------------------------------------------------

TEST(PriorityEngineAge, ZeroAndFractionalDays)
{
    EXPECT_DOUBLE_EQ(0.0, PriorityEngine::ageDays(kNow, kNow));
    EXPECT_NEAR(0.5, PriorityEngine::ageDays(kNow - kSecondsPerDay / 2, kNow), 1e-15);
    EXPECT_NEAR(0.5 / 24.0, PriorityEngine::ageDays(kNow - 1800, kNow), 1e-15);
    EXPECT_NEAR(9.0, PriorityEngine::ageDays(kNow - 9 * kSecondsPerDay, kNow), 1e-15);
}

TEST(PriorityEngineAge, FutureCreatedAtClampsToZero)
{
    // The clamp. A future created_at (clock skew, a hand-edited row) must not
    // earn negative age: a term that can SUBTRACT would push a task down the
    // queue, which is worse than having no aging term at all.
    EXPECT_DOUBLE_EQ(0.0, PriorityEngine::ageDays(kNow + 1, kNow));
    EXPECT_DOUBLE_EQ(0.0, PriorityEngine::ageDays(kNow + kSecondsPerDay, kNow));
    EXPECT_DOUBLE_EQ(0.0, PriorityEngine::ageDays(kNow + 100 * kSecondsPerDay, kNow));
    EXPECT_GE(PriorityEngine::ageDays(kNow + 5 * kSecondsPerDay, kNow), 0.0);
}

// ---------------------------------------------------------------------------
// score — the four weighted terms
// ---------------------------------------------------------------------------

TEST(PriorityEngineScore, MissingDeadlineContributesZeroUrgency)
{
    // Same task as example A but undated: everything else is unchanged, and
    // the 30-point urgency term collapses to 0. This is the concrete shape of
    // "no deadline scores zero, not maximum".
    const Task task = makeTask(1, "someday", 5, std::nullopt, kNow - 3 * kSecondsPerDay, 2);
    const ScoreBreakdown breakdown = PriorityEngine::score(task, kNow, Weights{});

    EXPECT_NEAR(25.0, breakdown.importance, kTolerance);
    EXPECT_NEAR(0.0, breakdown.urgency, kTolerance);
    EXPECT_NEAR(6.0, breakdown.age, kTolerance);
    EXPECT_NEAR(16.0, breakdown.blocks, kTolerance);
    EXPECT_NEAR(47.0, breakdown.total, kTolerance);
}

TEST(PriorityEngineScore, TotalIsTheSumOfTheFourParts)
{
    // A client is shown both the parts and the total, so the two must agree:
    // a total computed by a second, differently-ordered evaluation of the
    // formula could differ in the last bits and make the evidence look wrong.
    const Task task = makeTask(1, "t", 4, kNow + 2 * kSecondsPerDay, kNow - 5 * kSecondsPerDay, 3);
    const ScoreBreakdown breakdown = PriorityEngine::score(task, kNow, Weights{});
    const double re_summed =
        breakdown.importance + breakdown.urgency + breakdown.age + breakdown.blocks;
    EXPECT_DOUBLE_EQ(re_summed, breakdown.total);
}

TEST(PriorityEngineScore, EachWeightScalesOnlyItsOwnTerm)
{
    // The property set_weights relies on at runtime: changing one coefficient
    // moves exactly one part and leaves the other three alone.
    const Task task = makeTask(1, "t", 4, kNow + kSecondsPerDay, kNow - 2 * kSecondsPerDay, 3);
    const ScoreBreakdown base = PriorityEngine::score(task, kNow, Weights{});

    Weights heavier;
    heavier.importance = 10.0;
    const ScoreBreakdown scaled = PriorityEngine::score(task, kNow, heavier);

    EXPECT_NEAR(base.importance * 2.0, scaled.importance, kTolerance);
    EXPECT_NEAR(base.urgency, scaled.urgency, kTolerance);
    EXPECT_NEAR(base.age, scaled.age, kTolerance);
    EXPECT_NEAR(base.blocks, scaled.blocks, kTolerance);
    EXPECT_NEAR(base.total + base.importance, scaled.total, kTolerance);
}

TEST(PriorityEngineScore, ZeroWeightSwitchesATermOff)
{
    // docs/scoring.md, section "Retuning": weight 0.0 is the supported way to
    // switch a term off, and it must zero the part rather than merely shrink
    // it (an unbounded term like age would otherwise still leak through).
    Weights no_age;
    no_age.age = 0.0;

    const Task stale = makeTask(1, "stale", 3, std::nullopt, kNow - 900 * kSecondsPerDay, 0);
    const ScoreBreakdown breakdown = PriorityEngine::score(stale, kNow, no_age);

    EXPECT_DOUBLE_EQ(0.0, breakdown.age);
    EXPECT_NEAR(15.0, breakdown.importance, kTolerance);
    EXPECT_NEAR(15.0, breakdown.total, kTolerance);
}

// ---------------------------------------------------------------------------
// The worked example table, exactly
// ---------------------------------------------------------------------------

TEST(PriorityEngineRank, WorkedExamplePartsMatchTheSpecification)
{
    const std::vector<RankedTask> ranked = PriorityEngine::rank(workedExampleTasks(), kNow, Weights{});
    ASSERT_EQ(std::size_t{ 4 }, ranked.size());

    // Parts derived from the formula, not copied from the printed table:
    //   importance: 5 x 5, 4 x 5, 3 x 5, 2 x 5
    //   urgency   : 30 x (1 - 0/7), 30 x (1 - 1/7), 30 x (1 - 5/7), 30 x (1 - 2/7)
    //   age       : 2 x 3d, 2 x 1d, 2 x 9d, 2 x 1d
    //   blocks    : 8 x 2, 8 x 0, 8 x 0, 8 x 0
    expectBreakdown(ranked, "A", breakdownOf(25.0, 30.0 * (1.0 - 0.0 / 7.0), 6.0, 16.0), kTolerance);
    expectBreakdown(ranked, "B", breakdownOf(20.0, 30.0 * (1.0 - 1.0 / 7.0), 2.0, 0.0), kTolerance);
    expectBreakdown(ranked, "C", breakdownOf(15.0, 30.0 * (1.0 - 5.0 / 7.0), 18.0, 0.0), kTolerance);
    expectBreakdown(ranked, "D", breakdownOf(10.0, 30.0 * (1.0 - 2.0 / 7.0), 2.0, 0.0), kTolerance);

    // The totals the document names: A=77.00, B=47.7142857, C=41.5714286,
    // D=33.4285714. Checked at kTolerance against the exact values.
    const RankedTask *a = findByTitle(ranked, "A");
    const RankedTask *b = findByTitle(ranked, "B");
    const RankedTask *c = findByTitle(ranked, "C");
    const RankedTask *d = findByTitle(ranked, "D");
    ASSERT_NE(nullptr, a);
    ASSERT_NE(nullptr, b);
    ASSERT_NE(nullptr, c);
    ASSERT_NE(nullptr, d);
    EXPECT_NEAR(77.0, a->score.total, kTolerance);
    EXPECT_NEAR(20.0 + 180.0 / 7.0 + 2.0, b->score.total, kTolerance);
    EXPECT_NEAR(15.0 + 60.0 / 7.0 + 18.0, c->score.total, kTolerance);
    EXPECT_NEAR(10.0 + 150.0 / 7.0 + 2.0, d->score.total, kTolerance);

    // The raw inputs are what makes the ranking explainable, so they must
    // survive into the result alongside the weighted parts.
    EXPECT_NEAR(1.0, a->urgency_factor, kTolerance);
    EXPECT_NEAR(3.0, a->age_days, kTolerance);
    EXPECT_NEAR(1.0 - 5.0 / 7.0, c->urgency_factor, kTolerance);
    EXPECT_NEAR(9.0, c->age_days, kTolerance);
}

TEST(PriorityEngineRank, WorkedExampleTableIsReproducedAsPrinted)
{
    const std::vector<RankedTask> ranked = PriorityEngine::rank(workedExampleTasks(), kNow, Weights{});

    // The per-term table as the document prints it (two decimals per cell).
    // Asserted at the precision it was written to: the underlying values are
    // 180/7, 60/7 and 150/7, which no two-decimal rendering can hold exactly.
    expectBreakdown(ranked, "A", breakdownOf(25.00, 30.00, 6.00, 16.00), kPrintedTableTolerance);
    expectBreakdown(ranked, "B", breakdownOf(20.00, 25.71, 2.00, 0.00), kPrintedTableTolerance);
    expectBreakdown(ranked, "C", breakdownOf(15.00, 8.57, 18.00, 0.00), kPrintedTableTolerance);
    expectBreakdown(ranked, "D", breakdownOf(10.00, 21.43, 2.00, 0.00), kPrintedTableTolerance);

    // The totals as the document's prose states them.
    const RankedTask *a = findByTitle(ranked, "A");
    const RankedTask *b = findByTitle(ranked, "B");
    const RankedTask *c = findByTitle(ranked, "C");
    const RankedTask *d = findByTitle(ranked, "D");
    ASSERT_NE(nullptr, a);
    ASSERT_NE(nullptr, b);
    ASSERT_NE(nullptr, c);
    ASSERT_NE(nullptr, d);
    EXPECT_NEAR(77.00, a->score.total, kPrintedProseTolerance);
    EXPECT_NEAR(47.7142857, b->score.total, kPrintedProseTolerance);
    EXPECT_NEAR(41.5714286, c->score.total, kPrintedProseTolerance);
    EXPECT_NEAR(33.4285714, d->score.total, kPrintedProseTolerance);
}

TEST(PriorityEngineRank, WorkedExampleQueueOrderIsABCD)
{
    const std::vector<std::string> expected{ "A", "B", "C", "D" };
    const std::vector<RankedTask> ranked = PriorityEngine::rank(workedExampleTasks(), kNow, Weights{});
    EXPECT_EQ(expected, titlesOf(ranked));
}

// ---------------------------------------------------------------------------
// The aging flip — the load-bearing test the document calls out by name
// ---------------------------------------------------------------------------

TEST(PriorityEngineRank, AgingTermLiftsCAboveDAndRemovingItFlipsThemBack)
{
    // docs/scoring.md: "Without it, C would score 23.57 and land below D's
    // 31.43 ... That flip is the entire point of the term." This is the test
    // that must fail loudly if someone ever "simplifies" aging away.
    const std::vector<Task> tasks = workedExampleTasks();

    // Aging OFF: the same weights with age=0.
    Weights no_aging;
    no_aging.importance = 5.0;
    no_aging.urgency = 30.0;
    no_aging.age = 0.0;
    no_aging.blocks = 8.0;

    const std::vector<RankedTask> with_aging = PriorityEngine::rank(tasks, kNow, Weights{});
    const std::vector<RankedTask> without_aging = PriorityEngine::rank(tasks, kNow, no_aging);

    // Scores first, so a failure names the number that moved rather than only
    // the position.
    const RankedTask *c_aged = findByTitle(with_aging, "C");
    const RankedTask *d_aged = findByTitle(with_aging, "D");
    ASSERT_NE(nullptr, c_aged);
    ASSERT_NE(nullptr, d_aged);
    EXPECT_NEAR(15.0 + 60.0 / 7.0 + 18.0, c_aged->score.total, kTolerance);
    EXPECT_NEAR(10.0 + 150.0 / 7.0 + 2.0, d_aged->score.total, kTolerance);
    EXPECT_GT(c_aged->score.total, d_aged->score.total);

    const RankedTask *c_fresh = findByTitle(without_aging, "C");
    const RankedTask *d_fresh = findByTitle(without_aging, "D");
    ASSERT_NE(nullptr, c_fresh);
    ASSERT_NE(nullptr, d_fresh);
    EXPECT_NEAR(23.571428571428573, c_fresh->score.total, kTolerance);
    EXPECT_NEAR(31.428571428571427, d_fresh->score.total, kTolerance);
    EXPECT_GT(d_fresh->score.total, c_fresh->score.total);

    // And the queue order flips with the scores.
    const std::vector<std::string> with_aging_order{ "A", "B", "C", "D" };
    const std::vector<std::string> no_aging_order{ "A", "B", "D", "C" };
    EXPECT_EQ(with_aging_order, titlesOf(with_aging));
    EXPECT_EQ(no_aging_order, titlesOf(without_aging));
}

// ---------------------------------------------------------------------------
// Tie-breaking and the total order
// ---------------------------------------------------------------------------

TEST(PriorityEngineRank, TiesBreakOnOlderCreatedAtThenLowerId)
{
    // age is switched off so created_at does NOT feed the score. That is what
    // makes the three totals genuinely equal: with aging on, the older task
    // would simply score higher and the tie-breakers would never run at all.
    Weights no_age;
    no_age.age = 0.0;

    std::vector<Task> tasks{
        makeTask(7, "younger-id", 3, std::nullopt, kNow - 2 * kSecondsPerDay, 0),
        makeTask(2, "older", 3, std::nullopt, kNow - 5 * kSecondsPerDay, 0),
        makeTask(4, "twin-lower-id", 3, std::nullopt, kNow - 2 * kSecondsPerDay, 0),
    };

    const std::vector<RankedTask> ranked = PriorityEngine::rank(tasks, kNow, no_age);
    ASSERT_EQ(std::size_t{ 3 }, ranked.size());
    for (const RankedTask &entry : ranked)
    {
        EXPECT_DOUBLE_EQ(0.0, entry.score.age);
        EXPECT_DOUBLE_EQ(ranked.front().score.total, entry.score.total);
    }

    // "older" wins key 2. The two remaining tasks are a FULL tie (equal score
    // and equal created_at), so they fall through to key 3 and the lower id
    // wins: 4 before 7 — never the input order, which lists 7 first.
    const std::vector<std::string> expected{ "older", "twin-lower-id", "younger-id" };
    EXPECT_EQ(expected, titlesOf(ranked));
}

TEST(PriorityEngineRank, AllZeroWeightsIsPureFifo)
{
    // docs/scoring.md, section "Retuning": all four weights at zero is legal
    // and produces a queue ordered entirely by the tie-breakers — oldest
    // first. It is a defensible FIFO mode, not an error, so it must not
    // produce an empty or input-ordered result.
    Weights zero;
    zero.importance = 0.0;
    zero.urgency = 0.0;
    zero.age = 0.0;
    zero.blocks = 0.0;

    // Deliberately scrambled: input order, id order and created_at order all
    // disagree, so only the documented tie-breaker can produce the expected
    // answer.
    std::vector<Task> tasks{
        makeTask(5, "middle", 1, kNow, kNow - 4 * kSecondsPerDay, 0),
        makeTask(1, "oldest", 5, kNow + kSecondsPerDay, kNow - 9 * kSecondsPerDay, 3),
        makeTask(9, "newest", 3, std::nullopt, kNow - 1 * kSecondsPerDay, 1),
    };

    const std::vector<RankedTask> ranked = PriorityEngine::rank(tasks, kNow, zero);
    ASSERT_EQ(std::size_t{ 3 }, ranked.size());
    for (const RankedTask &entry : ranked)
    {
        EXPECT_DOUBLE_EQ(0.0, entry.score.total);
    }

    const std::vector<std::string> expected{ "oldest", "middle", "newest" };
    EXPECT_EQ(expected, titlesOf(ranked));
}

TEST(PriorityEngineRank, OrderIsIndependentOfInputOrderAndOfRepeatedCalls)
{
    // The queue must be a function of the task SET, not of the order the
    // caller happened to hand the tasks over in, nor of the sort's internals.
    // Six tasks, including an exact score+created_at tie, exercise all three
    // keys.
    const std::vector<Task> tasks{
        makeTask(3, "t3", 4, kNow + 2 * kSecondsPerDay, kNow - 2 * kSecondsPerDay, 1),
        makeTask(1, "t1", 4, kNow + 2 * kSecondsPerDay, kNow - 2 * kSecondsPerDay, 1),
        makeTask(2, "t2", 5, std::nullopt, kNow - 30 * kSecondsPerDay, 0),
        makeTask(6, "t6", 1, kNow + 6 * kSecondsPerDay, kNow - 1 * kSecondsPerDay, 0),
        makeTask(4, "t4", 2, kNow + 10 * kSecondsPerDay, kNow, 4),
        makeTask(5, "t5", 3, kNow - kSecondsPerDay, kNow - 7 * kSecondsPerDay, 0),
    };

    const std::vector<std::string> reference = titlesOf(PriorityEngine::rank(tasks, kNow, Weights{}));

    // Same input, twice.
    EXPECT_EQ(reference, titlesOf(PriorityEngine::rank(tasks, kNow, Weights{})));

    // Reversed input.
    const std::vector<Task> reversed(tasks.rbegin(), tasks.rend());
    EXPECT_EQ(reference, titlesOf(PriorityEngine::rank(reversed, kNow, Weights{})));

    // Shuffled input. The seed is fixed so a failure is reproducible; the
    // point is that a different arrangement cannot change the answer.
    std::mt19937 generator{ 20261008U };
    std::vector<Task> shuffled = tasks;
    std::shuffle(shuffled.begin(), shuffled.end(), generator);
    EXPECT_EQ(reference, titlesOf(PriorityEngine::rank(shuffled, kNow, Weights{})));

    // The tie between t1 and t3 is resolved by id (1 before 3) rather than by
    // whichever the input or the sort happened to place first.
    const auto t1 = std::find(reference.begin(), reference.end(), "t1");
    const auto t3 = std::find(reference.begin(), reference.end(), "t3");
    ASSERT_NE(reference.end(), t1);
    ASSERT_NE(reference.end(), t3);
    EXPECT_LT(t1, t3);
}

// ---------------------------------------------------------------------------
// Filtering, sortByScore, and the JSON shape
// ---------------------------------------------------------------------------

TEST(PriorityEngineRank, DropsDoneAndArchivedKeepsOpenAndInProgress)
{
    std::vector<Task> tasks{
        makeTask(1, "open", 3, std::nullopt, kNow, 0),
        makeTask(2, "in-progress", 3, std::nullopt, kNow, 0),
        makeTask(3, "done", 5, kNow, kNow - 100 * kSecondsPerDay, 9),
        makeTask(4, "archived", 5, kNow, kNow - 100 * kSecondsPerDay, 9),
    };
    tasks[1].status = TaskStatus::kInProgress;
    tasks[2].status = TaskStatus::kDone;
    tasks[3].status = TaskStatus::kArchived;

    // The two excluded tasks would top the queue on score alone (they are
    // deliberately the highest-scoring ones), which is exactly why the filter
    // must key off status and not off ranking.
    const std::vector<RankedTask> ranked = PriorityEngine::rank(tasks, kNow, Weights{});
    ASSERT_EQ(std::size_t{ 2 }, ranked.size());

    const std::vector<std::string> expected{ "open", "in-progress" };
    EXPECT_EQ(expected, titlesOf(ranked));
    for (const RankedTask &entry : ranked)
    {
        EXPECT_TRUE(entry.task.isActionable());
    }
}

TEST(PriorityEngineSort, SortByScoreMatchesRankAndKeepsEverything)
{
    // sortByScore is the in-place variant with the same total order. Unlike
    // rank() it does NOT filter, so a caller reordering a mixed view keeps
    // its done rows.
    std::vector<Task> tasks = workedExampleTasks();
    Task finished = makeTask(99, "finished", 5, kNow, kNow - 100 * kSecondsPerDay, 0);
    finished.status = TaskStatus::kDone;
    tasks.push_back(finished);

    const std::vector<std::string> ranked_titles =
        titlesOf(PriorityEngine::rank(tasks, kNow, Weights{}));

    PriorityEngine::sortByScore(tasks, kNow, Weights{});
    ASSERT_EQ(std::size_t{ 5 }, tasks.size());

    std::size_t finished_count = 0;
    std::vector<std::string> actionable_titles;
    for (const Task &task : tasks)
    {
        if ("finished" == task.title)
        {
            ++finished_count;
        }
        if (task.isActionable())
        {
            actionable_titles.push_back(task.title);
        }
    }
    EXPECT_EQ(std::size_t{ 1 }, finished_count);
    EXPECT_EQ(ranked_titles, actionable_titles);
}

TEST(PriorityEngineRank, RankedJsonFlattensTheTaskAndAddsScoreEvidence)
{
    const std::vector<RankedTask> ranked = PriorityEngine::rank(workedExampleTasks(), kNow, Weights{});
    const RankedTask *a = findByTitle(ranked, "A");
    ASSERT_NE(nullptr, a);

    const nlohmann::json flat = toJson(a->task);
    const nlohmann::json json = toJson(*a);

    // "Flattened" means every field Task::toJson emits appears at the top
    // level of the ranked form with the same value. Checked against
    // Task::toJson itself rather than against a hard-coded key list, so the
    // two serializers cannot drift apart as Task gains fields.
    for (const auto &[key, value] : flat.items())
    {
        EXPECT_TRUE(json.contains(key)) << "ranked JSON is missing task field: " << key;
        EXPECT_EQ(value, json.at(key));
    }

    EXPECT_TRUE(json.contains("score"));
    EXPECT_TRUE(json.contains("score_parts"));
    EXPECT_TRUE(json.contains("urgency_factor"));
    EXPECT_TRUE(json.contains("age_days"));

    // score is the TOTAL, and score_parts carries the four contributions.
    EXPECT_NEAR(a->score.total, json.at("score").get<double>(), kTolerance);
    EXPECT_NEAR(a->score.importance, json.at("score_parts").at("importance").get<double>(), kTolerance);
    EXPECT_NEAR(a->score.urgency, json.at("score_parts").at("urgency").get<double>(), kTolerance);
    EXPECT_NEAR(a->score.age, json.at("score_parts").at("age").get<double>(), kTolerance);
    EXPECT_NEAR(a->score.blocks, json.at("score_parts").at("blocks").get<double>(), kTolerance);

    // The raw inputs are the pre-weight values: A is due exactly now (factor
    // 1.0) and has waited three days.
    EXPECT_NEAR(a->urgency_factor, json.at("urgency_factor").get<double>(), kTolerance);
    EXPECT_NEAR(a->age_days, json.at("age_days").get<double>(), kTolerance);
    EXPECT_NEAR(1.0, json.at("urgency_factor").get<double>(), kTolerance);
    EXPECT_NEAR(3.0, json.at("age_days").get<double>(), kTolerance);
}

} // namespace
} // namespace taskpilot
