// test_priority_engine.cpp — asserts docs/scoring.md, not the implementation.
//
// Every expectation in this file is derived from the specification document:
// the four cases of urgencyFactor, the clamped age, the worked example table,
// the aging flip, the tie-breakers, and the "pure FIFO" degenerate case. A
// refactor of PriorityEngine.cpp that keeps these tests green keeps the queue
// honest; a change that breaks them is a change to the ranking RULES, which
// is a product decision rather than a refactor.
//
// The FINAL tie-break is uid, not the local id, and that is a portability
// rule rather than a style one: the id differs by construction between two
// machines holding the same backlog (a merge assigns ids in uid order), while
// docs/sync.md promises the queue order IS comparable across machines. The
// tie-break tests below therefore arrange uids to disagree with ids and with
// the input order, so a comparator still keying on anything but the uid fails
// them.
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

/// A v4-shaped uid derived from a small non-negative number, so every task a
/// test builds carries a DISTINCT uid without each call site spelling out
/// thirty-six characters.
///
/// The derived order tracks the number (the leading group is the number in
/// zero-padded lowercase hex, and equal-width hex strings compare in numeric
/// order). That coincidence is a convenience for the tests whose subject is
/// NOT the tie-break, because it keeps their expected order readable; it is
/// never relied on where the tie-break itself is under test. Those tests pass
/// uids through makeTaskWithUid() and arrange them to DISAGREE with the ids
/// and the input order, so nothing but a uid-keyed comparator can pass them.
///
/// Precondition: 0 <= number < 2^32 (the ids in this file are at most two
/// digits). Wider values would lose their high bits and two of them could
/// collide, which is exactly the "several tasks with the same uid" state these
/// tests must avoid.
std::string uidForNumber(std::int64_t number)
{
    constexpr char kHexDigits[] = "0123456789abcdef";
    std::string leading(8, '0');
    std::uint64_t remaining = static_cast<std::uint64_t>(number);
    for (std::size_t index = leading.size(); 0 != index && 0 != remaining; --index)
    {
        leading[index - 1] = kHexDigits[static_cast<std::size_t>(remaining & 0xF)];
        remaining >>= 4;
    }
    return leading + "-0000-4000-8000-000000000000";
}

/// Five v4-shaped uids listed in ASCENDING order. They differ only in their
/// first character ('1', '3', '5', '7', '9' — all legal lowercase hex), so
/// every comparison is decided at the first byte and the expected order of a
/// tie-break test is readable straight off the constants' names. Ranking never
/// validates the uuid SHAPE, but keeping these shape-legal costs nothing and
/// means they would survive a trip through the export parser too.
constexpr const char *kUidA = "1f3c1a2b-4d5e-4f60-8a9b-0c1d2e3f4a5b";
constexpr const char *kUidB = "3f3c1a2b-4d5e-4f60-8a9b-0c1d2e3f4a5b";
constexpr const char *kUidC = "5f3c1a2b-4d5e-4f60-8a9b-0c1d2e3f4a5b";
constexpr const char *kUidD = "7f3c1a2b-4d5e-4f60-8a9b-0c1d2e3f4a5b";
constexpr const char *kUidE = "9f3c1a2b-4d5e-4f60-8a9b-0c1d2e3f4a5b";

