// tests/integration/test_daemon_roundtrip.cpp
//
// End-to-end round trip: a real TaskStore on a temporary database file, a real
// JsonRpcServer bound to an OS-assigned port, and a real ControlClient driving
// it over a real TCP socket. Nothing here is mocked. This is the only test
// that proves the four pieces agree — the store writes what the services read,
// the dispatcher serializes what the client can parse, and the error mapping
// survives the trip in both directions.
//
// Three constraints shape this file:
//
//   1. NO FIXED PORT. The server is started on port 0 and the test reads the
//      assigned port back from port(). A hard-coded port would collide with a
//      daemon the user happens to be running, and with a parallel test run.
//
//   2. NO FIXED DATABASE. The store lives in a temp file, never under data/,
//      so running the suite can never touch the user's real backlog.
//
//   3. STOP ON EVERY PATH. The suite registers one CTest entry per TEST()
//      with a 60-second timeout; a server thread left parked in read_until()
//      would sit there until the timeout fired and turn a clear assertion
//      failure into a mystifying "timed out". ServerStopper calls stop() from
//      its destructor, so an ASSERT failure unwinds through it. A test that
//      hangs on failure is worse than one that fails.
//
// The ordering assertions are built so their expected values do NOT depend on
// the wall clock. Scores are pinned exactly by zeroing every weight except
// one, and the deadline-driven case uses offsets measured in whole days from
// the 7-day urgency horizon, so the only time-dependent number in the file is
// the ~0.5-point tolerance around C's urgency score.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "control/ControlClient.hpp"
#include "control/JsonRpcServer.hpp"
#include "control/Services.hpp"
#include "core/Clock.hpp"
#include "core/Result.hpp"
#include "core/TaskStore.hpp"

namespace taskpilot
{
namespace
{

/// Version the fixture's Services and JsonRpcServer report, and the value
/// get_status must echo back.
constexpr const char *kServerVersion{ "0.1.0" };

/// Port 0 means "ask the OS for a free port". The header guarantees port()
/// reports the assigned one back, which is the whole reason this test can run
/// alongside a live daemon without colliding with it.
constexpr std::uint16_t kAnyFreePort{ 0 };

/// Generous enough for a loaded machine, short enough that a wedged daemon
/// fails the test long before the 60s CTest timeout.
constexpr std::int64_t kCallTimeoutMs{ 5000 };

/// Fallback returned by the field readers when the daemon did not publish a
/// field. Deliberately outside every legal score so a missing field fails the
/// assertion that follows instead of coincidentally matching a real value.
constexpr double kMissingNumber{ -12345.678 };

/// The interface every socket in this file uses. Kept as a named constant
/// because the server binds it and the client connects to it; the two must
/// agree or every call fails for reasons unrelated to the test's subject.
constexpr const char *kLoopback{ "127.0.0.1" };

/// One-line rendering of an error for assertion messages.
///
/// ErrorCode is a SCOPED enum, so it does not stream and this gtest has no
/// enum printer: without this helper a failure would read "expected kNotFound,
/// got 4-byte object <04 00 00 00>", which names the wrong thing. The code
/// goes out through toString(), the same stable name the wire carries.
[[nodiscard]] std::string describe(const Error &error)
{
    return toString(error.code) + ": " + error.message;
}

/// Owns a unique temp-file path and deletes it — plus SQLite's sidecar files —
/// on destruction, so a failing test leaves nothing behind and a passing one
/// leaves nothing to clean up either.
class TempDatabase
{
  public:
    TempDatabase()
    {
        const std::filesystem::path directory = std::filesystem::temp_directory_path();
        std::string pattern = (directory / "taskpilot-it-XXXXXX").string();

        // mkstemp needs a writable buffer and is the only portable way to get
        // a name no concurrent process can also pick.
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');

        const int fd = ::mkstemp(buffer.data());
        if (fd >= 0)
        {
            // The name is what we wanted; the file itself is left in place for
            // SQLite to initialize, which it does for a zero-length file.
            ::close(fd);
        }

        m_path = buffer.data();
    }

    ~TempDatabase()
    {
        // SQLite may leave a write-ahead log and a shared-memory file next to
        // the database; deleting only the main file would litter the temp
        // directory on every run.
        std::error_code ignored;
        for (const char *suffix : { "", "-wal", "-shm", "-journal" })
        {
            std::filesystem::remove(m_path + suffix, ignored);
        }
    }

