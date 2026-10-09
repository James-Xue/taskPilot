// test_services.cpp — the API surface: catalog, dispatch, validation, ordering
//
// Everything here runs against a real in-memory TaskStore and a FixedClock, so
// the assertions are about behaviour rather than about a mock's call log. No
// sockets are involved; the TCP and MCP transports are tested separately.
//
// Two properties get more attention than the rest because they are the ones
// that break silently in production:
//   1. The catalog and the dispatch table agree. A method advertised in
//      tools/list that no dispatch row serves is a tool that always fails, and
//      nothing at runtime notices — so the cross-check below invokes every
//      advertised name and forbids only the "unknown method" answer.
//   2. "now" is sampled once per call. Every score assertion therefore runs
//      against a clock the test controls, and one test counts the samples.
//   3. An undeclared parameter is refused, for every method. A key the method's
//      schema does not declare used to be ignored, so a mistyped filter
//      answered a question the caller had not asked; the catalog is iterated in
//      the tests below so a method added later is covered without a new test,
//      and both halves — the refusal and the happy path — are asserted, because
//      a check that refused everything would pass a refusal-only test.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <latch>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "control/Services.hpp"
#include "core/Clock.hpp"
#include "core/PriorityEngine.hpp"
#include "core/Task.hpp"
#include "core/TaskStore.hpp"
#include "core/TaskSync.hpp"
#include "core/Uuid.hpp"
#include "core/Weights.hpp"

namespace taskpilot
{
namespace
{

constexpr const char *kVersion{ "test-version" };

/// The wall clock, used as the default instant of a fixture.
///
/// A hard-coded date would be worse here, not better: TaskStore stamps
/// created_at from the system clock (it takes no Clock), so a fixture frozen
/// in the past would give every task an age of hundreds of days. Tests that
/// need an exact age move the clock by an exact offset from the created_at the
/// store actually wrote.
[[nodiscard]] std::int64_t wallNow()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

/// Open a throwaway database, or fail the test loudly.
///
/// A fixture cannot use ASSERT_* (it is not a void function), so this reports
/// with ADD_FAILURE and then throws — GTest turns an exception escaping a
/// fixture into a failure of the test that needed it, which is the honest
/// outcome: an unopenable store makes every assertion below meaningless.
[[nodiscard]] std::unique_ptr<TaskStore> openStore()
{
    Result<std::unique_ptr<TaskStore>> opened = TaskStore::open(":memory:");
    if (!opened.ok())
    {
        ADD_FAILURE() << "cannot open the in-memory store: " << opened.error().message;
        throw std::runtime_error("in-memory store unavailable: " + opened.error().message);
    }
    return std::move(opened).value();
}

/// The dependencies Services needs, held together so that the store outlives
/// the reference Services keeps to it. Members are declared in the order the
/// constructor must initialize them.
struct Harness
{
    explicit Harness(std::int64_t now = wallNow())
        : store{ openStore() }, clock{ now }, services{ *store, clock, kVersion }
    {
    }

    std::unique_ptr<TaskStore> store;
    FixedClock clock;
    Services services;
};

/// Call a method that is expected to succeed.
///
/// Not [[nodiscard]]: a test is allowed to call this purely for its effect
/// (setup), and the call already reports its own failure through ADD_FAILURE.
nlohmann::json callOk(Services &services, const std::string &method,
                      const nlohmann::json &params = nlohmann::json::object())
{
    RpcResult result = services.invoke(method, params);
    if (!result.ok())
    {
        ADD_FAILURE() << method << " failed: " << result.error().message;
        return nlohmann::json{};
    }
    return result.value();
}

/// Call a method that is expected to fail, returning its error.
[[nodiscard]] Error callError(Services &services, const std::string &method,
                              const nlohmann::json &params)
{
    RpcResult result = services.invoke(method, params);
    if (result.ok())
    {
        ADD_FAILURE() << method << " unexpectedly succeeded";
        return Error::internal("call unexpectedly succeeded");
    }
    return result.error();
}

/// add_task with the title spliced in, so a test reads as the fields it cares
/// about. See callOk for why this is not [[nodiscard]].
nlohmann::json addTask(Services &services, const std::string &title,
                       const nlohmann::json &fields = nlohmann::json::object())
{
    nlohmann::json params = fields;
    params["title"] = title;
    return callOk(services, "add_task", params);
}

/// A clock that counts how often it was asked for the time, so the
/// single-sample contract can be asserted instead of assumed.
///
/// Not thread-safe on purpose — this is a test double, and the counter is only
/// ever touched from the test's own thread.
class CountingClock final : public Clock
{
  public:
    explicit CountingClock(std::int64_t now) : m_now{ now } {}

    [[nodiscard]] std::int64_t nowEpochSeconds() const override
    {
        ++m_calls;
        return m_now;
    }

    [[nodiscard]] int calls() const { return m_calls; }
    void reset() const { m_calls = 0; }