/// Build an open task, exposing only the fields ranking reads so no test can
/// accidentally depend on the rest. The uid is derived from the id so that no
/// task ever reaches the comparator with an EMPTY uid: several empty uids in a
/// full tie would compare equivalent, which is legal for a strict weak
/// ordering but would let a tie-break assertion pass by accident instead of by
/// rule — the one thing these tests exist to prevent.
Task makeTask(std::int64_t id, std::string title, int importance,
              std::optional<std::int64_t> due_at, std::int64_t created_at, int blocks)
{
    Task task;
    task.uid = uidForNumber(id);
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

/// Same as makeTask but with a caller-chosen uid. A tie-break test has to be
/// able to place a uid deliberately — including in the REVERSE of the id order
/// — so the uid must not be a function of the id there.
Task makeTaskWithUid(std::string uid, std::int64_t id, std::string title, int importance,
                     std::optional<std::int64_t> due_at, std::int64_t created_at, int blocks)
{
    Task task = makeTask(id, std::move(title), importance, due_at, created_at, blocks);
    task.uid = std::move(uid);
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

TEST(PriorityEngineRank, TiesBreakOnOlderCreatedAtThenLowerUid)
{
    // age is switched off so created_at does NOT feed the score. That is what
    // makes the three totals genuinely equal: with aging on, the older task
    // would simply score higher and the tie-breakers would never run at all.
    Weights no_age;
    no_age.age = 0.0;

    // The two created_at twins (twin, latecomer) carry uids in the REVERSE of
    // their id order: the LARGER id gets the SMALLER uid. An implementation
    // still falling back to the local id would put "latecomer" first; the
    // comparator under test must instead follow the uid and put "twin" first.
    std::vector<Task> tasks{
        makeTaskWithUid(kUidE, 4, "latecomer", 3, std::nullopt, kNow - 2 * kSecondsPerDay, 0),
        makeTaskWithUid(kUidC, 2, "older", 3, std::nullopt, kNow - 5 * kSecondsPerDay, 0),
        makeTaskWithUid(kUidA, 7, "twin", 3, std::nullopt, kNow - 2 * kSecondsPerDay, 0),
    };

    const std::vector<RankedTask> ranked = PriorityEngine::rank(tasks, kNow, no_age);
    ASSERT_EQ(std::size_t{ 3 }, ranked.size());
    for (const RankedTask &entry : ranked)
    {
        EXPECT_DOUBLE_EQ(0.0, entry.score.age);
        EXPECT_DOUBLE_EQ(ranked.front().score.total, entry.score.total);
    }

    // "older" wins key 2. The two remaining tasks are a FULL tie (equal score
    // and equal created_at), so they fall through to key 3, where the smaller
    // uid wins: A (twin, id 7) before E (latecomer, id 4). The ids point the
    // other way and the input lists the higher uid first, so both an id-keyed
    // and an input-order-keyed answer are excluded by the same assertion.
    const std::vector<std::string> expected{ "older", "twin", "latecomer" };
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
    //
    // t1 and t3 tie on score and created_at, and their uids are arranged to
    // OPPOSE their ids: the smaller id carries the larger uid. The queue must
    // follow the uid — see the assertion at the end of the test.
    const std::vector<Task> tasks{
        makeTaskWithUid(kUidA, 3, "t3", 4, kNow + 2 * kSecondsPerDay, kNow - 2 * kSecondsPerDay, 1),
        makeTaskWithUid(kUidE, 1, "t1", 4, kNow + 2 * kSecondsPerDay, kNow - 2 * kSecondsPerDay, 1),
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

    // The tie between t1 and t3 is resolved by uid (A before E) — NOT by the
    // input order, and NOT by the id either: t1 holds the smaller id but the
    // larger uid, so an id-keyed comparator would put t1 first. t3 comes
    // first, and the same answer survives the two rearrangements above.
    const auto t1 = std::find(reference.begin(), reference.end(), "t1");
    const auto t3 = std::find(reference.begin(), reference.end(), "t3");
    ASSERT_NE(reference.end(), t1);
    ASSERT_NE(reference.end(), t3);
    EXPECT_LT(t3, t1);
}

// ---------------------------------------------------------------------------
// The final tie-break is the uid — the only key two machines can agree on
//
// The local id is a row number, not an identity (Task.hpp): two machines both
// have a row 9, a merge assigns ids in uid order — random with respect to
// creation order — and the sync format does not transport ids at all. A tie
// that fell through to the id would therefore order the SAME backlog
// differently on two machines, while docs/sync.md promises the queue order IS
// comparable. These three tests hold the comparator to the uid.
// ---------------------------------------------------------------------------

TEST(PriorityEngineRank, FullTieOrdersByUidAndSwappingTheUidsSwapsTheOrder)
{
    // alpha and beta tie on everything the first two keys can see: equal score
    // (15 = importance 3 x 5, no deadline, no blocks, created at `now` so age
    // is zero) and equal created_at. Only the uid can separate them.
    //
    // The ids are deliberately read the WRONG WAY ROUND — alpha carries the
    // larger id — so this first arrangement is also one that a comparator
    // still falling back to the id gets backwards.
    const std::vector<Task> alpha_holds_the_lower_uid{
        makeTaskWithUid(kUidA, 9, "alpha", 3, std::nullopt, kNow, 0),
        makeTaskWithUid(kUidE, 1, "beta", 3, std::nullopt, kNow, 0),
    };

    const std::vector<RankedTask> ranked =
        PriorityEngine::rank(alpha_holds_the_lower_uid, kNow, Weights{});
    ASSERT_EQ(std::size_t{ 2 }, ranked.size());

    // The tie is asserted, not assumed: if the scoring ever changed, this test
    // must fail loudly instead of quietly checking a weaker property.
    EXPECT_DOUBLE_EQ(ranked[0].score.total, ranked[1].score.total);
    EXPECT_EQ(ranked[0].task.created_at, ranked[1].task.created_at);

    const std::vector<std::string> alpha_first{ "alpha", "beta" };
    EXPECT_EQ(alpha_first, titlesOf(ranked));

    // Now exchange ONLY the uids, leaving ids, created_at, scores and the input
    // order untouched. The expected order swaps — the single fact that proves
    // the tie-break reads the uid.
    const std::vector<Task> beta_holds_the_lower_uid{
        makeTaskWithUid(kUidE, 9, "alpha", 3, std::nullopt, kNow, 0),
        makeTaskWithUid(kUidA, 1, "beta", 3, std::nullopt, kNow, 0),
    };
    const std::vector<std::string> beta_first{ "beta", "alpha" };
    EXPECT_EQ(beta_first,
              titlesOf(PriorityEngine::rank(beta_holds_the_lower_uid, kNow, Weights{})));
}

TEST(PriorityEngineRank, FullTieOrderIsUnchangedWhenTheLocalIdsAreSwapped)
{
    // The same two tasks and the same uid arrangement twice; the ONLY
    // difference between the two inputs is which of them holds which local id.
    // If the comparator consulted the id at all, the two answers could not
    // both match the uid order — and on the old comparator (lower id first)
    // the first input answers "first" instead of "second" and fails here.
    const std::vector<Task> ids_as_given{
        makeTaskWithUid(kUidE, 1, "first", 3, std::nullopt, kNow, 0),
        makeTaskWithUid(kUidA, 2, "second", 3, std::nullopt, kNow, 0),
    };
    const std::vector<Task> ids_swapped{
        makeTaskWithUid(kUidE, 2, "first", 3, std::nullopt, kNow, 0),
        makeTaskWithUid(kUidA, 1, "second", 3, std::nullopt, kNow, 0),
    };

    // "second" holds uid A and "first" holds uid E, so second comes first —
    // whichever id each of them carries.
    const std::vector<std::string> expected{ "second", "first" };
    EXPECT_EQ(expected, titlesOf(PriorityEngine::rank(ids_as_given, kNow, Weights{})));
    EXPECT_EQ(expected, titlesOf(PriorityEngine::rank(ids_swapped, kNow, Weights{})));
}

TEST(PriorityEngineRank, FullTieGroupOrdersByUidAndIgnoresInputOrder)
{
    // Five tasks in a FULL tie on keys 1 and 2: same score (15), same
    // created_at, no deadline. Only the uid separates them, so the queue must
    // come out in exactly ascending uid order: A, B, C, D, E.
    //
    // The ids are scrambled to oppose BOTH the uid order and the input order,
    // so an implementation that reached for the id (or left the order to the
    // sort's internals) cannot produce this sequence by accident.
    const auto fullTieGroup = []()
    {
        return std::vector<Task>{
            makeTaskWithUid(kUidC, 30, "C", 3, std::nullopt, kNow, 0),
            makeTaskWithUid(kUidA, 50, "A", 3, std::nullopt, kNow, 0),
            makeTaskWithUid(kUidE, 10, "E", 3, std::nullopt, kNow, 0),
            makeTaskWithUid(kUidB, 40, "B", 3, std::nullopt, kNow, 0),
            makeTaskWithUid(kUidD, 20, "D", 3, std::nullopt, kNow, 0),
        };
    };

    const std::vector<std::string> expected{ "A", "B", "C", "D", "E" };
    const std::vector<Task> tasks = fullTieGroup();
    const std::vector<RankedTask> ranked = PriorityEngine::rank(tasks, kNow, Weights{});
    ASSERT_EQ(std::size_t{ 5 }, ranked.size());
    for (const RankedTask &entry : ranked)
    {
        EXPECT_DOUBLE_EQ(15.0, entry.score.total);
    }
    EXPECT_EQ(expected, titlesOf(ranked));

    // A shuffled input (fixed seed, so a failure is reproducible) must give
    // the identical answer, and so must the reversed one: the output is a
    // function of the uids, never of the arrangement handed in.
    std::mt19937 generator{ 20261008U };
    std::vector<Task> shuffled = tasks;
    std::shuffle(shuffled.begin(), shuffled.end(), generator);
    EXPECT_EQ(expected, titlesOf(PriorityEngine::rank(shuffled, kNow, Weights{})));

    const std::vector<Task> reversed(tasks.rbegin(), tasks.rend());
    EXPECT_EQ(expected, titlesOf(PriorityEngine::rank(reversed, kNow, Weights{})));
}

// ---------------------------------------------------------------------------
// Filtering, sortByScore, and the JSON shape
// ---------------------------------------------------------------------------

TEST(PriorityEngineRank, DropsDoneAndArchivedKeepsOpenAndInProgress)
{
    std::vector<Task> tasks{
        makeTaskWithUid(kUidA, 1, "open", 3, std::nullopt, kNow, 0),
        makeTaskWithUid(kUidB, 2, "in-progress", 3, std::nullopt, kNow, 0),
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

    // The two survivors are themselves a FULL tie — both score 15 with the
    // same created_at — so their order comes from the uid key: A before B,
    // which the uids assigned above make true by construction.
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