    TempDatabase(const TempDatabase &) = delete;
    TempDatabase &operator=(const TempDatabase &) = delete;

    [[nodiscard]] const std::string &path() const { return m_path; }

  private:
    std::string m_path;
};

/// Calls JsonRpcServer::stop() from its destructor.
///
/// The fixture declares this AFTER the server so it is destroyed FIRST, which
/// is what guarantees the accept and session threads are joined even when an
/// assertion aborted the test body early. Without it, a failed assertion in a
/// test that had already opened a connection would leave the server parked in
/// read_until() until the CTest timeout.
struct ServerStopper
{
    JsonRpcServer *server{ nullptr };

    ~ServerStopper()
    {
        if (nullptr != server)
        {
            server->stop();
        }
    }
};

/// One queue entry reduced to the fields the assertions read. Parsing the wire
/// shape once, here, keeps the test bodies about ranking instead of about JSON
/// navigation.
struct QueueEntry
{
    std::int64_t id{ 0 };
    double score{ 0.0 };
    double urgency_factor{ 0.0 };
    double importance_part{ 0.0 };
};

class DaemonRoundtripTest : public ::testing::Test
{
  protected:
    /// Member order is lifetime order: the destructor runs in reverse, so the
    /// stopper runs first (joining the server's threads), then the server,
    /// then the services and clock, and only then the store they all reference
    /// and the temp file the store holds open.
    TempDatabase m_db;
    std::unique_ptr<TaskStore> m_store;
    SystemClock m_clock;
    std::unique_ptr<Services> m_services;
    std::unique_ptr<JsonRpcServer> m_server;
    ServerStopper m_stopper;
    std::optional<ControlClient> m_client;

    void SetUp() override
    {
        Result<std::unique_ptr<TaskStore>> opened = TaskStore::open(m_db.path());
        ASSERT_TRUE(opened.ok()) << "opening " << m_db.path() << ": " << opened.error().message;
        m_store = std::move(opened).value();

        // The real clock, because the store stamps created_at from the real
        // clock too: a FixedClock here would make every seeded task look like
        // it was created in the future, and age is clamped at zero, so the
        // aging term would be silently meaningless rather than obviously
        // wrong.
        m_services = std::make_unique<Services>(*m_store, m_clock, kServerVersion);

        m_server = std::make_unique<JsonRpcServer>(kLoopback, kAnyFreePort, m_services->registry(),
                                                   kServerVersion);

        const Status started = m_server->start();
        ASSERT_TRUE(started.ok()) << "starting the control socket: " << started.error().message;

        // port() is only meaningful after a successful start(), and it is what
        // replaces a hard-coded port in this test.
        EXPECT_NE(kAnyFreePort, m_server->port());
        m_stopper.server = m_server.get();

        m_client.emplace(kLoopback, m_server->port(), kCallTimeoutMs);
    }

    /// Seconds since the epoch, for building deadlines relative to "now".
    [[nodiscard]] static std::int64_t currentEpochSeconds()
    {
        const auto elapsed = std::chrono::system_clock::now().time_since_epoch();
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
    }

    /// Assert a failure's code without streaming the scoped enum: this gtest
    /// has no enum printer, so a raw EXPECT_EQ renders the actual value as
    /// "4-byte object <01-00 00-00>" — a failure message that names the wrong
    /// thing. This prints the stable name and the server's own message.
    void expectErrorCode(ErrorCode expected, const Error &error)
    {
        EXPECT_TRUE(expected == error.code)
            << "expected " << toString(expected) << ", got " << describe(error);
    }

    /// Issue a call and fail (without throwing) when the daemon answers with an
    /// error. Returns a null JSON value on failure so the caller's later field
    /// assertions report a missing field rather than crashing on it.
    nlohmann::json call(const std::string &method, const nlohmann::json &params)
    {
        const RpcResult result = m_client->call(method, params);
        if (!result.ok())
        {
            ADD_FAILURE() << method << " failed: " << describe(result.error());
            return nlohmann::json{};
        }

        return result.value();
    }

    /// Numeric field reader. A missing or non-numeric field is a contract
    /// break, so it is reported as an assertion failure with the offending
    /// JSON attached, not as an exception out of the JSON library.
    [[nodiscard]] double numberField(const nlohmann::json &object, const char *key)
    {
        if (!object.is_object() || !object.contains(key) || !object[key].is_number())
        {
            ADD_FAILURE() << "expected a numeric \"" << key << "\" in " << object.dump();
            return kMissingNumber;
        }

        return object[key].get<double>();
    }