  private:
    std::int64_t m_now;
    mutable int m_calls{ 0 };
};

class ServicesTest : public ::testing::Test
{
  protected:
    Harness harness;
};

// --- introspection ------------------------------------------------------

TEST_F(ServicesTest, GetStatusReportsIdentityAndBacklog)
{
    addTask(harness.services, "write the tests");

    const nlohmann::json status = callOk(harness.services, "get_status");

    EXPECT_EQ(status.at("version").get<std::string>(), kVersion);
    EXPECT_EQ(status.at("db_path").get<std::string>(), ":memory:");
    EXPECT_EQ(status.at("method_count").get<int>(), 15);
    EXPECT_GE(status.at("uptime_seconds").get<std::int64_t>(), 0);

    // The store's tallies are merged at the top level of the reply, so the
    // key names here are the store's own stats keys.
    ASSERT_TRUE(status.contains("open"));
    EXPECT_EQ(status.at("open").get<std::size_t>(), 1U);
}

TEST_F(ServicesTest, UptimeIsMeasuredFromTheInjectedClock)
{
    harness.clock.setNow(harness.services.startTime() + 90);

    const nlohmann::json status = callOk(harness.services, "get_status");

    EXPECT_EQ(status.at("uptime_seconds").get<std::int64_t>(), 90);
}

TEST_F(ServicesTest, DescribeMethodsReturnsTheWholeCatalog)
{
    const nlohmann::json catalog = callOk(harness.services, "describe_methods");

    ASSERT_TRUE(catalog.is_array());
    ASSERT_EQ(catalog.size(), 15U);

    std::vector<std::string> names;
    for (const nlohmann::json &entry : catalog)
    {
        ASSERT_TRUE(entry.is_object());
        ASSERT_TRUE(entry.contains("name"));
        ASSERT_TRUE(entry.contains("description"));
        ASSERT_TRUE(entry.contains("params_schema"));

        // Descriptions are read by an LLM choosing a tool, so an empty one is
        // a defect rather than a style issue.
        EXPECT_FALSE(entry.at("description").get<std::string>().empty());

        const nlohmann::json &schema = entry.at("params_schema");
        ASSERT_TRUE(schema.is_object());
        EXPECT_EQ(schema.at("type").get<std::string>(), "object");
        ASSERT_TRUE(schema.contains("properties"));
        const nlohmann::json &properties = schema.at("properties");
        ASSERT_TRUE(properties.is_object());

        // Required-ness must live in the schema-level array. A per-property
        // "required": true is not JSON Schema, and host-side validators reject
        // the whole tool definition when they meet it.
        for (auto property = properties.begin(); property != properties.end(); ++property)
        {
            EXPECT_FALSE(property->contains("required"))
                << "property '" << property.key() << "' of " << entry.at("name").get<std::string>()
                << " carries an invalid per-property required";
        }

        if (schema.contains("required"))
        {
            const nlohmann::json &required = schema.at("required");
            ASSERT_TRUE(required.is_array());
            for (const nlohmann::json &name : required)
            {
                ASSERT_TRUE(name.is_string());
                EXPECT_TRUE(properties.contains(name.get<std::string>()))
                    << entry.at("name").get<std::string>() << " requires an undeclared property "
                    << name.get<std::string>();
            }
        }

        names.push_back(entry.at("name").get<std::string>());
    }

    std::vector<std::string> sorted = names;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(std::unique(sorted.begin(), sorted.end()), sorted.end())
        << "method names must be unique: the wire vocabulary is the tool name";
}

TEST_F(ServicesTest, EveryAdvertisedMethodIsDispatchedAndEveryMethodIsAdvertised)
{
    // The catalog is the single source of truth, so the two transports cannot
    // drift. This is the test that catches a spec with no dispatch row (a tool
    // that always fails) and a dispatch row with no spec (an operation no
    // client can discover).
    for (const MethodSpec &spec : Services::methodSpecs())
    {
        const RpcResult result = harness.services.invoke(spec.name, nlohmann::json::object());
        if (!result.ok())
        {
            // A missing required parameter is a legitimate answer for a method
            // called with nothing; "unknown method" is not.
            EXPECT_EQ(std::string::npos, result.error().message.find("unknown method"))
                << spec.name << " is advertised but not dispatched";
        }
    }

    const MethodRegistry registry = harness.services.registry();
    EXPECT_EQ(registry.size(), Services::methodSpecs().size());
    for (const MethodSpec &spec : Services::methodSpecs())
    {
        EXPECT_TRUE(registry.contains(spec.name)) << spec.name << " is missing from the registry";
    }
    EXPECT_FALSE(registry.contains("no_such_method"));
}

TEST_F(ServicesTest, FindSpecResolvesNamesAndRejectsUnknownOnes)
{
    const MethodSpec *found = Services::findSpec("add_task");
    ASSERT_NE(nullptr, found);
    EXPECT_EQ(found->name, "add_task");
    EXPECT_EQ(nullptr, Services::findSpec("add_tasks"));
}

// --- task mutations and the queue ---------------------------------------

TEST_F(ServicesTest, AddTaskReturnsTheStoredTask)
{
    const nlohmann::json created =
        addTask(harness.services, "fix the reconnection bug",
                { { "importance", 5 }, { "blocks", 2 }, { "notes", "keep it short" },
                  { "tags", nlohmann::json::array({ "exchange", "urgent" }) } });

    EXPECT_GT(created.at("id").get<std::int64_t>(), 0);
    EXPECT_EQ(created.at("title").get<std::string>(), "fix the reconnection bug");
    EXPECT_EQ(created.at("status").get<std::string>(), "open");
    EXPECT_EQ(created.at("importance").get<int>(), 5);
    EXPECT_EQ(created.at("blocks").get<int>(), 2);
    EXPECT_EQ(created.at("notes").get<std::string>(), "keep it short");
    ASSERT_EQ(created.at("tags").size(), 2U);
    EXPECT_EQ(created.at("tags").at(0).get<std::string>(), "exchange");
    EXPECT_GT(created.at("created_at").get<std::int64_t>(), 0);
    EXPECT_TRUE(created.at("due_at").is_null());
    EXPECT_TRUE(created.at("completed_at").is_null());
}

TEST_F(ServicesTest, TheQueueIsAbleToExplainItsOwnOrdering)
{
    addTask(harness.services, "only one task");

    const nlohmann::json queue = callOk(harness.services, "get_queue");

    ASSERT_TRUE(queue.at("queue").is_array());
    ASSERT_EQ(queue.at("queue").size(), 1U);
    EXPECT_EQ(queue.at("now").get<std::int64_t>(), harness.clock.nowEpochSeconds());
    ASSERT_TRUE(queue.contains("weights"));
    EXPECT_TRUE(queue.at("weights").is_object());

    const nlohmann::json &entry = queue.at("queue").front();
    // A queue entry carries the task's own fields flattened plus the evidence
    // for its position, which is what lets a caller explain the order instead
    // of having to trust it.
    EXPECT_EQ(entry.at("title").get<std::string>(), "only one task");
    EXPECT_TRUE(entry.contains("score"));
    ASSERT_TRUE(entry.contains("score_parts"));
    EXPECT_TRUE(entry.at("score_parts").is_object());
}

TEST_F(ServicesTest, AThreeDayOldTaskDueTodayScoresTheDocumentedValue)
{
    const nlohmann::json created = addTask(harness.services, "fix the WS reconnection bug",
                                           { { "importance", 5 }, { "blocks", 2 } });
    const std::int64_t createdAt = created.at("created_at").get<std::int64_t>();

    // created_at is stamped by the store's own clock, so "three days old" is
    // produced by moving the injected clock exactly three days past the
    // instant the store actually wrote — which keeps the age an exact whole
    // number of days rather than an approximation that happens to be close.
    harness.clock.setNow(createdAt + 3 * kSecondsPerDay);
    callOk(harness.services, "update_task",
           { { "id", created.at("id") }, { "due_at", harness.clock.nowEpochSeconds() } });

    const nlohmann::json queue = callOk(harness.services, "get_queue");
    ASSERT_EQ(queue.at("queue").size(), 1U);

    // importance 5 x 5 = 25, urgency 30 x 1.0 = 30 (due now saturates the
    // ramp), age 2 x 3 = 6, blocks 8 x 2 = 16. Every term is exactly
    // representable in binary, so the total is exact rather than merely near.
    EXPECT_DOUBLE_EQ(queue.at("queue").front().at("score").get<double>(), 77.0);
}

TEST_F(ServicesTest, TheQueueRanksByScoreUsingTheDocumentedTerms)
{
    const std::int64_t now = harness.clock.nowEpochSeconds();

    // The ages are equal here (the store stamps them milliseconds apart), so
    // this isolates the three terms the caller controls: importance, urgency
    // and blocks. docs/scoring.md's worked example also differs ages, which is
    // impossible through the API — a task cannot be created with a backdated
    // created_at — so the age term is proven separately by advancing the clock.
    addTask(harness.services, "A", { { "importance", 5 }, { "blocks", 2 }, { "due_at", now } });
    addTask(harness.services, "B",
            { { "importance", 4 }, { "due_at", now + kSecondsPerDay } });
    addTask(harness.services, "D",
            { { "importance", 2 }, { "due_at", now + 2 * kSecondsPerDay } });
    addTask(harness.services, "C",
            { { "importance", 3 }, { "due_at", now + 5 * kSecondsPerDay } });

    const nlohmann::json queue = callOk(harness.services, "get_queue");
    ASSERT_EQ(queue.at("queue").size(), 4U);

    std::vector<std::string> titles;
    for (const nlohmann::json &entry : queue.at("queue"))
    {
        titles.push_back(entry.at("title").get<std::string>());
    }
    EXPECT_EQ(titles, (std::vector<std::string>{ "A", "B", "D", "C" }));

    // 71.0 = 25 + 30 + 16; the others only ever earn importance and urgency.
    // The tolerance is because the four tasks' ages differ by a few
    // milliseconds, worth ~1e-8 points.
    EXPECT_NEAR(queue.at("queue").at(0).at("score").get<double>(), 71.0, 1e-6);
    EXPECT_NEAR(queue.at("queue").at(1).at("score").get<double>(),
                20.0 + 30.0 * (1.0 - 1.0 / 7.0), 1e-6);
    EXPECT_NEAR(queue.at("queue").at(2).at("score").get<double>(),
                10.0 + 30.0 * (1.0 - 2.0 / 7.0), 1e-6);
    EXPECT_NEAR(queue.at("queue").at(3).at("score").get<double>(),
                15.0 + 30.0 * (1.0 - 5.0 / 7.0), 1e-6);
}

TEST_F(ServicesTest, TheQueueLimitTrimsTheRankedListNotTheInput)
{
    const std::int64_t now = harness.clock.nowEpochSeconds();
    addTask(harness.services, "low", { { "importance", 1 } });
    addTask(harness.services, "high", { { "importance", 5 }, { "due_at", now } });
    addTask(harness.services, "middle", { { "importance", 3 } });

    const nlohmann::json queue = callOk(harness.services, "get_queue", { { "limit", 2 } });

    ASSERT_EQ(queue.at("queue").size(), 2U);
    EXPECT_EQ(queue.at("queue").at(0).at("title").get<std::string>(), "high");
    EXPECT_EQ(queue.at("queue").at(1).at("title").get<std::string>(), "middle");
}

TEST_F(ServicesTest, TheQueueCanExcludeWorkAlreadyInProgress)
{
    const nlohmann::json started =
        addTask(harness.services, "started", { { "importance", 4 } });
    addTask(harness.services, "not started", { { "importance", 4 } });
    callOk(harness.services, "update_task",
           { { "id", started.at("id") }, { "status", "in_progress" } });

    const nlohmann::json byDefault = callOk(harness.services, "get_queue");
    EXPECT_EQ(byDefault.at("queue").size(), 2U);

    const nlohmann::json openOnly =
        callOk(harness.services, "get_queue", { { "include_in_progress", false } });
    ASSERT_EQ(openOnly.at("queue").size(), 1U);
    EXPECT_EQ(openOnly.at("queue").front().at("title").get<std::string>(), "not started");
}

TEST_F(ServicesTest, TiedScoresBreakDeterministicallyByUid)
{
    // Two identical tasks: same importance, same (absent) deadline, same (zero)
    // blocks, and created within the same wall-clock second — so created_at
    // ties as well as the score, and the THIRD key is what decides.
    //
    // That key is the uid. It used to be the local row id, which is the one
    // thing it must not be: the id is a per-machine row number (docs/sync.md),
    // so ordering by it makes the queue differ between two machines holding the
    // same backlog. Hence the rename from "...ThenId".
    //
    // The uid is RANDOM, so the expectation is derived from it rather than
    // hard-coded — a fixed order here would pass only by luck. What is asserted
    // is that the pair really is ordered by ascending uid and that the order is
    // reproducible; the focused tie-break tests live in
    // test_priority_engine.cpp, which can control the uids directly.
    addTask(harness.services, "first");
    addTask(harness.services, "second");

    const nlohmann::json queue = callOk(harness.services, "get_queue");
    ASSERT_EQ(queue.at("queue").size(), 2U);

    const std::string uid0 = queue.at("queue").at(0).at("uid").get<std::string>();
    const std::string uid1 = queue.at("queue").at(1).at("uid").get<std::string>();
    const std::string title0 = queue.at("queue").at(0).at("title").get<std::string>();
    const std::string title1 = queue.at("queue").at(1).at("title").get<std::string>();

    // Derive the expected order from the documented keys instead of assuming
    // which one decides. The two creates usually land in one second, so
    // created_at ties and the uid runs — but when they straddle a second
    // boundary, created_at separates them first and the uid never gets a say.
    // Assuming the tie is what made this test fail about one run in fifty.
    const std::int64_t created0 = queue.at("queue").at(0).at("created_at").get<std::int64_t>();
    const std::int64_t created1 = queue.at("queue").at(1).at("created_at").get<std::int64_t>();
    const bool uid_decides = (created0 == created1);
    const bool ok_order = uid_decides ? (uid0 < uid1) : (created0 < created1);

    EXPECT_TRUE(ok_order)
        << (uid_decides ? "same created_at, so the pair must ascend by uid"
                        : "different created_at, so the older must come first")
        << "; got uid " << uid0 << " then " << uid1 << ", created_at " << created0
        << " then " << created1 << " (" << title0 << " / " << title1 << ")";

    // The ages differ by at most a second of wall time, so the scores are equal
    // to within that: this really is a tie, broken by the tie-breakers rather
    // than by a term.
    EXPECT_NEAR(queue.at("queue").at(0).at("score").get<double>(),
                queue.at("queue").at(1).at("score").get<double>(), 1e-3);

    // Reproducible: a second read gives the identical order. Comparing the uids
    // rather than the whole entries keeps this independent of the wall clock.
    const nlohmann::json again = callOk(harness.services, "get_queue");
    std::vector<std::string> order_first;
    std::vector<std::string> order_again;
    for (const nlohmann::json &entry : queue.at("queue"))
    {
        order_first.push_back(entry.at("uid").get<std::string>());
    }
    for (const nlohmann::json &entry : again.at("queue"))
    {
        order_again.push_back(entry.at("uid").get<std::string>());
    }
    EXPECT_EQ(order_first, order_again);
}

TEST_F(ServicesTest, CompletedTasksLeaveTheQueueAndReopeningRestoresThem)
{
    const nlohmann::json created = addTask(harness.services, "ship it");
    const std::int64_t id = created.at("id").get<std::int64_t>();

    const nlohmann::json completed = callOk(harness.services, "complete_task", { { "id", id } });
    EXPECT_EQ(completed.at("status").get<std::string>(), "done");
    EXPECT_FALSE(completed.at("completed_at").is_null());
    EXPECT_TRUE(callOk(harness.services, "get_queue").at("queue").empty());

    // History is kept: a completed task is gone from the queue but still
    // listed when the caller asks for it by status.
    const nlohmann::json done = callOk(harness.services, "list_tasks", { { "status", "done" } });
    ASSERT_TRUE(done.is_array());
    ASSERT_EQ(done.size(), 1U);
    EXPECT_EQ(done.front().at("title").get<std::string>(), "ship it");

    const nlohmann::json reopened = callOk(harness.services, "reopen_task", { { "id", id } });
    EXPECT_EQ(reopened.at("status").get<std::string>(), "open");
    EXPECT_TRUE(reopened.at("completed_at").is_null());
    EXPECT_EQ(callOk(harness.services, "get_queue").at("queue").size(), 1U);
}

TEST_F(ServicesTest, DeleteTaskIsIdempotentOnlyInTheSenseThatItReportsTheSecondAttempt)
{
    const nlohmann::json created = addTask(harness.services, "created by mistake");
    const std::int64_t id = created.at("id").get<std::int64_t>();

    const nlohmann::json deleted = callOk(harness.services, "delete_task", { { "id", id } });
    EXPECT_TRUE(deleted.at("deleted").get<bool>());
    EXPECT_EQ(deleted.at("id").get<std::int64_t>(), id);
    EXPECT_TRUE(callOk(harness.services, "get_queue").at("queue").empty());

    // A delete that silently succeeds on a missing row hides double-delete
    // bugs, so the second attempt is an error and carries the wire code that
    // marks it as a domain failure rather than a malformed request.
    const Error failure = callError(harness.services, "delete_task", { { "id", id } });
    EXPECT_EQ(ErrorCode::kNotFound, failure.code);
    EXPECT_EQ(-32001, rpcErrorCode(failure.code));
}

TEST_F(ServicesTest, UpdateTaskClearsADeadlineWithoutTouchingOtherFields)
{
    const std::int64_t deadline = harness.clock.nowEpochSeconds() + kSecondsPerDay;
    const nlohmann::json created =
        addTask(harness.services, "dated", { { "due_at", deadline }, { "importance", 4 } });
    const std::int64_t id = created.at("id").get<std::int64_t>();

    const nlohmann::json patched =
        callOk(harness.services, "update_task", { { "id", id }, { "notes", "some notes" } });
    EXPECT_EQ(patched.at("notes").get<std::string>(), "some notes");
    EXPECT_EQ(patched.at("title").get<std::string>(), "dated");
    EXPECT_EQ(patched.at("importance").get<int>(), 4);
    EXPECT_EQ(patched.at("due_at").get<std::int64_t>(), deadline);

    const nlohmann::json cleared =
        callOk(harness.services, "update_task", { { "id", id }, { "clear_due_at", true } });
    EXPECT_TRUE(cleared.at("due_at").is_null());

    // Setting a deadline and clearing it in the same call is a caller bug, and
    // it is refused rather than resolved by an arbitrary precedence rule.
    const Error conflict = callError(
        harness.services, "update_task",
        { { "id", id }, { "due_at", harness.clock.nowEpochSeconds() }, { "clear_due_at", true } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, conflict.code);
    EXPECT_TRUE(callOk(harness.services, "get_task", { { "id", id } }).at("due_at").is_null());
}

TEST_F(ServicesTest, TagsRoundTripAndCanBeFilteredOn)
{
    addTask(harness.services, "buy milk",
            { { "tags", nlohmann::json::array({ "home", "urgent" }) } });
    addTask(harness.services, "write the report",
            { { "tags", nlohmann::json::array({ "work" }) } });

    const nlohmann::json tagged = callOk(harness.services, "list_tasks", { { "tag", "urgent" } });

    ASSERT_EQ(tagged.size(), 1U);
    EXPECT_EQ(tagged.front().at("title").get<std::string>(), "buy milk");

    // The error for a bad element names its position, which is the only way to
    // make "tags must be strings" actionable for a nine-entry list.
    const Error failure =
        callError(harness.services, "add_task",
                  { { "title", "broken" }, { "tags", nlohmann::json::array({ 1, 2 }) } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, failure.code);
    EXPECT_NE(std::string::npos, failure.message.find("tags[0]"));
}

// --- queries and settings -----------------------------------------------

TEST_F(ServicesTest, ListTasksHonoursEveryOrdering)
{
    const std::int64_t now = harness.clock.nowEpochSeconds();
    addTask(harness.services, "low",
            { { "importance", 1 }, { "due_at", now + 9 * kSecondsPerDay } });
    addTask(harness.services, "high", { { "importance", 5 } });
    addTask(harness.services, "middle",
            { { "importance", 3 }, { "due_at", now + 2 * kSecondsPerDay } });

    const auto titlesOf = [](const nlohmann::json &tasks) {
        std::vector<std::string> titles;
        for (const nlohmann::json &task : tasks)
        {
            titles.push_back(task.at("title").get<std::string>());
        }
        return titles;
    };

    // score: middle 36.43, high 25, low 5 (low's deadline is beyond the
    // seven-day horizon, so it earns no urgency at all).
    EXPECT_EQ(titlesOf(callOk(harness.services, "list_tasks")),
              (std::vector<std::string>{ "middle", "high", "low" }));
    EXPECT_EQ(titlesOf(callOk(harness.services, "list_tasks", { { "order", "score" } })),
              (std::vector<std::string>{ "middle", "high", "low" }));

    // created: oldest first, which for tasks created in one test run means the
    // order they were inserted in.
    EXPECT_EQ(titlesOf(callOk(harness.services, "list_tasks", { { "order", "created" } })),
              (std::vector<std::string>{ "low", "high", "middle" }));

    // due: soonest deadline first, and the undated task last — it has no
    // position in a deadline order, and treating it as due at the epoch would
    // put it in front of real dates.
    EXPECT_EQ(titlesOf(callOk(harness.services, "list_tasks", { { "order", "due" } })),
              (std::vector<std::string>{ "middle", "low", "high" }));

    const Error failure = callError(harness.services, "list_tasks", { { "order", "random" } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, failure.code);
    EXPECT_NE(std::string::npos, failure.message.find("order"));

    const Error badStatus = callError(harness.services, "list_tasks", { { "status", "finished" } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, badStatus.code);
    EXPECT_NE(std::string::npos, badStatus.message.find("status"));

    const Error negativeLimit = callError(harness.services, "list_tasks", { { "limit", -1 } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, negativeLimit.code);

    const std::vector<std::string> top =
        titlesOf(callOk(harness.services, "list_tasks", { { "limit", 1 } }));
    EXPECT_EQ(top, (std::vector<std::string>{ "middle" }));
}

TEST_F(ServicesTest, GetTaskReportsTheScoreWithItsBreakdown)
{
    const nlohmann::json created = addTask(harness.services, "explain me",
                                           { { "importance", 4 }, { "due_at", 0 } });

    harness.clock.setNow(created.at("created_at").get<std::int64_t>() + 2 * kSecondsPerDay);

    const nlohmann::json task =
        callOk(harness.services, "get_task", { { "id", created.at("id") } });

    // importance 4 x 5 = 20, urgency saturated at 30 (due_at 0 is decades
    // past, so the ramp clamps), age 2 x 2 = 4, no blocks: 54 exactly.
    EXPECT_DOUBLE_EQ(task.at("score").get<double>(), 20.0 + 30.0 + 4.0);
    ASSERT_TRUE(task.at("score_parts").is_object());

    const Error missing = callError(harness.services, "get_task", { { "id", 9999 } });
    EXPECT_EQ(ErrorCode::kNotFound, missing.code);
    EXPECT_EQ(-32001, rpcErrorCode(missing.code));
}

TEST_F(ServicesTest, GetTaskReportsTheScoreForFinishedWorkToo)
{
    // The score is reported for EVERY status, not only the actionable ones: the
    // queue filters, the math does not. A task that is done still has an
    // importance, a deadline, an age and a block count, and a caller asking
    // about that one task by id wants the number those fields produce — not a
    // zero it cannot explain. So this asserts the exact total AND the four
    // terms behind it, which is what a regression gating the score on
    // isActionable() (0.0, all-zero parts) has to fail.
    const nlohmann::json created =
        addTask(harness.services, "already shipped", { { "importance", 5 }, { "blocks", 2 } });
    const std::int64_t id = created.at("id").get<std::int64_t>();

    // created_at is stamped by the store's own clock, so "three days old" is
    // produced by moving the injected clock exactly three days past the instant
    // the store actually wrote, and the deadline is that same instant so the
    // urgency ramp saturates at 1.0.
    harness.clock.setNow(created.at("created_at").get<std::int64_t>() + 3 * kSecondsPerDay);
    callOk(harness.services, "update_task",
           { { "id", id }, { "due_at", harness.clock.nowEpochSeconds() } });
    callOk(harness.services, "complete_task", { { "id", id } });

    const nlohmann::json done = callOk(harness.services, "get_task", { { "id", id } });
    ASSERT_EQ(done.at("status").get<std::string>(), "done");

    // importance 5 x 5 = 25, urgency 30 x 1.0 = 30 (due now saturates the
    // ramp), age 2 x 3 = 6, blocks 8 x 2 = 16. Every term is exactly
    // representable in binary, so the total is exact rather than merely near.
    EXPECT_DOUBLE_EQ(done.at("score").get<double>(), 77.0);
    ASSERT_TRUE(done.at("score_parts").is_object());
    EXPECT_DOUBLE_EQ(done.at("score_parts").at("importance").get<double>(), 25.0);
    EXPECT_DOUBLE_EQ(done.at("score_parts").at("urgency").get<double>(), 30.0);
    EXPECT_DOUBLE_EQ(done.at("score_parts").at("age").get<double>(), 6.0);
    EXPECT_DOUBLE_EQ(done.at("score_parts").at("blocks").get<double>(), 16.0);

    // Archiving is the other way work leaves the queue, and the handler has no
    // status branch at all — it reads the task and scores it, whatever the
    // status says — so an archived task reports the same 77.0 and the same
    // breakdown rather than a second, quieter rule.
    const nlohmann::json archived =
        callOk(harness.services, "update_task", { { "id", id }, { "status", "archived" } });
    ASSERT_EQ(archived.at("status").get<std::string>(), "archived");

    const nlohmann::json after = callOk(harness.services, "get_task", { { "id", id } });
    EXPECT_DOUBLE_EQ(after.at("score").get<double>(), 77.0);
    EXPECT_DOUBLE_EQ(after.at("score_parts").at("importance").get<double>(), 25.0);
    EXPECT_DOUBLE_EQ(after.at("score_parts").at("urgency").get<double>(), 30.0);
    EXPECT_DOUBLE_EQ(after.at("score_parts").at("age").get<double>(), 6.0);
    EXPECT_DOUBLE_EQ(after.at("score_parts").at("blocks").get<double>(), 16.0);

    // Reporting is not ranking: the queue still refuses both finished statuses,
    // which is the half of the contract the score being non-zero must not
    // undermine.
    EXPECT_TRUE(callOk(harness.services, "get_queue").at("queue").empty());
}

TEST_F(ServicesTest, GetStatsReportsTheTopOfTheQueue)
{
    const nlohmann::json created = addTask(harness.services, "the important one",
                                           { { "importance", 5 }, { "due_at", 0 } });

    const nlohmann::json stats = callOk(harness.services, "get_stats");

    ASSERT_TRUE(stats.contains("open"));
    EXPECT_EQ(stats.at("open").get<std::size_t>(), 1U);
    EXPECT_EQ(stats.at("top_title").get<std::string>(), "the important one");
    EXPECT_GT(stats.at("top_score").get<double>(), 0.0);
    EXPECT_EQ(stats.at("now").get<std::int64_t>(), harness.clock.nowEpochSeconds());

    // An empty backlog reports nulls rather than dropping the keys, so a
    // client can rely on the shape of the reply.
    callOk(harness.services, "delete_task", { { "id", created.at("id") } });
    const nlohmann::json empty = callOk(harness.services, "get_stats");
    EXPECT_TRUE(empty.at("top_title").is_null());
    EXPECT_TRUE(empty.at("top_score").is_null());
}

TEST_F(ServicesTest, GetStatsAdvertisesNoLimitAndAgreesWithTheQueueHead)
{
    // get_stats used to advertise a `limit` that only sized an already-ranked
    // vector before its head was read, so every value produced the same reply:
    // a parameter that promises a cheaper scan and changes nothing. It is gone,
    // and the schema must say so — the top of a truncated backlog is not the
    // top of the queue, so there is no honest bounded version to offer.
    const MethodSpec *spec = Services::findSpec("get_stats");
    ASSERT_NE(nullptr, spec);
    ASSERT_TRUE(spec->params_schema.contains("properties"));
    EXPECT_TRUE(spec->params_schema.at("properties").empty());
    EXPECT_FALSE(spec->params_schema.contains("required"));

    addTask(harness.services, "the important one", { { "importance", 5 }, { "due_at", 0 } });
    addTask(harness.services, "the trivial one", { { "importance", 1 } });

    const nlohmann::json stats = callOk(harness.services, "get_stats");
    const nlohmann::json queue = callOk(harness.services, "get_queue");
    ASSERT_FALSE(queue.at("queue").empty());

    // One FixedClock serves both calls, so both ranked the same backlog against
    // the same instant: the reported top and the head of the queue have to be
    // the same entry, score for score.
    EXPECT_EQ(stats.at("top_title").get<std::string>(),
              queue.at("queue").front().at("title").get<std::string>());
    EXPECT_DOUBLE_EQ(stats.at("top_score").get<double>(),
                     queue.at("queue").front().at("score").get<double>());

    // A leftover `limit` in a client's request must never look like it had an
    // effect, and it no longer can: an undeclared key is refused by name rather
    // than ignored, so a caller that still sends the parameter it remembers
    // from an older version is told there is no such parameter here. Ignoring
    // it would be the quieter failure — a knob that silently does nothing is
    // indistinguishable from one that worked.
    const Error stray = callError(harness.services, "get_stats", { { "limit", 1 } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, stray.code);
    EXPECT_NE(std::string::npos, stray.message.find("limit"));
}

TEST_F(ServicesTest, SetWeightsMergesOntoTheStoredWeights)
{
    const nlohmann::json before = callOk(harness.services, "get_weights");

    const nlohmann::json updated = callOk(harness.services, "set_weights", { { "urgency", 0.0 } });

    EXPECT_DOUBLE_EQ(updated.at("urgency").get<double>(), 0.0);
    // The documented retuning path is a partial update: the fields the caller
    // did not mention keep the values that were already stored.
    EXPECT_DOUBLE_EQ(updated.at("importance").get<double>(),
                     before.at("importance").get<double>());
    EXPECT_DOUBLE_EQ(updated.at("age").get<double>(), before.at("age").get<double>());
    EXPECT_DOUBLE_EQ(updated.at("blocks").get<double>(), before.at("blocks").get<double>());
    EXPECT_DOUBLE_EQ(updated.at("urgency_horizon_days").get<double>(),
                     before.at("urgency_horizon_days").get<double>());

    // And the merge is persisted, not just echoed.
    const nlohmann::json reread = callOk(harness.services, "get_weights");
    EXPECT_DOUBLE_EQ(reread.at("urgency").get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(reread.at("importance").get<double>(), before.at("importance").get<double>());

    // The documented retuning examples are written with integer literals
    // (`set_weights {"urgency": 0}`), and JSON has one number type for both, so
    // an integer weight must be accepted rather than rejected as a wrong type.
    const nlohmann::json integerForm =
        callOk(harness.services, "set_weights", { { "age", 3 } });
    EXPECT_DOUBLE_EQ(integerForm.at("age").get<double>(), 3.0);
    EXPECT_DOUBLE_EQ(integerForm.at("urgency").get<double>(), 0.0);

    // An empty patch is a no-op rather than an error: it says nothing, so there
    // is nothing to refuse, and the weights stay as they are.
    const nlohmann::json unchanged =
        callOk(harness.services, "set_weights", nlohmann::json::object());
    EXPECT_DOUBLE_EQ(unchanged.at("age").get<double>(), 3.0);
    EXPECT_DOUBLE_EQ(unchanged.at("blocks").get<double>(), before.at("blocks").get<double>());
}

TEST_F(ServicesTest, SetWeightsRejectsInvalidValuesAndStoresNothing)
{
    const Error negative = callError(harness.services, "set_weights", { { "importance", -1.0 } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, negative.code);

    const Error wrongType = callError(harness.services, "set_weights", { { "age", "lots" } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, wrongType.code);
    EXPECT_NE(std::string::npos, wrongType.message.find("age"));

    const Error zeroHorizon =
        callError(harness.services, "set_weights", { { "urgency_horizon_days", 0.0 } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, zeroHorizon.code);

    // A rejected change must leave every weight exactly as it was: a silently
    // ignored weight change is indistinguishable from a failed one, and the
    // caller would tune for an hour wondering why nothing moved.
    const nlohmann::json after = callOk(harness.services, "get_weights");
    EXPECT_DOUBLE_EQ(after.at("urgency_horizon_days").get<double>(), 7.0);
    EXPECT_DOUBLE_EQ(after.at("importance").get<double>(), 5.0);
}

TEST_F(ServicesTest, ConcurrentPartialWeightUpdatesKeepEveryKey)
{
    // Five clients each retune a DIFFERENT key, all released at the same
    // instant, ONE update each. Every key must hold its own value afterwards —
    // none may be lost to another thread's snapshot.
    //
    // This is the test that fails when the merge happens outside the store's
    // lock: reading the weights, merging in Services and writing them back
    // releases the lock between the read and the write, so the five reads land
    // in one wave and the later writes each overwrite the whole object with a
    // snapshot that predates the earlier ones — the keys of whichever writers
    // went first revert to their defaults. A lost update like that leaves no
    // trace on either side of the wire, which is why it needs a test.
    //
    // One update per thread, deliberately: a thread that keeps re-sending its
    // own key repairs the damage from the previous round on its next call, so a
    // loop of updates hides exactly the lost update this test is looking for.
    struct PartialUpdate
    {
        const char *key;
        double value;
    };
    constexpr PartialUpdate kUpdates[] = {
        { "importance", 7.0 },
        { "urgency", 11.0 },
        { "age", 3.0 },
        { "blocks", 13.0 },
        { "urgency_horizon_days", 2.0 },
    };

    // A latch rather than a start flag: every thread blocks until all five are
    // ready, so the calls really do overlap instead of queueing up behind each
    // thread's start-up and completing one at a time.
    std::latch release{ static_cast<std::ptrdiff_t>(std::size(kUpdates)) };
    std::atomic<int> failures{ 0 };

    std::vector<std::thread> threads;
    threads.reserve(std::size(kUpdates));
    for (const PartialUpdate &update : kUpdates)
    {
        threads.emplace_back([this, &release, &failures, update] {
            release.arrive_and_wait();
            const nlohmann::json patch{ { update.key, update.value } };
            if (!harness.services.invoke("set_weights", patch).ok())
            {
                // Only counted here: an assertion macro on a worker thread is
                // not reliable, and one failed call already makes the final
                // assertion meaningless.
                ++failures;
            }
        });
    }
    for (std::thread &thread : threads)
    {
        thread.join();
    }

    EXPECT_EQ(0, failures.load()) << "a concurrent set_weights call failed";
    const nlohmann::json stored = callOk(harness.services, "get_weights");
    for (const PartialUpdate &update : kUpdates)
    {
        // Each key holds the value only its own thread ever set, so no thread's
        // update was discarded by another thread's stale snapshot.
        EXPECT_DOUBLE_EQ(stored.at(update.key).get<double>(), update.value)
            << update.key << " was lost to a concurrent partial update";
    }
}

// --- synchronisation ----------------------------------------------------
//
// The format itself is exercised in test_task_sync.cpp and the store's own
// export/merge in test_task_store.cpp. What these tests protect is the API
// above them: that the pair is in the catalog, that the export reply really is
// a parsable document, and above all that a merge is never applied by accident.

/// The whole backlog of a second, independent machine — its own store, its own
/// clock — as the file its owner would commit.
///
/// Driven through export_tasks rather than hand-written JSONL so these tests
/// cannot drift from what the exporter actually produces: a fixture written by
/// hand would keep passing after a change to the format, which is the one
/// thing synchronisation cannot survive.
[[nodiscard]] std::string exportFromAnotherMachine(const std::vector<std::string> &titles)
{
    Harness other;
    for (const std::string &title : titles)
    {
        addTask(other.services, title);
    }
    return callOk(other.services, "export_tasks").at("jsonl").get<std::string>();
}

TEST_F(ServicesTest, GetStatusReportsTombstones)
{
    addTask(harness.services, "kept");
    const nlohmann::json thrownAway = addTask(harness.services, "thrown away");

    // Zero rather than absent before any deletion, so a client can read the
    // count without a key check.
    EXPECT_EQ(callOk(harness.services, "get_status").at("tombstones").get<std::size_t>(), 0U);

    callOk(harness.services, "delete_task", { { "id", thrownAway.at("id") } });

    const nlohmann::json status = callOk(harness.services, "get_status");

    // A tombstone is bookkeeping, not a task: the task tallies count what is
    // here, and the deleted row is gone from them.
    EXPECT_EQ(status.at("tombstones").get<std::size_t>(), 1U);
    EXPECT_EQ(status.at("open").get<std::size_t>(), 1U);

    // The same fact from the other end — the count of records a sync has to
    // carry is one live task plus one tombstone.
    EXPECT_EQ(callOk(harness.services, "export_tasks").at("count").get<std::size_t>(), 2U);
}

TEST_F(ServicesTest, ExportTasksReturnsOneParsableRecordPerLine)
{
    addTask(harness.services, "keep this one");
    const nlohmann::json doomed = addTask(harness.services, "delete this one");
    callOk(harness.services, "delete_task", { { "id", doomed.at("id") } });

    const nlohmann::json exported = callOk(harness.services, "export_tasks");

    EXPECT_EQ(exported.at("format").get<int>(), kExportFormatVersion);

    const std::string jsonl = exported.at("jsonl").get<std::string>();
    const Result<std::vector<SyncRecord>> parsed = parseJsonl(jsonl);
    ASSERT_TRUE(parsed.ok()) << parsed.error().message;

    // One live task and the tombstone of the other. The deletion has to travel:
    // the machine that still holds the task would otherwise export it back and
    // the merge would restore work that was deliberately removed.
    ASSERT_EQ(parsed.value().size(), 2U);
    EXPECT_EQ(exported.at("count").get<std::size_t>(), parsed.value().size());

    std::size_t tombstones = 0;
    for (const SyncRecord &record : parsed.value())
    {
        EXPECT_TRUE(looksLikeUuidV4(record.uid)) << record.uid;
        if (record.deleted)
        {
            ++tombstones;
            EXPECT_EQ(record.uid, doomed.at("uid").get<std::string>())
                << "a tombstone must carry the uid of the task that was deleted, which "
                   "is the only identity that means anything on the other machine";
        }
    }
    EXPECT_EQ(tombstones, 1U);

    // One record per line, and as many lines as the reply says there are
    // records: the file's entire purpose is that git can merge two machines'
    // exports line by line, which a record spanning lines would destroy.
    std::size_t lines = 0;
    std::istringstream stream(jsonl);
    std::string line;
    while (std::getline(stream, line))
    {
        if (line.empty())
        {
            continue;
        }
        ++lines;
        const Result<SyncRecord> one = parseJsonLine(line);
        EXPECT_TRUE(one.ok()) << "line " << lines << " does not parse: " << line;
    }
    EXPECT_EQ(lines, exported.at("count").get<std::size_t>());
}

TEST_F(ServicesTest, ImportTasksWithDryRunOmittedChangesNothing)
{
    const std::string incoming = exportFromAnotherMachine({ "invented over there" });
    addTask(harness.services, "already here");
    const std::size_t before =
        callOk(harness.services, "get_status").at("open").get<std::size_t>();

    const nlohmann::json report =
        callOk(harness.services, "import_tasks", { { "jsonl", incoming } });

    // A merge overwrites and removes local work, so omitting the flag must mean
    // "show me what would happen". This is the assertion that fails the moment
    // the default is flipped to destructive: the merge would leave two tasks
    // where the caller expected one.
    EXPECT_EQ(callOk(harness.services, "get_status").at("open").get<std::size_t>(), before);
    ASSERT_EQ(callOk(harness.services, "list_tasks").size(), 1U);

    // The report is not a placeholder, so the preview really took every
    // decision instead of short-circuiting to an empty answer.
    EXPECT_EQ(report.at("inserted").get<std::size_t>(), 1U);
    EXPECT_TRUE(report.at("dry_run").get<bool>())
        << "the reply must say which of the two happened: the counts are the same "
           "either way, so a caller cannot infer it";

    // An explicit null is how several clients spell "no opinion" (the MCP
    // transport normalises a null `arguments` the same way), so it is treated
    // as an absent key: still a dry run, and certainly not false.
    nlohmann::json nulledParams{ { "jsonl", incoming } };
    nulledParams["dry_run"] = nullptr;
    const nlohmann::json nulled = callOk(harness.services, "import_tasks", nulledParams);

    EXPECT_EQ(nulled.at("inserted").get<std::size_t>(), 1U);
    EXPECT_TRUE(nulled.at("dry_run").get<bool>());
    EXPECT_EQ(callOk(harness.services, "get_status").at("open").get<std::size_t>(), before);
}

TEST_F(ServicesTest, ImportTasksAppliesTheMergeOnlyWhenDryRunIsFalse)
{
    const std::string incoming = exportFromAnotherMachine({ "invented over there" });
    const Result<std::vector<SyncRecord>> incomingRecords = parseJsonl(incoming);
    ASSERT_TRUE(incomingRecords.ok()) << incomingRecords.error().message;
    ASSERT_EQ(incomingRecords.value().size(), 1U);

    const nlohmann::json report = callOk(
        harness.services, "import_tasks", { { "jsonl", incoming }, { "dry_run", false } });

    EXPECT_EQ(report.at("inserted").get<std::size_t>(), 1U);
    EXPECT_FALSE(report.at("dry_run").get<bool>());

    const nlohmann::json stored = callOk(harness.services, "list_tasks");
    ASSERT_EQ(stored.size(), 1U);
    EXPECT_EQ(stored.front().at("title").get<std::string>(), "invented over there");
    // The task arrives with the uid it was given on the other machine: that,
    // not the row number this store assigns it, is what identifies it, and the
    // integer id it now has means nothing anywhere else.
    EXPECT_EQ(stored.front().at("uid").get<std::string>(), incomingRecords.value().front().uid);

    // Re-syncing the same file inserts nothing: a uid that is already here is
    // what stops the next sync from making a second copy of the same task.
    const nlohmann::json again = callOk(
        harness.services, "import_tasks", { { "jsonl", incoming }, { "dry_run", false } });
    EXPECT_EQ(again.at("inserted").get<std::size_t>(), 0U);
    EXPECT_EQ(callOk(harness.services, "list_tasks").size(), 1U);
}

TEST_F(ServicesTest, ImportTasksRequiresAJsonlString)
{
    const Error missing = callError(harness.services, "import_tasks", nlohmann::json::object());
    EXPECT_EQ(ErrorCode::kInvalidArgument, missing.code);
    EXPECT_NE(std::string::npos, missing.message.find("jsonl"));

    const Error wrongType = callError(harness.services, "import_tasks", { { "jsonl", 5 } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, wrongType.code);
    EXPECT_NE(std::string::npos, wrongType.message.find("jsonl"));

    // A wrongly typed flag is refused rather than quietly replaced by the
    // default: a caller that asked for dry_run "yes" cannot tell a refused
    // merge from an applied one, and the applied one cannot be undone.
    const Error badFlag =
        callError(harness.services, "import_tasks", { { "jsonl", "" }, { "dry_run", "yes" } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, badFlag.code);
    EXPECT_NE(std::string::npos, badFlag.message.find("dry_run"));
}

TEST_F(ServicesTest, ImportTasksRefusesAFileItCannotReadEntirely)
{
    const std::string incoming = exportFromAnotherMachine({ "would have arrived" });
    ASSERT_FALSE(incoming.empty());

    // One readable record followed by a line no parser can accept (the uid is
    // not a uuid). Merging the part that parsed would leave this machine in a
    // state that is neither its old backlog nor the other machine's, and the
    // next sync would treat that mixture as truth — so the whole call fails
    // and nothing is written.
    std::string damaged = incoming;
    if ('\n' != damaged.back())
    {
        damaged.push_back('\n');
    }
    damaged += "{\"v\":1,\"uid\":\"not-a-uuid\",\"deleted\":true}\n";

    const Error failure = callError(
        harness.services, "import_tasks", { { "jsonl", damaged }, { "dry_run", false } });

    EXPECT_EQ(ErrorCode::kInvalidArgument, failure.code);
    EXPECT_TRUE(callOk(harness.services, "list_tasks").empty())
        << "the records before the unreadable line must not have been applied";
    EXPECT_EQ(callOk(harness.services, "get_status").at("open").get<std::size_t>(), 0U);
}

// --- parameter validation and dispatch ----------------------------------

TEST_F(ServicesTest, MissingRequiredParametersAreNamed)
{
    const Error noTitle = callError(harness.services, "add_task", nlohmann::json::object());
    EXPECT_EQ(ErrorCode::kInvalidArgument, noTitle.code);
    EXPECT_NE(std::string::npos, noTitle.message.find("title"));

    const Error noId = callError(harness.services, "complete_task", nlohmann::json::object());
    EXPECT_EQ(ErrorCode::kInvalidArgument, noId.code);
    EXPECT_NE(std::string::npos, noId.message.find("id"));
}

TEST_F(ServicesTest, WrongTypesAreRejectedRatherThanCoerced)
{
    // "5" is not 5: coercing it would make a malformed call indistinguishable
    // from a well-formed one, and the message says which type arrived so the
    // caller can see the mistake.
    const Error numericString = callError(harness.services, "add_task",
                                          { { "title", "x" }, { "importance", "high" } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, numericString.code);
    EXPECT_NE(std::string::npos, numericString.message.find("importance"));
    EXPECT_NE(std::string::npos, numericString.message.find("string"));

    const Error idString = callError(harness.services, "get_task", { { "id", "7" } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, idString.code);
    EXPECT_NE(std::string::npos, idString.message.find("id"));
    EXPECT_NE(std::string::npos, idString.message.find("string"));

    const Error notesNumber =
        callError(harness.services, "add_task", { { "title", "x" }, { "notes", 12 } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, notesNumber.code);
    EXPECT_NE(std::string::npos, notesNumber.message.find("notes"));

    const Error tagsObject =
        callError(harness.services, "add_task",
                  { { "title", "x" }, { "tags", nlohmann::json::object() } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, tagsObject.code);

    const Error flagNumber = callError(harness.services, "get_queue",
                                       { { "include_in_progress", 1 } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, flagNumber.code);
    EXPECT_NE(std::string::npos, flagNumber.message.find("include_in_progress"));
}

TEST_F(ServicesTest, IntegersTooLargeForAnInt64AreRejectedInsteadOfWrapping)
{
    // UINT64_MAX. nlohmann stores it as an unsigned number and converts it to
    // int64 with an unchecked static_cast, so it does NOT throw — it silently
    // yields -1. Reading it without a range check is worse than a wrong number
    // for due_at: -1 is 1969, so the task is not merely accepted, it becomes
    // the most urgent row in the queue.
    const nlohmann::json unsignedMax = nlohmann::json::parse("18446744073709551615");

    const Error added = callError(harness.services, "add_task",
                                  { { "title", "wrapped deadline" }, { "due_at", unsignedMax } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, added.code);
    EXPECT_NE(std::string::npos, added.message.find("due_at"));
    EXPECT_NE(std::string::npos, added.message.find("out of range"));
    EXPECT_NE(std::string::npos, added.message.find("18446744073709551615"));

    // Nothing was stored: the rejected call must not have created the task at
    // all, let alone one dated 1969.
    EXPECT_TRUE(callOk(harness.services, "get_queue").at("queue").empty());
    EXPECT_EQ(0U, callOk(harness.services, "get_status").at("open").get<std::size_t>());

    // The update path goes through the same reader, so an existing task's
    // deadline cannot be rewritten to the wrapped value either.
    const nlohmann::json existing = addTask(harness.services, "keep my deadline");
    const Error patched = callError(
        harness.services, "update_task", { { "id", existing.at("id") }, { "due_at", unsignedMax } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, patched.code);
    EXPECT_TRUE(callOk(harness.services, "get_task", { { "id", existing.at("id") } })
                    .at("due_at")
                    .is_null())
        << "the stored deadline must still be absent, not -1";

    // An id of that size is caller error too, rather than an answer to "no task
    // with id -1" — a value the caller never sent.
    const Error fetched = callError(harness.services, "get_task", { { "id", unsignedMax } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, fetched.code);
    EXPECT_NE(std::string::npos, fetched.message.find("id"));
    EXPECT_NE(std::string::npos, fetched.message.find("out of range"));

    // The boundary itself still works: INT64_MAX is a legitimate (if implausible)
    // id and must reach the lookup rather than being swept up by the check.
    const nlohmann::json int64Max = nlohmann::json::parse("9223372036854775807");
    const Error tooLarge = callError(harness.services, "get_task", { { "id", int64Max } });
    EXPECT_EQ(ErrorCode::kNotFound, tooLarge.code);
}

TEST_F(ServicesTest, UnknownMethodsAreReportedAsSuch)
{
    const Error failure = callError(harness.services, "add_tasks", nlohmann::json::object());

    EXPECT_EQ(ErrorCode::kInvalidArgument, failure.code);
    EXPECT_EQ(std::string{ "unknown method: add_tasks" }, failure.message);
    // -32602 (invalid params) rather than -32601 (method not found): the
    // vocabulary is closed, but the code has to be one every transport already
    // understands, and the message carries the actionable part.
    EXPECT_EQ(-32602, rpcErrorCode(failure.code));
}

TEST_F(ServicesTest, ParamsMustBeAnObjectAndNullIsTreatedAsEmpty)
{
    const RpcResult withNull = harness.services.invoke("get_status", nlohmann::json{});
    EXPECT_TRUE(withNull.ok()) << "a request without params must work";

    const Error withArray = callError(harness.services, "get_status", nlohmann::json::array());
    EXPECT_EQ(ErrorCode::kInvalidArgument, withArray.code);
    EXPECT_NE(std::string::npos, withArray.message.find("params"));
}

TEST_F(ServicesTest, AClientSuppliedNowIsOverwrittenRatherThanTrusted)
{
    // invoke() stamps "__taskpilot_now" with the instant it sampled, and it does
    // so unconditionally. That is the whole anti-forgery guarantee: the key
    // travels with the params — the handler signature takes nothing else — so a
    // caller that could set it would choose the clock its own ranking was
    // computed against. The stamp is therefore not "metadata the caller may
    // already have supplied"; it is overwritten, and the check below is what
    // says so.
    //
    // Epoch 0 is the sharpest forgeable value available: it is a legal
    // integer, a plausible-looking default, and it moves the reference instant
    // by decades.
    const nlohmann::json created =
        addTask(harness.services, "three days old", { { "importance", 5 }, { "blocks", 2 } });
    addTask(harness.services, "brand new", { { "importance", 1 } });

    harness.clock.setNow(created.at("created_at").get<std::int64_t>() + 3 * kSecondsPerDay);
    callOk(harness.services, "update_task",
           { { "id", created.at("id") }, { "due_at", harness.clock.nowEpochSeconds() } });

    const nlohmann::json honest = callOk(harness.services, "get_queue");
    const nlohmann::json forged =
        callOk(harness.services, "get_queue", { { "__taskpilot_now", 0 } });

    // The ranking IS the reply — order, totals and the four terms behind them —
    // so it is compared whole. One line per entry, because a mismatch that
    // prints two entire JSON documents is a mismatch nobody reads.
    const auto signatureOf = [](const nlohmann::json &queue) {
        std::vector<std::string> rows;
        for (const nlohmann::json &entry : queue.at("queue"))
        {
            rows.push_back(entry.at("title").get<std::string>() +
                           " score=" + std::to_string(entry.at("score").get<double>()) +
                           " parts=" + entry.at("score_parts").dump());
        }
        return rows;
    };
    EXPECT_EQ(signatureOf(forged), signatureOf(honest))
        << "a caller-supplied __taskpilot_now must not survive invoke()";

    // The reply's own `now` is the instant the handlers were given, so it is
    // the forged value if and only if the stamp was skipped.
    EXPECT_EQ(forged.at("now").get<std::int64_t>(), harness.clock.nowEpochSeconds());

    // And the absolute value, so that two identically wrong calls cannot pass
    // by agreeing with each other: importance 5 x 5 = 25, urgency 30 x 1.0 = 30
    // (due now saturates the ramp), age 2 x 3 = 6, blocks 8 x 2 = 16 — 77.0.
    // Ranked against the epoch instead, the same task scores 41.0: its deadline
    // would be decades out (a horizon of seven days earns no urgency at all)
    // and its created_at would be in the future, which the age clamp floors at
    // zero — 25 + 16, with the other two terms gone.
    ASSERT_FALSE(forged.at("queue").empty());
    EXPECT_DOUBLE_EQ(forged.at("queue").front().at("score").get<double>(), 77.0);
    EXPECT_DOUBLE_EQ(forged.at("queue").front().at("score_parts").at("importance").get<double>(),
                     25.0);
    EXPECT_DOUBLE_EQ(forged.at("queue").front().at("score_parts").at("urgency").get<double>(),
                     30.0);
    EXPECT_DOUBLE_EQ(honest.at("queue").front().at("score").get<double>(), 77.0);
}

TEST_F(ServicesTest, EveryMethodRefusesAnUndeclaredParameter)
{
    // Iterating the catalog, like the dispatch cross-check above, so a method
    // added later is covered the moment it is advertised. The key is spelled
    // so that it is a substring of no declared parameter name, which keeps
    // "the message names the offending key" from being satisfied by the
    // accepted list that follows it in the same message.
    constexpr const char *kUndeclared{ "zzz_undeclared_parameter" };

    for (const MethodSpec &spec : Services::methodSpecs())
    {
        const Error failure = callError(harness.services, spec.name, { { kUndeclared, 1 } });

        EXPECT_EQ(ErrorCode::kInvalidArgument, failure.code)
            << spec.name << " ignored an undeclared parameter: " << failure.message;
        EXPECT_NE(std::string::npos, failure.message.find(kUndeclared))
            << spec.name << " did not name the offending key: " << failure.message;

        // Every key the method DOES accept is listed back. That list is the
        // half a caller can act on: a model writing JSON by hand can correct
        // the request in one turn instead of bisecting it against the docs.
        const nlohmann::json &properties = spec.params_schema.at("properties");
        for (auto property = properties.begin(); property != properties.end(); ++property)
        {
            EXPECT_NE(std::string::npos, failure.message.find(property.key()))
                << spec.name << " did not list its accepted parameter '" << property.key()
                << "': " << failure.message;
        }

        // A method that takes no parameters has to refuse ANY key, and say so:
        // an empty accepted list would read like a formatting bug rather than
        // as an answer.
        if (properties.empty())
        {
            EXPECT_NE(std::string::npos, failure.message.find("accepts no parameters"))
                << spec.name << ": " << failure.message;
        }
    }
}

TEST_F(ServicesTest, EveryMethodStillAcceptsItsOwnDeclaredParameters)
{
    // The other half of the contract, and the reason the check cannot be
    // "satisfied" by refusing everything: a call whose keys are ALL declared
    // still has to work. One valid sample per method, looked up by name, so a
    // method with no sample fails here instead of being skipped silently.
    const nlohmann::json toUpdate = addTask(harness.services, "update me");
    const nlohmann::json toComplete = addTask(harness.services, "complete me");
    const nlohmann::json toReopen = addTask(harness.services, "reopen me");
    const nlohmann::json toDelete = addTask(harness.services, "delete me");
    const nlohmann::json toFetch = addTask(harness.services, "fetch me");
    const std::int64_t now = harness.clock.nowEpochSeconds();
    // This machine's own export, as the document import_tasks is given: a real,
    // non-empty file rather than an empty string, so the sample exercises the
    // merge path instead of its degenerate case. It is merged as a dry run, so
    // nothing above changes.
    const nlohmann::json ownExport = callOk(harness.services, "export_tasks");

    struct Sample
    {
        const char *method;
        nlohmann::json params;
    };
    const std::vector<Sample> samples{
        { "get_status", nlohmann::json::object() },
        { "describe_methods", nlohmann::json::object() },
        { "add_task",
          nlohmann::json{ { "title", "sampled" },
                          { "importance", 2 },
                          { "due_at", now + kSecondsPerDay },
                          { "blocks", 1 },
                          { "tags", nlohmann::json::array({ "sample" }) },
                          { "notes", "sampled notes" } } },
        { "update_task",
          nlohmann::json{ { "id", toUpdate.at("id") },
                          { "title", "renamed" },
                          { "notes", "" },
                          { "importance", 4 },
                          { "due_at", now },
                          { "blocks", 2 },
                          { "tags", nlohmann::json::array({ "renamed" }) },
                          { "status", "in_progress" } } },
        { "complete_task", nlohmann::json{ { "id", toComplete.at("id") } } },
        { "reopen_task", nlohmann::json{ { "id", toReopen.at("id") } } },
        { "delete_task", nlohmann::json{ { "id", toDelete.at("id") } } },
        { "get_queue", nlohmann::json{ { "limit", 1 }, { "include_in_progress", true } } },
        { "list_tasks",
          nlohmann::json{ { "status", "open" },
                          { "tag", "sample" },
                          { "limit", 1 },
                          { "order", "created" } } },
        { "get_task", nlohmann::json{ { "id", toFetch.at("id") } } },
        { "get_stats", nlohmann::json::object() },
        { "export_tasks", nlohmann::json::object() },
        { "import_tasks",
          nlohmann::json{ { "jsonl", ownExport.at("jsonl") }, { "dry_run", true } } },
        { "get_weights", nlohmann::json::object() },
        { "set_weights", nlohmann::json{ { "importance", 6.0 } } },
    };

    for (const MethodSpec &spec : Services::methodSpecs())
    {
        const auto sample =
            std::find_if(samples.begin(), samples.end(), [&spec](const Sample &candidate) {
                return spec.name == candidate.method;
            });
        ASSERT_NE(samples.end(), sample)
            << spec.name << " has no valid sample here: add one, or the check could start "
                           "refusing a declared parameter with nothing to notice";

        // The sample may not smuggle in an undeclared key either, or it would
        // pass only because the check was broken.
        const nlohmann::json &properties = spec.params_schema.at("properties");
        for (auto entry = sample->params.begin(); entry != sample->params.end(); ++entry)
        {
            EXPECT_TRUE(properties.contains(entry.key()))
                << spec.name << "'s sample uses undeclared key '" << entry.key() << "'";
        }

        const RpcResult result = harness.services.invoke(spec.name, sample->params);
        if (!result.ok())
        {
            ADD_FAILURE() << spec.name << " refused a call whose keys are all declared: "
                          << result.error().message;
        }
    }

    // update_task is the one method whose sample cannot carry every declared
    // key: due_at and clear_due_at together are the documented conflict, so a
    // sample holding both would be refused for a reason that has nothing to do
    // with this check. The other half of that pair gets its own call.
    const RpcResult cleared = harness.services.invoke(
        "update_task", nlohmann::json{ { "id", toFetch.at("id") }, { "clear_due_at", true } });
    if (!cleared.ok())
    {
        ADD_FAILURE() << "clear_due_at was refused as an undeclared parameter: "
                      << cleared.error().message;
    }
}

TEST_F(ServicesTest, TheReservedNowKeyIsNotAnUndeclaredParameter)
{
    // invoke() stamps __taskpilot_now into the params of EVERY call, and the
    // handlers read it from there, so a check that tripped on the reserved key
    // would refuse every request in this file — including calls to methods that
    // take no parameters, where the stamp is the only key in the object.
    // get_status is that sharpest case.
    const nlohmann::json status =
        callOk(harness.services, "get_status", { { "__taskpilot_now", 0 } });
    EXPECT_EQ(status.at("now").get<std::int64_t>(), harness.clock.nowEpochSeconds());

    // The exemption is for the reserved key alone, and it is invisible: a stray
    // key beside it is still named, while the reserved name — which the caller
    // never sent — must not appear in the complaint about it.
    const Error failure = callError(
        harness.services, "get_stats", { { "__taskpilot_now", 1 }, { "zzz_undeclared", 1 } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, failure.code);
    EXPECT_NE(std::string::npos, failure.message.find("zzz_undeclared"));
    EXPECT_EQ(std::string::npos, failure.message.find("__taskpilot_now")) << failure.message;
}

TEST_F(ServicesTest, AMistypedListTasksFilterIsRefusedRatherThanIgnored)
{
    // The defect this check exists for, in the shape it was found: list_tasks
    // with a mistyped filter key returned the whole backlog. The caller (usually
    // a model writing JSON by hand) asked for one status, got every task, and
    // had nothing in the reply to tell it the filter had never run — the worst
    // kind of failure, because the answer looks like a good one.
    addTask(harness.services, "still open");
    const nlohmann::json finished = addTask(harness.services, "already done");
    callOk(harness.services, "complete_task", { { "id", finished.at("id") } });

    const Error mistyped = callError(harness.services, "list_tasks", { { "statsu", "done" } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, mistyped.code);
    EXPECT_NE(std::string::npos, mistyped.message.find("statsu"));
    // The accepted names come back with the refusal, which is what the caller
    // needs in order to fix it without a documentation lookup.
    EXPECT_NE(std::string::npos, mistyped.message.find("status"));

    // The correctly spelled key alone still filters: the refusal is about the
    // undeclared key, not a blanket rejection of filtered queries.
    ASSERT_EQ(callOk(harness.services, "list_tasks", { { "status", "done" } }).size(), 1U);

    // And a request carrying both — the filter the caller meant and the typo it
    // made as well — is an error rather than a query in which one filter
    // silently did nothing.
    const Error partial = callError(
        harness.services, "list_tasks", { { "status", "done" }, { "statsu", "open" } });
    EXPECT_EQ(ErrorCode::kInvalidArgument, partial.code);
    EXPECT_NE(std::string::npos, partial.message.find("statsu"));
}

TEST(ServicesClockTest, TheClockIsSampledOncePerCall)
{
    std::unique_ptr<TaskStore> store = openStore();
    CountingClock clock(wallNow());
    Services services(*store, clock, kVersion);

    clock.reset();
    const RpcResult first = services.invoke("get_status", nlohmann::json::object());
    ASSERT_TRUE(first.ok()) << first.error().message;
    EXPECT_EQ(1, clock.calls())
        << "a call must sample the clock once, so every task in it is ranked against one instant";

    const RpcResult second = services.invoke("get_queue", nlohmann::json::object());
    ASSERT_TRUE(second.ok()) << second.error().message;
    EXPECT_EQ(2, clock.calls());
}

TEST(ServicesErrorCodeTest, EveryCodeHasItsWireCounterpart)
{
    // -32000..-32099 is reserved by JSON-RPC 2.0 for application errors, which
    // is where the domain failures live: an unknown task id is not a transport
    // fault, and a client has to be able to tell the two apart.
    EXPECT_EQ(-32602, rpcErrorCode(ErrorCode::kInvalidArgument));
    EXPECT_EQ(-32001, rpcErrorCode(ErrorCode::kNotFound));
    EXPECT_EQ(-32002, rpcErrorCode(ErrorCode::kConflict));
    EXPECT_EQ(-32003, rpcErrorCode(ErrorCode::kStorageFailure));
    EXPECT_EQ(-32603, rpcErrorCode(ErrorCode::kInternal));
}

} // namespace
} // namespace taskpilot
