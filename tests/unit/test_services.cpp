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

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <latch>
#include <memory>
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
    EXPECT_EQ(status.at("method_count").get<int>(), 13);
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
    ASSERT_EQ(catalog.size(), 13U);

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

TEST_F(ServicesTest, TiedScoresAreBrokenByAgeThenId)
{
    // Two identical tasks: same importance, same (absent) deadline, same (zero)
    // blocks, and ages that are equal to within the rounding of the store's
    // own clock. The order must still be fully determined — the first task
    // created has both the older created_at and the lower id — because an
    // order that depends on the input order or on the sort implementation is
    // not reproducible.
    addTask(harness.services, "first");
    addTask(harness.services, "second");

    const nlohmann::json queue = callOk(harness.services, "get_queue");
    ASSERT_EQ(queue.at("queue").size(), 2U);
    EXPECT_EQ(queue.at("queue").at(0).at("title").get<std::string>(), "first");
    EXPECT_EQ(queue.at("queue").at(1).at("title").get<std::string>(), "second");
    // The ages differ by at most a second of wall time, so the scores are
    // equal to within that: this really is a tie, broken by the tie-breakers
    // rather than by a term.
    EXPECT_NEAR(queue.at("queue").at(0).at("score").get<double>(),
                queue.at("queue").at(1).at("score").get<double>(), 1e-3);
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

    // A leftover `limit` in a client's request is now just an undeclared key,
    // which every method ignores; what it must never do again is look like it
    // had an effect.
    const nlohmann::json stray = callOk(harness.services, "get_stats", { { "limit", 1 } });
    EXPECT_DOUBLE_EQ(stray.at("top_score").get<double>(), stats.at("top_score").get<double>());
    EXPECT_EQ(stray.at("top_title").get<std::string>(), stats.at("top_title").get<std::string>());
    EXPECT_EQ(stray.at("open").get<std::size_t>(), stats.at("open").get<std::size_t>());
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