    /// String field reader; same contract as numberField.
    [[nodiscard]] std::string stringField(const nlohmann::json &object, const char *key)
    {
        if (!object.is_object() || !object.contains(key) || !object[key].is_string())
        {
            ADD_FAILURE() << "expected a string \"" << key << "\" in " << object.dump();
            return {};
        }

        return object[key].get<std::string>();
    }

    /// The ranked queue, with a limit no seeded case can truncate and
    /// in-progress work included so a queue assertion can never pass merely
    /// because something was filtered out.
    nlohmann::json queueJson()
    {
        return call("get_queue", { { "limit", 50 }, { "include_in_progress", true } });
    }

    /// Reduce a get_queue reply to the fields the assertions read.
    std::vector<QueueEntry> queueEntries(const nlohmann::json &queue_result)
    {
        std::vector<QueueEntry> entries;

        if (!queue_result.is_object() || !queue_result.contains("queue")
            || !queue_result["queue"].is_array())
        {
            ADD_FAILURE() << "get_queue returned an unexpected shape: " << queue_result.dump();
            return entries;
        }

        for (const nlohmann::json &raw : queue_result["queue"])
        {
            QueueEntry entry;
            if (!raw.is_object() || !raw.contains("id") || !raw["id"].is_number_integer())
            {
                ADD_FAILURE() << "queue entry without an integer id: " << raw.dump();
                continue;
            }

            entry.id = raw["id"].get<std::int64_t>();
            entry.score = numberField(raw, "score");
            entry.urgency_factor = numberField(raw, "urgency_factor");

            // score_parts is the evidence behind the verdict; the ranking test
            // checks the importance part against the exact expected product.
            const nlohmann::json parts =
                raw.contains("score_parts") ? raw["score_parts"] : nlohmann::json::object();
            entry.importance_part = numberField(parts, "importance");

            entries.push_back(entry);
        }

        return entries;
    }

    [[nodiscard]] static std::vector<std::int64_t> idsOf(const std::vector<QueueEntry> &entries)
    {
        std::vector<std::int64_t> ids;
        ids.reserve(entries.size());
        for (const QueueEntry &entry : entries)
        {
            ids.push_back(entry.id);
        }
        return ids;
    }

    [[nodiscard]] static bool containsId(const std::vector<QueueEntry> &entries, std::int64_t id)
    {
        for (const QueueEntry &entry : entries)
        {
            if (id == entry.id)
            {
                return true;
            }
        }
        return false;
    }

    /// The first key of the queue's total order: higher score first. A queue
    /// that violates this is wrong no matter what the weights say.
    void expectDescendingScores(const std::vector<QueueEntry> &entries)
    {
        for (std::size_t index = 1; index < entries.size(); ++index)
        {
            EXPECT_LE(entries[index].score, entries[index - 1].score)
                << "queue position " << index << " scores higher than the one before it";
        }
    }

    void setWeights(double importance, double urgency, double age, double blocks)
    {
        const nlohmann::json stored = call("set_weights",
                                           {
                                               { "importance", importance },
                                               { "urgency", urgency },
                                               { "age", age },
                                               { "blocks", blocks },
                                               { "urgency_horizon_days", 7.0 },
                                           });
        EXPECT_DOUBLE_EQ(importance, numberField(stored, "importance"));
        EXPECT_DOUBLE_EQ(urgency, numberField(stored, "urgency"));
        EXPECT_DOUBLE_EQ(age, numberField(stored, "age"));
        EXPECT_DOUBLE_EQ(blocks, numberField(stored, "blocks"));
    }

    /// Four tasks whose order differs under each of the three weight sets the
    /// ranking test uses. That difference is the point: an assertion that
    /// survives every weight change would prove only that the queue ignored
    /// the weights, so each task gets its own importance, block count, and
    /// position relative to the 7-day urgency horizon.
    struct Seeded
    {
        std::int64_t a{ 0 };
        std::int64_t b{ 0 };
        std::int64_t c{ 0 };
        std::int64_t d{ 0 };
    };

    Seeded seedTasks()
    {
        const std::int64_t now = currentEpochSeconds();

        Seeded seeded;
        // A: low importance, deadline beyond the horizon, no dependents.
        seeded.a = addTask({ { "title", "write the quarterly report" },
                             { "importance", 1 },
                             { "blocks", 0 },
                             { "due_at", now + (30 * kSecondsPerDay) } });
        // B: top importance, already overdue, no dependents.
        seeded.b = addTask({ { "title", "fix the reconnect bug" },
                             { "importance", 5 },
                             { "blocks", 0 },
                             { "due_at", now - 60 } });
        // C: mid importance, five downstream dependents, due inside the horizon.
        seeded.c = addTask({ { "title", "tidy the kline data" },
                             { "importance", 3 },
                             { "blocks", 5 },
                             { "due_at", now + (3 * kSecondsPerDay) } });
        // D: low importance and no deadline at all, which scores zero urgency
        // rather than maximum.
        seeded.d = addTask({ { "title", "answer the inbox" }, { "importance", 2 }, { "blocks", 0 } });

        EXPECT_LT(0, seeded.a);
        EXPECT_LT(0, seeded.b);
        EXPECT_LT(0, seeded.c);
        EXPECT_LT(0, seeded.d);
        return seeded;
    }

    std::int64_t addTask(const nlohmann::json &params)
    {
        const nlohmann::json created = call("add_task", params);
        if (!created.is_object() || !created.contains("id") || !created["id"].is_number_integer())
        {
            ADD_FAILURE() << "add_task did not return an id: " << created.dump();
            return 0;
        }
        return created["id"].get<std::int64_t>();
    }
};

// ---------------------------------------------------------------------------
// Wiring: the port the OS handed out, and the store the daemon actually opened.
// ---------------------------------------------------------------------------
TEST_F(DaemonRoundtripTest, ServesOnTheAssignedPortAndReportsItsOwnDatabase)
{
    // The fixture connected the client to port(); if the OS-assigned port were
    // not reported back, every call in this file would be a connection error.
    EXPECT_EQ(m_server->port(), m_client->port());

    const nlohmann::json status = call("get_status", nlohmann::json::object());
    EXPECT_EQ(std::string(kServerVersion), stringField(status, "version"));

    // Provenance check: the version and the path must describe THIS process,
    // not a compiled-in default. A daemon that opened a different database
    // would be silently writing somewhere else.
    EXPECT_EQ(m_db.path(), stringField(status, "db_path"));
    EXPECT_GE(numberField(status, "uptime_seconds"), 0.0);
}

// ---------------------------------------------------------------------------
// Ranking, driven by weights through the real socket. The three weight sets
// below produce three DIFFERENT orders from the same four tasks, which is the
// evidence that the weights actually reach the engine.
// ---------------------------------------------------------------------------
TEST_F(DaemonRoundtripTest, RankedQueueFollowsTheScoringDocument)
{
    const Seeded seeded = seedTasks();

    // --- 1. importance dominates ------------------------------------------
    // Every other weight is zero, so each score is exactly importance * 100
    // and the expectation cannot drift with the wall clock, with how long the
    // test has run, or with when the store stamped created_at.
    setWeights(100.0, 0.0, 0.0, 0.0);
    const std::vector<QueueEntry> by_importance = queueEntries(queueJson());
    ASSERT_EQ(4u, by_importance.size());
    EXPECT_EQ((std::vector<std::int64_t>{ seeded.b, seeded.c, seeded.d, seeded.a }),
              idsOf(by_importance));
    EXPECT_DOUBLE_EQ(500.0, by_importance[0].score);
    EXPECT_DOUBLE_EQ(300.0, by_importance[1].score);
    EXPECT_DOUBLE_EQ(200.0, by_importance[2].score);
    EXPECT_DOUBLE_EQ(100.0, by_importance[3].score);
    EXPECT_DOUBLE_EQ(500.0, by_importance[0].importance_part);
    expectDescendingScores(by_importance);

    // --- 2. blocks dominates ----------------------------------------------
    // C blocks five others, so it takes the front; the remaining three score
    // exactly 0, which is where scoring.md's SECOND tie-breaker shows up:
    // equal scores fall back to the older created_at and then to the lower id.
    // A, B and D were created in that order, so that is the expected order
    // whichever of the two keys ends up deciding it.
    setWeights(0.0, 0.0, 0.0, 100.0);
    const std::vector<QueueEntry> by_blocks = queueEntries(queueJson());
    ASSERT_EQ(4u, by_blocks.size());
    EXPECT_EQ((std::vector<std::int64_t>{ seeded.c, seeded.a, seeded.b, seeded.d }),
              idsOf(by_blocks));
    EXPECT_DOUBLE_EQ(500.0, by_blocks[0].score);
    EXPECT_DOUBLE_EQ(0.0, by_blocks[1].score);
    expectDescendingScores(by_blocks);

    // --- 3. urgency dominates ---------------------------------------------
    // B is overdue      -> urgencyFactor 1.0, saturated
    // C is due in 3 days -> 1 - 3/7 of the 7-day horizon (~0.5714)
    // A is due in 30 days -> beyond the horizon -> 0.0
    // D has no deadline   -> 0.0
    setWeights(0.0, 100.0, 0.0, 0.0);
    const std::vector<QueueEntry> by_urgency = queueEntries(queueJson());
    ASSERT_EQ(4u, by_urgency.size());
    EXPECT_EQ((std::vector<std::int64_t>{ seeded.b, seeded.c, seeded.a, seeded.d }),
              idsOf(by_urgency));

    EXPECT_DOUBLE_EQ(1.0, by_urgency[0].urgency_factor) << "an overdue task saturates at 1.0";
    EXPECT_NEAR(1.0 - (3.0 / 7.0), by_urgency[1].urgency_factor, 0.001);
    EXPECT_DOUBLE_EQ(0.0, by_urgency[2].urgency_factor) << "beyond the horizon is not urgent yet";
    EXPECT_DOUBLE_EQ(0.0, by_urgency[3].urgency_factor) << "no deadline is not urgent";

    EXPECT_NEAR(100.0, by_urgency[0].score, 0.01);
    // The one time-dependent tolerance in this file: the daemon samples its own
    // "now" a moment after the test computed the deadline, so C's urgency is a
    // hair above the nominal 3/7. Half a point absorbs any realistic skew and
    // still separates 42.86 from 0 and from 100.
    EXPECT_NEAR(100.0 * (1.0 - (3.0 / 7.0)), by_urgency[1].score, 0.5);
    EXPECT_DOUBLE_EQ(0.0, by_urgency[2].score);
    EXPECT_DOUBLE_EQ(0.0, by_urgency[3].score);
    expectDescendingScores(by_urgency);
}

// ---------------------------------------------------------------------------
// Lifecycle: a completed task leaves the queue, and reopening puts it back.
// ---------------------------------------------------------------------------
TEST_F(DaemonRoundtripTest, CompletingATaskTakesItOutOfTheQueueAndReopeningPutsItBack)
{
    const Seeded seeded = seedTasks();
    setWeights(100.0, 0.0, 0.0, 0.0);
    ASSERT_TRUE(containsId(queueEntries(queueJson()), seeded.b));

    const nlohmann::json completed = call("complete_task", { { "id", seeded.b } });
    EXPECT_EQ(std::string("done"), stringField(completed, "status"));
    EXPECT_FALSE(completed.value("completed_at", nlohmann::json{}).is_null())
        << "completing a task must stamp completed_at";

    // Done work is history, not a queue candidate: it disappears from the
    // queue while the survivors keep their relative order.
    const std::vector<QueueEntry> after_complete = queueEntries(queueJson());
    ASSERT_EQ(3u, after_complete.size());
    EXPECT_FALSE(containsId(after_complete, seeded.b));
    EXPECT_EQ((std::vector<std::int64_t>{ seeded.c, seeded.d, seeded.a }), idsOf(after_complete));

    const nlohmann::json reopened = call("reopen_task", { { "id", seeded.b } });
    EXPECT_EQ(std::string("open"), stringField(reopened, "status"));
    EXPECT_TRUE(reopened.value("completed_at", nlohmann::json{}).is_null())
        << "reopening a task must clear completed_at";

    const std::vector<QueueEntry> after_reopen = queueEntries(queueJson());
    ASSERT_EQ(4u, after_reopen.size());
    EXPECT_EQ((std::vector<std::int64_t>{ seeded.b, seeded.c, seeded.d, seeded.a }),
              idsOf(after_reopen));
}

// ---------------------------------------------------------------------------
// Failure paths: application errors are structured errors on the wire, and the
// -32001 mapping survives the trip out and back.
// ---------------------------------------------------------------------------
TEST_F(DaemonRoundtripTest, FailurePathsArriveAsStructuredErrorsOverTheSocket)
{
    const Seeded seeded = seedTasks();

    const nlohmann::json deleted = call("delete_task", { { "id", seeded.a } });
    EXPECT_TRUE(deleted.value("deleted", false));
    EXPECT_EQ(seeded.a, deleted.value("id", static_cast<std::int64_t>(-1)));

    // 1. The deleted task is gone, and the failure is a domain error with the
    //    documented wire code — not a socket failure and not a crash.
    const RpcResult missing = m_client->call("get_task", { { "id", seeded.a } });
    ASSERT_FALSE(missing.ok()) << "get_task on a deleted id must fail";
    EXPECT_EQ(-32001, rpcErrorCode(missing.error().code));
    expectErrorCode(ErrorCode::kNotFound, missing.error());

    // 2. Deleting twice is an error rather than a second "success": a delete
    //    that reports success on a missing row hides double-delete bugs.
    const RpcResult second_delete = m_client->call("delete_task", { { "id", seeded.a } });
    ASSERT_FALSE(second_delete.ok());
    expectErrorCode(ErrorCode::kNotFound, second_delete.error());

    // 3. An id that never existed takes the same path end to end.
    const RpcResult never_existed = m_client->call("get_task", { { "id", 999999 } });
    ASSERT_FALSE(never_existed.ok());
    EXPECT_EQ(-32001, rpcErrorCode(never_existed.error().code));
    expectErrorCode(ErrorCode::kNotFound, never_existed.error());

    // 4. A rejected write leaves the store untouched, and the session survives
    //    it: one bad call must not poison the connection.
    const RpcResult rejected = m_client->call("update_task", { { "id", seeded.b },
                                                               { "importance", 99 } });
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(-32602, rpcErrorCode(rejected.error().code));
    expectErrorCode(ErrorCode::kInvalidArgument, rejected.error());

    const nlohmann::json survivor = call("get_task", { { "id", seeded.b } });
    EXPECT_EQ(seeded.b, survivor.value("id", static_cast<std::int64_t>(-1)));
    EXPECT_EQ(5.0, numberField(survivor, "importance")) << "the rejected update must not have applied";
}

// ---------------------------------------------------------------------------
// Settings: the retuning path documented in docs/scoring.md, over the socket.
// ---------------------------------------------------------------------------
TEST_F(DaemonRoundtripTest, WeightUpdatesMergeOntoTheStoredWeights)
{
    const nlohmann::json defaults = call("get_weights", nlohmann::json::object());
    EXPECT_DOUBLE_EQ(5.0, numberField(defaults, "importance"));
    EXPECT_DOUBLE_EQ(30.0, numberField(defaults, "urgency"));
    EXPECT_DOUBLE_EQ(2.0, numberField(defaults, "age"));
    EXPECT_DOUBLE_EQ(8.0, numberField(defaults, "blocks"));
    EXPECT_DOUBLE_EQ(7.0, numberField(defaults, "urgency_horizon_days"));

    // A partial update merges: only urgency moves.
    const nlohmann::json updated = call("set_weights", { { "urgency", 0.0 } });
    EXPECT_DOUBLE_EQ(0.0, numberField(updated, "urgency"));
    EXPECT_DOUBLE_EQ(5.0, numberField(updated, "importance"));
    EXPECT_DOUBLE_EQ(2.0, numberField(updated, "age"));
    EXPECT_DOUBLE_EQ(8.0, numberField(updated, "blocks"));
    EXPECT_DOUBLE_EQ(7.0, numberField(updated, "urgency_horizon_days"));

    // The change is store-level, not connection-level: a fresh connection sees
    // it, which is what makes it survive a daemon restart.
    const nlohmann::json reread = call("get_weights", nlohmann::json::object());
    EXPECT_DOUBLE_EQ(0.0, numberField(reread, "urgency"));

    // An invalid weight is rejected rather than silently clamped or ignored:
    // a weight change that quietly did nothing is indistinguishable from one
    // that failed, and the user would tune the queue for an hour wondering why
    // nothing moved.
    const RpcResult rejected = m_client->call("set_weights", { { "urgency", -1.0 } });
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(-32602, rpcErrorCode(rejected.error().code));
    expectErrorCode(ErrorCode::kInvalidArgument, rejected.error());

    const nlohmann::json after_reject = call("get_weights", nlohmann::json::object());
    EXPECT_DOUBLE_EQ(0.0, numberField(after_reject, "urgency")) << "the stored weights must be intact";
    EXPECT_DOUBLE_EQ(5.0, numberField(after_reject, "importance"));
}

} // namespace
} // namespace taskpilot
