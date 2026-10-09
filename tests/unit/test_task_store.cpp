// test_task_store.cpp — TaskStore against a real (in-memory) SQLite database
//
// Every case runs on ":memory:" except the durability case, which needs a real
// file and gets one in a self-removing temp directory; the migration cases
// likewise need a file, because they build an old database with raw SQL and
// then reopen it through the store. No network, no fixed port, no shared state
// between cases: each TEST opens the store it uses.
//
// The store samples its own clock (see TaskStore.cpp), so timestamp assertions
// are written as brackets and monotonicity claims rather than equalities to a
// fixed instant. The one equality asserted is between two columns the store
// fills from a single sample.
//
// The synchronisation cases at the end are the ones that matter most, because
// a mistake in them loses work rather than failing loudly: the merge round trip
// pins that two machines whose local ids both start at 1 still end up holding
// all of both backlogs, and the dry-run case pins that a dry run's report is
// the report of the merge that follows it.

#include "core/TaskStore.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "core/Clock.hpp"
#include "core/Result.hpp"
#include "core/Task.hpp"
#include "core/TaskSync.hpp"
#include "core/Uuid.hpp"
#include "core/Weights.hpp"

namespace taskpilot
{
namespace
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// A scratch directory that deletes itself, so a failing durability test does
/// not leave a database behind for the next run to trip over.
class TempDir
{
  public:
    TempDir()
        : m_path{ std::filesystem::temp_directory_path() /
                  ("taskpilot_store_" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) }
    {
        std::filesystem::create_directories(m_path);
    }

    ~TempDir()
    {
        // An error_code overload: a destructor must not throw, and a leftover
        // temp directory is not worth failing a test run over.
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }

    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    [[nodiscard]] std::string file(const std::string &name) const
    {
        return (m_path / name).string();
    }

  private:
    std::filesystem::path m_path;
};

/// Wall clock, used only to bracket what the store stamps.
[[nodiscard]] std::int64_t systemNow()
{
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(since_epoch).count());
}

/// Open a store, recording a failure instead of crashing on a bad Result.
[[nodiscard]] std::unique_ptr<TaskStore> openStore(const std::string &path)
{
    Result<std::unique_ptr<TaskStore>> opened = TaskStore::open(path);
    EXPECT_TRUE(opened.ok()) << "open(" << path
                             << ") failed: " << (opened.ok() ? std::string{} : opened.error().message);
    if (!opened.ok()) {
        return nullptr;
    }
    return std::move(opened).value();
}

/// Unwrap a successful Result, failing the current test and using `fallback`
/// when it holds an Error. ASSERT_* cannot be used in a helper that returns a
/// value, so the fallback is what keeps the caller safe to continue.
template <typename T>
T okValue(const Result<T> &result, T fallback, const char *what)
{
    EXPECT_TRUE(result.ok()) << what
                             << " failed: " << (result.ok() ? std::string{} : result.error().message);
    if (!result.ok()) {
        return std::move(fallback);
    }
    return result.value();
}

/// Assert a Status succeeded, quoting the message when it did not.
void expectOk(const Status &status, const char *what)
{
    EXPECT_TRUE(status.ok()) << what
                             << " failed: " << (status.ok() ? std::string{} : status.error().message);
}

[[nodiscard]] Task draftTask(const std::string &title, int importance = 3)
{
    Task task;
    task.title = title;
    task.importance = importance;
    return task;
}

/// Add a task whose status and deadline matter for the caller's assertions.
[[nodiscard]] Result<Task> addWith(TaskStore &store, const std::string &title, TaskStatus status,
                                   std::optional<std::int64_t> due_at)
{
    Task draft = draftTask(title);
    draft.status = status;
    draft.due_at = due_at;
    return store.addTask(draft);
}

// ---------------------------------------------------------------------------
// Export-format helpers
// ---------------------------------------------------------------------------
//
// A merge test has to name a uid before either store has seen it, and has to
// choose the timestamps the store would otherwise assign — last-write-wins is
// undecidable if both sides are stamped "now". So these build records in the
// format TaskSync.hpp documents, from values the test controls.
//
// The shape is deliberately spelled out here instead of reusing a serializer:
// a test that built its records with the code under test's own encoder could
// not tell a format change from a matching pair of bugs.

/// A fresh v4 uuid, for the records no store has created.
[[nodiscard]] std::string newUid()
{
    const Result<std::string> generated = generateUuidV4();
    EXPECT_TRUE(generated.ok()) << "generateUuidV4 failed: "
                                << (generated.ok() ? std::string{} : generated.error().message);
    return generated.ok() ? generated.value() : std::string{};
}

/// One live record, compactly dumped so it is a single line.
[[nodiscard]] std::string liveRecord(const std::string &uid, const std::string &title,
                                     std::int64_t created_at, std::int64_t updated_at,
                                     const std::string &status = "open", int importance = 3)
{
    const nlohmann::json record{
        { "v", 1 },
        { "uid", uid },
        { "title", title },
        { "notes", "" },
        { "status", status },
        { "importance", importance },
        { "due_at", nullptr },
        { "blocks", 0 },
        { "tags", nlohmann::json::array() },
        { "created_at", created_at },
        { "updated_at", updated_at },
        { "completed_at", nullptr },
        { "deleted", false },
    };
    return record.dump();
}

/// One tombstone record.
[[nodiscard]] std::string tombstoneRecord(const std::string &uid, std::int64_t deleted_at)
{
    const nlohmann::json record{
        { "v", 1 },
        { "uid", uid },
        { "updated_at", deleted_at },
        { "deleted", true },
    };
    return record.dump();
}

/// Everything a store holds, ordered by uid so two stores' contents can be
/// compared without depending on row order (which listTasks deliberately does
/// not define).
[[nodiscard]] std::vector<Task> tasksByUid(TaskStore &store)
{
    std::vector<Task> tasks =
        okValue(store.listTasks(TaskFilter{}), std::vector<Task>{}, "listTasks");
    std::sort(tasks.begin(), tasks.end(),
              [](const Task &lhs, const Task &rhs) { return lhs.uid < rhs.uid; });
    return tasks;
}

/// The uids a store holds, as a set — the shape most merge assertions want.
[[nodiscard]] std::set<std::string> uidSet(TaskStore &store)
{
    std::set<std::string> uids;
    for (const Task &task : tasksByUid(store)) {
        uids.insert(task.uid);
    }
    return uids;
}

/// The local ids a store holds, as a set. Used to assert the ids stay unique:
/// two machines' backlogs both start at 1, and a merge that matched on id
/// instead of uid would collapse them into one row.
[[nodiscard]] std::set<std::int64_t> idSet(TaskStore &store)
{
    std::set<std::int64_t> ids;
    for (const Task &task : tasksByUid(store)) {
        ids.insert(task.id);
    }
    return ids;
}

/// Merge one store's whole export into another, failing the test on either
/// side's error. The shape every sync case starts from.
[[nodiscard]] MergeReport mergeFrom(TaskStore &from, TaskStore &to, bool dry_run = false)
{
    const std::string exported = okValue(from.exportJsonl(), std::string{}, "exportJsonl");
    return okValue(to.mergeJsonl(exported, dry_run), MergeReport{}, "mergeJsonl");
}

/// The titles a store holds, as a set: for asserting that a merge added
/// everything and overwrote nothing.
[[nodiscard]] std::set<std::string> titleSet(TaskStore &store)
{
    std::set<std::string> titles;
    for (const Task &task : tasksByUid(store)) {
        titles.insert(task.title);
    }
    return titles;
}

/// Split an export into its lines. The export is newline-terminated, so the
/// final element after the last newline is empty and is dropped here — the
/// tests that care about that byte assert on it directly.
[[nodiscard]] std::vector<std::string> splitLines(const std::string &text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        if (std::string::npos == end) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

// ---------------------------------------------------------------------------
// Raw column access
// ---------------------------------------------------------------------------
//
// The store never writes a non-integer timestamp or an unknown status, so the
// only way to exercise how such a row is READ is to write it behind the store's
// back. These helpers do that through a second connection to the same file —
// the same shape a hand-edited or third-party-written database has.
//
// That also forces those cases onto a file rather than ":memory:": every
// connection to an in-memory database gets its own private one.

/// Open a second connection to the database file at `path`, or nullptr with a
/// recorded failure when sqlite refuses.
///
/// `create` opens (and creates) the file. Only the version-0 builder needs it:
/// it is the first thing to touch its database, while every other caller is
/// reading a file the store already made.
[[nodiscard]] sqlite3 *openRawConnection(const std::string &path, bool create = false)
{
    const int flags = create ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) : SQLITE_OPEN_READWRITE;
    sqlite3 *db = nullptr;
    if (SQLITE_OK != sqlite3_open_v2(path.c_str(), &db, flags, nullptr)) {
        const std::string detail = (nullptr != db) ? sqlite3_errmsg(db) : "out of memory";
        sqlite3_close(db);
        ADD_FAILURE() << "raw open of " << path << " failed: " << detail;
        return nullptr;
    }
    // The store holds no long-lived transaction, but a writer may be mid-statement;
    // waiting is friendlier than failing a test with SQLITE_BUSY.
    sqlite3_busy_timeout(db, 5000);
    return db;
}

/// Overwrite one column of one row with a TEXT value. `column` is always a
/// literal from this file, never data, so interpolating it is not the injection
/// risk that binding values exists to prevent.
///
/// Returns the number of rows changed, and every caller asserts on it: a
/// silently failed UPDATE would leave the original value in place and make a
/// "reads back as absent" expectation pass for the wrong reason.
[[nodiscard]] int overwriteColumnRaw(const std::string &path, std::int64_t id,
                                     const std::string &column, const std::string &value)
{
    sqlite3 *db = openRawConnection(path);
    if (nullptr == db) {
        return 0;
    }

    sqlite3_stmt *stmt = nullptr;
    const std::string sql = "UPDATE tasks SET " + column + " = ? WHERE id = ?";
    if (SQLITE_OK != sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr)) {
        ADD_FAILURE() << "raw prepare failed for " << column << ": " << sqlite3_errmsg(db);
        sqlite3_close(db);
        return 0;
    }

    const int length = static_cast<int>(value.size());
    const bool bound = SQLITE_OK == sqlite3_bind_text(stmt, 1, value.c_str(), length, SQLITE_TRANSIENT) &&
                       SQLITE_OK == sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(id));
    if (!bound) {
        ADD_FAILURE() << "raw bind failed for " << column << ": " << sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return 0;
    }

    if (SQLITE_DONE != sqlite3_step(stmt)) {
        ADD_FAILURE() << "raw update of " << column << " failed: " << sqlite3_errmsg(db);
    }
    const int changed = sqlite3_changes(db);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return changed;
}

/// The storage class sqlite reports for one cell ("integer", "text", "null",
/// "blob", "real"). The tests assert on it so a planted value cannot satisfy an
/// "it reads back as absent" check merely by having landed as NULL.
[[nodiscard]] std::string storageClassRaw(const std::string &path, std::int64_t id,
                                          const std::string &column)
{
    sqlite3 *db = openRawConnection(path);
    if (nullptr == db) {
        return std::string{};
    }

    sqlite3_stmt *stmt = nullptr;
    const std::string sql = "SELECT typeof(" + column + ") FROM tasks WHERE id = ?";
    if (SQLITE_OK != sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr)) {
        ADD_FAILURE() << "raw prepare failed for typeof(" << column << "): " << sqlite3_errmsg(db);
        sqlite3_close(db);
        return std::string{};
    }
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(id));

    std::string storage_class;
    if (SQLITE_ROW == sqlite3_step(stmt)) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (nullptr != text) {
            storage_class = reinterpret_cast<const char *>(text);
        }
    } else {
        ADD_FAILURE() << "no task " << id << " while reading typeof(" << column << ")";
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return storage_class;
}

/// Run one statement on a fresh raw connection and return sqlite's result code.
/// `sql` is a literal from this file, never data: binding is how values travel,
/// even in a test.
[[nodiscard]] int runRaw(const std::string &path, const std::string &sql, bool create = false)
{
    sqlite3 *db = openRawConnection(path, create);
    if (nullptr == db) {
        return SQLITE_ERROR;
    }
    char *message = nullptr;
    const int result = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message);
    sqlite3_free(message);
    sqlite3_close(db);
    return result;
}

/// Read one TEXT value from the raw database. `sql` is a literal from this
/// file, and the query is expected to return exactly one row with one column —
/// anything else is recorded as a failure, because a missing row would
/// otherwise make an assertion pass for the wrong reason.
[[nodiscard]] std::string rawScalarText(const std::string &path, const std::string &sql)
{
    sqlite3 *db = openRawConnection(path);
    if (nullptr == db) {
        return std::string{};
    }
    sqlite3_stmt *stmt = nullptr;
    if (SQLITE_OK != sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr)) {
        ADD_FAILURE() << "raw prepare failed (" << sqlite3_errmsg(db) << "): " << sql;
        sqlite3_close(db);
        return std::string{};
    }
    std::string value;
    if (SQLITE_ROW == sqlite3_step(stmt)) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (nullptr != text) {
            value = reinterpret_cast<const char *>(text);
        }
    } else {
        ADD_FAILURE() << "raw query returned no row: " << sql;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return value;
}

/// Build the database a build from before `uid` existed left behind: the old
/// tasks table (no uid column, no tombstones table) and two rows with data
/// worth not losing.
///
/// The settings row is the interesting part. That build wrote
/// schema_version='1' without ever reading it — the constant lived in
/// TaskStore.cpp with a comment saying nothing read it yet — so the database
/// that needs migrating is one that CLAIMS to be current while having no uid
/// column at all. A migration keyed on that row skips exactly the database it
/// exists for, which is why the test asserts this row is present before it
/// opens the store.
///
/// Returns true when every statement applied.
[[nodiscard]] bool buildVersionZeroDatabase(const std::string &path)
{
    const std::string schema = R"sql(
CREATE TABLE tasks (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    title        TEXT    NOT NULL,
    notes        TEXT    NOT NULL DEFAULT '',
    status       TEXT    NOT NULL DEFAULT 'open',
    importance   INTEGER NOT NULL DEFAULT 3,
    due_at       INTEGER,
    blocks       INTEGER NOT NULL DEFAULT 0,
    tags         TEXT    NOT NULL DEFAULT '[]',
    created_at   INTEGER NOT NULL,
    updated_at   INTEGER NOT NULL,
    completed_at INTEGER
);
CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
INSERT INTO settings(key, value) VALUES('schema_version', '1');
INSERT INTO tasks(title, notes, status, importance, due_at, blocks, tags,
                  created_at, updated_at, completed_at)
VALUES('legacy one', 'written before uids existed', 'in_progress', 4, 1700000000, 2, '["work"]',
       1600000000, 1600000500, NULL),
      ('legacy two', '', 'done', 2, NULL, 0, '[]',
       1600000100, 1600000200, 1600000300);
)sql";
    // create: this helper is the first thing to touch the file, exactly as the
    // old build was the first to touch its own database.
    return SQLITE_OK == runRaw(path, schema, true);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST(TaskStoreTest, OpenLeavesAUsableStore)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);
    EXPECT_EQ(":memory:", store->path());

    // A fresh store is empty, not broken: both read paths answer without an
    // error and report zero.
    const TaskFilter filter;
    const std::vector<Task> tasks = okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    EXPECT_TRUE(tasks.empty());

    const Stats stats = okValue(store->stats(1'700'000'000), Stats{}, "stats");
    EXPECT_EQ(0U, stats.open);
    EXPECT_EQ(0U, stats.in_progress);
    EXPECT_EQ(0U, stats.done);
    EXPECT_EQ(0U, stats.archived);
    EXPECT_EQ(0U, stats.overdue);
    EXPECT_EQ(0U, stats.due_within_24h);
}

TEST(TaskStoreTest, FileBackedStoreKeepsItsDataAcrossReopen)
{
    const TempDir directory;
    const std::string path = directory.file("tasks.db");

    std::int64_t task_id = 0;
    {
        const std::unique_ptr<TaskStore> store = openStore(path);
        ASSERT_NE(nullptr, store);
        const Task stored =
            okValue(store->addTask(draftTask("survive a restart", 5)), Task{}, "addTask");
        task_id = stored.id;

        Weights tuned;
        tuned.importance = 9.0;
        tuned.urgency_horizon_days = 3.5;
        expectOk(store->setWeights(tuned), "setWeights");
    } // The store is closed here; that block is the durability claim.

    const std::unique_ptr<TaskStore> reopened = openStore(path);
    ASSERT_NE(nullptr, reopened);

    const Task fetched = okValue(reopened->getTask(task_id), Task{}, "getTask");
    EXPECT_EQ("survive a restart", fetched.title);
    EXPECT_EQ(5, fetched.importance);

    // Reopening must not re-seed: the tuned weights are the point of storing
    // them, and overwriting them on every start would be invisible from here.
    const Weights weights = okValue(reopened->weights(), Weights{}, "weights");
    EXPECT_DOUBLE_EQ(9.0, weights.importance);
    EXPECT_DOUBLE_EQ(3.5, weights.urgency_horizon_days);
}

// ---------------------------------------------------------------------------
// The version-0 migration
// ---------------------------------------------------------------------------
//
// This is the real migration, not a hypothetical: the database in the field
// has the old table shape and a schema_version row that says the same thing
// this build does. Everything that can go wrong here loses the user's backlog,
// so the cases below check the data, the identity, the new table, the version
// and the idempotence, in that order.

TEST(TaskStoreTest, OpenMigratesAVersionZeroDatabaseInPlace)
{
    const TempDir directory;
    const std::string path = directory.file("legacy.db");
    ASSERT_TRUE(buildVersionZeroDatabase(path)) << "could not plant the old database";

    // Before: no uid column, and a settings row claiming version 1. The
    // COALESCE keeps this a one-row query, so a missing column reads as
    // 'absent' rather than as a failed query.
    EXPECT_EQ("absent",
              rawScalarText(path, "SELECT COALESCE((SELECT name FROM pragma_table_info('tasks') "
                                  "WHERE name = 'uid'), 'absent')"));
    EXPECT_EQ("1", rawScalarText(path, "SELECT value FROM settings WHERE key = 'schema_version'"));

    std::int64_t first_id = 0;
    std::string first_uid;
    {
        const std::unique_ptr<TaskStore> migrated = openStore(path);
        ASSERT_NE(nullptr, migrated);

        // Row 1 kept every field it had, byte for byte. The ids are still 1 and
        // 2: adding a column and backfilling it must not renumber rows, or
        // every id a client is holding silently points at another task.
        const Task one = okValue(migrated->getTask(1), Task{}, "getTask");
        EXPECT_EQ("legacy one", one.title);
        EXPECT_EQ("written before uids existed", one.notes);
        EXPECT_EQ(TaskStatus::kInProgress, one.status);
        EXPECT_EQ(4, one.importance);
        ASSERT_TRUE(one.due_at.has_value());
        EXPECT_EQ(1700000000, *one.due_at);
        EXPECT_EQ(2, one.blocks);
        EXPECT_EQ(std::vector<std::string>({ "work" }), one.tags);
        EXPECT_EQ(1600000000, one.created_at);
        EXPECT_EQ(1600000500, one.updated_at);

        const Task two = okValue(migrated->getTask(2), Task{}, "getTask");
        EXPECT_EQ("legacy two", two.title);
        EXPECT_EQ(TaskStatus::kDone, two.status);
        EXPECT_EQ(2, two.importance);
        EXPECT_FALSE(two.due_at.has_value());
        ASSERT_TRUE(two.completed_at.has_value());
        EXPECT_EQ(1600000300, *two.completed_at);

        // And gained the identity the feature is about: a well-formed v4 uuid,
        // one per row. A shared or missing uid would leave two machines unable
        // to tell these tasks apart — the exact collision uid exists to stop.
        EXPECT_TRUE(looksLikeUuidV4(one.uid)) << "migrated uid is not a v4 uuid: " << one.uid;
        EXPECT_TRUE(looksLikeUuidV4(two.uid)) << "migrated uid is not a v4 uuid: " << two.uid;
        EXPECT_NE(one.uid, two.uid);

        // The tombstones table exists (reading it is how that is proven: a
        // missing table fails the query) and starts empty, because nothing has
        // been deleted yet.
        EXPECT_EQ(0U, okValue(migrated->tombstoneCount(), std::size_t{0}, "tombstoneCount"));

        // The rows are findable by the new identity, not only by id.
        const Task by_uid = okValue(migrated->getTaskByUid(one.uid), Task{}, "getTaskByUid");
        EXPECT_EQ(one.id, by_uid.id);
        EXPECT_EQ("legacy one", by_uid.title);

        first_id = one.id;
        first_uid = one.uid;
    }

    // The version row now describes a database that really is at version 1.
    EXPECT_EQ("1", rawScalarText(path, "SELECT value FROM settings WHERE key = 'schema_version'"));

    // Reopening a migrated database changes NOTHING: same uid, same rows, same
    // tombstone count. This is what makes it safe to run the migration on every
    // open — a second pass that re-assigned uids would make every task a
    // stranger to the other machine, and the next merge would duplicate the
    // whole backlog.
    {
        const std::unique_ptr<TaskStore> again = openStore(path);
        ASSERT_NE(nullptr, again);

        const Task one = okValue(again->getTask(first_id), Task{}, "getTask");
        EXPECT_EQ(first_uid, one.uid);
        EXPECT_EQ("legacy one", one.title);
        EXPECT_EQ(2U, tasksByUid(*again).size());
        EXPECT_EQ(0U, okValue(again->tombstoneCount(), std::size_t{0}, "tombstoneCount"));
    }
}

TEST(TaskStoreTest, MigratedDatabaseEnforcesTheUniqueUidIndex)
{
    const TempDir directory;
    const std::string path = directory.file("unique_uid.db");
    ASSERT_TRUE(buildVersionZeroDatabase(path));

    const std::unique_ptr<TaskStore> store = openStore(path);
    ASSERT_NE(nullptr, store);
    const Task first = okValue(store->getTask(1), Task{}, "getTask");

    // The invariant is the database's, not a convention: a second row claiming
    // the first row's uid is refused by the index. Without it a hand-written or
    // restored row could duplicate an identity, and a merge cannot resolve two
    // rows with one uid — it would pick one and destroy the other.
    const std::string impostor =
        "INSERT INTO tasks(uid, title, created_at, updated_at) VALUES('" + first.uid +
        "', 'impostor', 1, 1)";
    EXPECT_EQ(SQLITE_CONSTRAINT, runRaw(path, impostor));

    // The refusal changed nothing: still two rows, still no impostor.
    EXPECT_EQ(2U, tasksByUid(*store).size());
    EXPECT_EQ(2U, uidSet(*store).size());
}

// ---------------------------------------------------------------------------
// addTask
// ---------------------------------------------------------------------------

TEST(TaskStoreTest, AddTaskAssignsIdentityAndTimestamps)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    Task draft = draftTask("pay the invoice");
    // A client that writes its own history is ignored: these columns come from
    // the store and nowhere else.
    draft.id = 999;
    draft.created_at = 1;
    draft.updated_at = 1;
    draft.completed_at = 1;

    const std::int64_t before = systemNow();
    const Task stored = okValue(store->addTask(draft), Task{}, "addTask");
    const std::int64_t after = systemNow();

    EXPECT_NE(999, stored.id);
    EXPECT_LT(0, stored.id);
    EXPECT_GE(stored.created_at, before);
    EXPECT_LE(stored.created_at, after);
    // One clock sample fills both columns on insert; claiming otherwise would
    // make a brand-new task look already modified.
    EXPECT_EQ(stored.created_at, stored.updated_at);
    EXPECT_FALSE(stored.completed_at.has_value());
    EXPECT_TRUE(stored.isActionable());
}

TEST(TaskStoreTest, AddTaskAssignsAUidAndOverwritesTheCallers)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    Task draft = draftTask("identity comes from the store");
    // A client-chosen uid is discarded exactly like a client-chosen id. It is
    // the more dangerous of the two: a forged uid can collide with a task
    // arriving from another machine, and the collision destroys one of the two
    // at the next merge — silently, and on the other machine.
    draft.uid = "00000000-0000-4000-8000-000000000000";

    const Task stored = okValue(store->addTask(draft), Task{}, "addTask");
    EXPECT_TRUE(looksLikeUuidV4(stored.uid)) << "uid is not a v4 uuid: " << stored.uid;
    EXPECT_NE(draft.uid, stored.uid);

    // The uid belongs to the row, not merely to the returned copy.
    const Task fetched = okValue(store->getTask(stored.id), Task{}, "getTask");
    EXPECT_EQ(stored.uid, fetched.uid);
    const Task by_uid = okValue(store->getTaskByUid(stored.uid), Task{}, "getTaskByUid");
    EXPECT_EQ(stored.id, by_uid.id);
    EXPECT_EQ(stored.title, by_uid.title);

    // The forged uid was not stored as an alias for the row.
    const Result<Task> forged = store->getTaskByUid(draft.uid);
    ASSERT_FALSE(forged.ok());
    EXPECT_EQ(ErrorCode::kNotFound, forged.error().code);

    // Two tasks created here never share an identity: the uniqueness the merge
    // relies on has to come from generation, not from luck.
    const Task second = okValue(store->addTask(draftTask("the other one")), Task{}, "addTask");
    EXPECT_TRUE(looksLikeUuidV4(second.uid));
    EXPECT_NE(stored.uid, second.uid);
}

TEST(TaskStoreTest, AddTaskRejectsBadFieldsAndNamesThem)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    std::vector<std::pair<std::string, Task>> cases;

    cases.emplace_back("title", draftTask(""));
    cases.emplace_back("title", draftTask("  \t "));

    Task low = draftTask("importance too low");
    low.importance = 0;
    cases.emplace_back("importance", low);

    Task high = draftTask("importance too high");
    high.importance = 6;
    cases.emplace_back("importance", high);

    Task negative = draftTask("negative blocks");
    negative.blocks = -1;
    cases.emplace_back("blocks", negative);

    Task empty_tag = draftTask("empty tag");
    empty_tag.tags = { "kept", "" };
    cases.emplace_back("tags", empty_tag);

    Task blank_tag = draftTask("whitespace tag");
    blank_tag.tags = { "   " };
    cases.emplace_back("tags", blank_tag);

    for (const auto &[field, draft] : cases) {
        const Result<Task> added = store->addTask(draft);
        ASSERT_FALSE(added.ok()) << "expected a rejection for field " << field;
        EXPECT_EQ(ErrorCode::kInvalidArgument, added.error().code) << "field " << field;
        EXPECT_NE(std::string::npos, added.error().message.find(field))
            << "message does not name the field: " << added.error().message;
    }

    // No rejected call wrote a partial row.
    const TaskFilter filter;
    const std::vector<Task> tasks = okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    EXPECT_TRUE(tasks.empty());
}

TEST(TaskStoreTest, RoundTripsEveryField)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    Task draft = draftTask("ship the release notes", 4);
    draft.notes = "include the migration section\nand a worked example";
    draft.status = TaskStatus::kInProgress;
    draft.due_at = 1'760'000'000;
    draft.blocks = 3;
    draft.tags = { "work", "docs", "deep work" };

    const Task stored = okValue(store->addTask(draft), Task{}, "addTask");
    const Task fetched = okValue(store->getTask(stored.id), Task{}, "getTask");

    EXPECT_EQ(stored.id, fetched.id);
    EXPECT_EQ("ship the release notes", fetched.title);
    EXPECT_EQ(draft.notes, fetched.notes);
    EXPECT_EQ(TaskStatus::kInProgress, fetched.status);
    EXPECT_EQ(4, fetched.importance);
    ASSERT_TRUE(fetched.due_at.has_value());
    EXPECT_EQ(1'760'000'000, *fetched.due_at);
    EXPECT_EQ(3, fetched.blocks);
    EXPECT_EQ(draft.tags, fetched.tags);
    EXPECT_EQ(stored.created_at, fetched.created_at);
    EXPECT_EQ(stored.updated_at, fetched.updated_at);
    EXPECT_FALSE(fetched.completed_at.has_value());

    // An unset deadline stays unset through the nullable column, and the
    // empty-string defaults come back as empty rather than as NULL.
    const Task undated = okValue(store->addTask(draftTask("someday maybe")), Task{}, "addTask");
    const Task reread = okValue(store->getTask(undated.id), Task{}, "getTask");
    EXPECT_FALSE(reread.due_at.has_value());
    EXPECT_TRUE(reread.tags.empty());
    EXPECT_TRUE(reread.notes.empty());
    EXPECT_EQ(TaskStatus::kOpen, reread.status);
    EXPECT_EQ(3, reread.importance);
    EXPECT_EQ(0, reread.blocks);
}

TEST(TaskStoreTest, QuoteAndSemicolonInTitleRoundTripsByteIdentical)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    // The shape of an injection attempt, stored as data. This is the test that
    // fails if anyone ever replaces parameter binding with string
    // concatenation, which is why it asserts byte equality and not just
    // presence.
    const std::string hostile = "O'Brien's notes; DROP TABLE tasks; -- and 'more'";
    const Task stored = okValue(store->addTask(draftTask(hostile)), Task{}, "addTask");
    const Task fetched = okValue(store->getTask(stored.id), Task{}, "getTask");
    EXPECT_EQ(hostile, fetched.title);

    // The table is still there and still usable rather than silently emptied.
    const Task second = okValue(store->addTask(draftTask("still alive")), Task{}, "addTask");
    const TaskFilter filter;
    const std::vector<Task> tasks = okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    EXPECT_EQ(2U, tasks.size());

    // The same input through the UPDATE path, which assembles its SQL at
    // runtime and is therefore the riskier of the two.
    TaskPatch patch;
    patch.title = hostile + " (edited)";
    patch.notes = hostile;
    const Task updated = okValue(store->updateTask(second.id, patch), Task{}, "updateTask");
    EXPECT_EQ(hostile + " (edited)", updated.title);
    EXPECT_EQ(hostile, updated.notes);
}

// ---------------------------------------------------------------------------
// listTasks
// ---------------------------------------------------------------------------

TEST(TaskStoreTest, ListTasksFiltersByStatus)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    const Result<Task> first = addWith(*store, "open one", TaskStatus::kOpen, std::nullopt);
    ASSERT_TRUE(first.ok());
    const Result<Task> second = addWith(*store, "in flight", TaskStatus::kInProgress, std::nullopt);
    ASSERT_TRUE(second.ok());
    const Result<Task> third = addWith(*store, "parked", TaskStatus::kArchived, std::nullopt);
    ASSERT_TRUE(third.ok());

    TaskFilter filter;
    filter.status = TaskStatus::kOpen;
    const std::vector<Task> open_tasks = okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    ASSERT_EQ(1U, open_tasks.size());
    EXPECT_EQ(first.value().id, open_tasks.front().id);

    filter.status = TaskStatus::kArchived;
    const std::vector<Task> archived =
        okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    ASSERT_EQ(1U, archived.size());
    EXPECT_EQ(third.value().id, archived.front().id);

    // No status filter means every status, including the ones the queue drops.
    const TaskFilter everything;
    const std::vector<Task> all = okValue(store->listTasks(everything), std::vector<Task>{}, "listTasks");
    EXPECT_EQ(3U, all.size());
}

TEST(TaskStoreTest, ListTasksFiltersByTag)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    Task tagged = draftTask("tagged item");
    tagged.tags = { "work", "deep work" };
    const Task a = okValue(store->addTask(tagged), Task{}, "addTask");

    Task other = draftTask("other item");
    other.tags = { "home" };
    const Task b = okValue(store->addTask(other), Task{}, "addTask");

    const Task untagged = okValue(store->addTask(draftTask("untagged item")), Task{}, "addTask");
    EXPECT_TRUE(untagged.tags.empty());

    TaskFilter filter;
    filter.tag = "home";
    const std::vector<Task> home = okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    ASSERT_EQ(1U, home.size());
    EXPECT_EQ(b.id, home.front().id);

    // The filter matches a whole tag, not a prefix of one.
    filter.tag = "wor";
    const std::vector<Task> prefix = okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    EXPECT_TRUE(prefix.empty());

    // A tag containing a space is one tag, not two.
    filter.tag = "deep work";
    const std::vector<Task> spaced = okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    ASSERT_EQ(1U, spaced.size());
    EXPECT_EQ(a.id, spaced.front().id);

    // A tag nobody carries matches nothing rather than everything.
    filter.tag = "missing";
    const std::vector<Task> absent = okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    EXPECT_TRUE(absent.empty());
}

TEST(TaskStoreTest, ListTasksTagFilterMatchesWholeElementsOnly)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    Task two = draftTask("two tags");
    two.tags = { "a", "b" };
    const Task ab = okValue(store->addTask(two), Task{}, "addTask");

    Task three = draftTask("three tags");
    three.tags = { "x", "y", "z" };
    const Task xyz = okValue(store->addTask(three), Task{}, "addTask");

    Task one = draftTask("one tag");
    one.tags = { "solo" };
    const Task solo = okValue(store->addTask(one), Task{}, "addTask");

    // A comma inside a tag is one tag, not a separator: this row stores
    // ["a,b"], which a substring search cannot tell apart from ["a","b"].
    Task comma = draftTask("comma inside the tag");
    comma.tags = { "a,b" };
    const Task comma_tag = okValue(store->addTask(comma), Task{}, "addTask");

    TaskFilter filter;

    // The over-matches a substring search for the quoted needle had. Neither
    // ',' nor '' is a tag any of these rows carries, so neither may return a
    // row: a filter that answers with tasks that do not carry the tag is
    // indistinguishable, from the caller's side, from a broken tag column.
    filter.tag = ",";
    const std::vector<Task> separator = okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    EXPECT_TRUE(separator.empty()) << "a filter for ',' matched the separator between array elements";

    filter.tag = "";
    const std::vector<Task> empty_tag = okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    EXPECT_TRUE(empty_tag.empty()) << "an empty filter tag matched something";

    // Exact membership, one row per probe: each task carries exactly one of
    // these, so a count of two would mean the filter matched a neighbour too.
    filter.tag = "a";
    const std::vector<Task> just_a = okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    ASSERT_EQ(1U, just_a.size());
    EXPECT_EQ(ab.id, just_a.front().id);

    filter.tag = "b";
    const std::vector<Task> just_b = okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    ASSERT_EQ(1U, just_b.size());
    EXPECT_EQ(ab.id, just_b.front().id);

    filter.tag = "solo";
    const std::vector<Task> only_solo = okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    ASSERT_EQ(1U, only_solo.size());
    EXPECT_EQ(solo.id, only_solo.front().id);

    filter.tag = "x";
    const std::vector<Task> just_x = okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    ASSERT_EQ(1U, just_x.size());
    EXPECT_EQ(xyz.id, just_x.front().id);

    // And the comma-bearing tag is found by its whole text, never by its parts:
    // the positive direction of the same exactness claim.
    filter.tag = "a,b";
    const std::vector<Task> the_comma =
        okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    ASSERT_EQ(1U, the_comma.size());
    EXPECT_EQ(comma_tag.id, the_comma.front().id);

    // A prefix of a tag is still not a tag.
    filter.tag = "sol";
    EXPECT_TRUE(okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks").empty());
}

TEST(TaskStoreTest, TagsWithQuotesOrBackslashesRoundTripAndStayFindable)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    // JSON escapes these as \" and \\, so the stored text no longer contains
    // the tag verbatim — the case the old substring filter documented as
    // unfindable. Comparing elements instead of text makes the escaping
    // irrelevant.
    const std::string quoted = "say \"hi\"";
    const std::string slashed = "path\\to\\thing";

    Task draft = draftTask("awkward tags");
    draft.tags = { quoted, slashed };
    const Task stored = okValue(store->addTask(draft), Task{}, "addTask");

    // Storage was always lossless; only the filter was lossy.
    const Task fetched = okValue(store->getTask(stored.id), Task{}, "getTask");
    EXPECT_EQ(std::vector<std::string>({ quoted, slashed }), fetched.tags);

    TaskFilter filter;
    filter.tag = quoted;
    const std::vector<Task> by_quote = okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    ASSERT_EQ(1U, by_quote.size()) << "a tag containing a double quote must be findable";
    EXPECT_EQ(stored.id, by_quote.front().id);

    filter.tag = slashed;
    const std::vector<Task> by_backslash =
        okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    ASSERT_EQ(1U, by_backslash.size()) << "a tag containing a backslash must be findable";
    EXPECT_EQ(stored.id, by_backslash.front().id);

    // Neither a prefix nor the empty needle is that tag. The empty one is the
    // second over-match of the old search: the stored form of `say "hi"` ends
    // in an escaped quote, so the text contains `""`.
    filter.tag = "say";
    EXPECT_TRUE(okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks").empty());
    filter.tag = "";
    EXPECT_TRUE(okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks").empty());
}

TEST(TaskStoreTest, ListTasksHonorsLimitAndCombinedFilters)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    for (int index = 0; index < 5; ++index) {
        Task draft = draftTask("task " + std::to_string(index));
        draft.tags = (index < 3) ? std::vector<std::string>{ "bulk" } : std::vector<std::string>{};
        if (2 == index) {
            draft.status = TaskStatus::kDone;
        }
        const Result<Task> stored = store->addTask(draft);
        ASSERT_TRUE(stored.ok());
    }

    TaskFilter limited;
    limited.limit = 2;
    const std::vector<Task> two = okValue(store->listTasks(limited), std::vector<Task>{}, "listTasks");
    EXPECT_EQ(2U, two.size());

    // A limit larger than the table is not an error.
    limited.limit = 50;
    const std::vector<Task> all = okValue(store->listTasks(limited), std::vector<Task>{}, "listTasks");
    EXPECT_EQ(5U, all.size());

    // Status and tag narrow together, and the limit applies afterwards.
    TaskFilter combined;
    combined.status = TaskStatus::kOpen;
    combined.tag = "bulk";
    combined.limit = 1;
    const std::vector<Task> one = okValue(store->listTasks(combined), std::vector<Task>{}, "listTasks");
    ASSERT_EQ(1U, one.size());
    EXPECT_EQ(TaskStatus::kOpen, one.front().status);
    EXPECT_EQ(std::vector<std::string>{ "bulk" }, one.front().tags);
}

TEST(TaskStoreTest, ListTasksZeroLimitMeansUnlimited)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    for (int index = 0; index < 4; ++index) {
        const Result<Task> stored = store->addTask(draftTask("task " + std::to_string(index)));
        ASSERT_TRUE(stored.ok());
    }

    const TaskFilter filter; // limit defaults to 0
    ASSERT_EQ(0U, filter.limit);
    const std::vector<Task> tasks = okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    EXPECT_EQ(4U, tasks.size());
}

// ---------------------------------------------------------------------------
// Rows this program did not write
// ---------------------------------------------------------------------------
//
// A row can be corrupted by a hand-edited database, a third-party writer, or a
// database from an older build. These cases pin the read policy down: a value
// the store would never write degrades toward the harmless end — no deadline,
// no completion stamp, not actionable — instead of being coerced by sqlite's
// own conversion rules into a plausible-looking lie that ranking believes.

TEST(TaskStoreTest, NonIntegerDueAtReadsBackAsNoDeadline)
{
    const TempDir directory;
    const std::string path = directory.file("corrupt_due_at.db");
    const std::unique_ptr<TaskStore> store = openStore(path);
    ASSERT_NE(nullptr, store);

    // The three shapes whose sqlite3_column_int64 coercion would be believed:
    // a TEXT value converts to its longest numeric prefix, else 0, so these
    // arrive as 2026, 0 and 0 — every one of them a 1970 timestamp, i.e.
    // decades overdue, which is maximum urgency in the scoring model. An
    // absent deadline scores zero urgency (docs/scoring.md), and that is what a
    // deadline this layer cannot read has to mean.
    const std::vector<std::string> corrupt_values{ "2026-10-08", "", "today" };
    for (const std::string &raw : corrupt_values) {
        const Task drafted = okValue(store->addTask(draftTask("corrupt deadline")), Task{}, "addTask");

        // Planted through a second connection because the store refuses to
        // write it — and asserted, so a failed UPDATE cannot make the
        // expectation below pass for the wrong reason.
        ASSERT_EQ(1, overwriteColumnRaw(path, drafted.id, "due_at", raw));
        EXPECT_EQ("text", storageClassRaw(path, drafted.id, "due_at"))
            << "the corrupt value did not land as TEXT, so this case proves nothing";

        const Task read_back = okValue(store->getTask(drafted.id), Task{}, "getTask");
        EXPECT_FALSE(read_back.due_at.has_value())
            << "due_at holding TEXT '" << raw << "' must read as no deadline, not as 1970";
        // Only the deadline degrades: the row is still a live task, so the
        // ranking layer sees it as undated rather than as left out.
        EXPECT_TRUE(read_back.isActionable());
        EXPECT_EQ(TaskStatus::kOpen, read_back.status);
    }

    // A genuine integer deadline still reads back, so the strictness above did
    // not simply break the column.
    const Task healthy =
        okValue(addWith(*store, "real deadline", TaskStatus::kOpen, 1'700'000'000), Task{}, "addTask");
    const Task healthy_back = okValue(store->getTask(healthy.id), Task{}, "getTask");
    ASSERT_TRUE(healthy_back.due_at.has_value());
    EXPECT_EQ(1'700'000'000, *healthy_back.due_at);

    // Nor did it drop the corrupt rows: one bad cell must not hide a row.
    const std::vector<Task> all = okValue(store->listTasks(TaskFilter{}), std::vector<Task>{}, "listTasks");
    EXPECT_EQ(corrupt_values.size() + 1, all.size());
}

TEST(TaskStoreTest, NonIntegerCompletedAtReadsBackAsNoCompletionStamp)
{
    const TempDir directory;
    const std::string path = directory.file("corrupt_completed_at.db");
    const std::unique_ptr<TaskStore> store = openStore(path);
    ASSERT_NE(nullptr, store);

    // A done task whose stamp was written as text by something other than this
    // store: 'yesterday' coerces to 0, which reads as "finished in 1970" —
    // older than the task it belongs to, and an input any duration arithmetic
    // would compute nonsense from.
    Task draft = draftTask("imported from elsewhere");
    draft.status = TaskStatus::kDone;
    const Task stored = okValue(store->addTask(draft), Task{}, "addTask");
    ASSERT_TRUE(stored.completed_at.has_value());

    ASSERT_EQ(1, overwriteColumnRaw(path, stored.id, "completed_at", "yesterday"));
    EXPECT_EQ("text", storageClassRaw(path, stored.id, "completed_at"));

    const Task read_back = okValue(store->getTask(stored.id), Task{}, "getTask");
    EXPECT_FALSE(read_back.completed_at.has_value())
        << "'yesterday' must read as no completion stamp, not as 1970";
    // The status is untouched by that: the task is still done, only the stamp
    // that cannot be trusted is withheld.
    EXPECT_EQ(TaskStatus::kDone, read_back.status);
    EXPECT_FALSE(read_back.isActionable());
}

TEST(TaskStoreTest, UnrecognisedStatusReadsBackAsArchivedAndIsNotActionable)
{
    const TempDir directory;
    const std::string path = directory.file("corrupt_status.db");
    const std::unique_ptr<TaskStore> store = openStore(path);
    ASSERT_NE(nullptr, store);

    const Task healthy = okValue(store->addTask(draftTask("healthy")), Task{}, "addTask");
    const Task corrupted = okValue(store->addTask(draftTask("corrupted")), Task{}, "addTask");
    ASSERT_EQ(1, overwriteColumnRaw(path, corrupted.id, "status", "blocked"));
    EXPECT_EQ("text", storageClassRaw(path, corrupted.id, "status"));

    const Task read_back = okValue(store->getTask(corrupted.id), Task{}, "getTask");

    // Not kOpen. Task.hpp's parser declines to guess for exactly this reason:
    // an unknown status that reads as open would let a corrupt row masquerade
    // as active work in the ranked queue.
    EXPECT_NE(TaskStatus::kOpen, read_back.status);
    EXPECT_EQ(TaskStatus::kArchived, read_back.status);
    // The ranking layer drops whatever Task::isActionable() rejects, so this
    // row cannot be promoted into the queue.
    EXPECT_FALSE(read_back.isActionable());

    // Non-destructive: the row is still there and an unfiltered list still
    // returns it, so the corruption stays in view instead of being hidden.
    const std::vector<Task> all = okValue(store->listTasks(TaskFilter{}), std::vector<Task>{}, "listTasks");
    EXPECT_EQ(2U, all.size());

    // kArchived here is a value this reader produces, not one the table holds:
    // the status filter compares the STORED text in SQL, so the corrupt row is
    // outside the filtered view. Pinned deliberately — it is what makes the
    // stderr report, not the new status, the way this row is found, and a
    // change that starts keying the filter on the degraded value has to be a
    // decision rather than an accident.
    TaskFilter archived;
    archived.status = TaskStatus::kArchived;
    const std::vector<Task> parked = okValue(store->listTasks(archived), std::vector<Task>{}, "listTasks");
    EXPECT_TRUE(parked.empty());

    // A neighbour's corruption does not change how a good status reads.
    EXPECT_TRUE(healthy.isActionable());
    const Task healthy_back = okValue(store->getTask(healthy.id), Task{}, "getTask");
    EXPECT_EQ(TaskStatus::kOpen, healthy_back.status);
}

TEST(TaskStoreTest, TagFilterSurvivesACorruptTagsCell)
{
    const TempDir directory;
    const std::string path = directory.file("corrupt_tags.db");
    const std::unique_ptr<TaskStore> store = openStore(path);
    ASSERT_NE(nullptr, store);

    Task tagged = draftTask("healthy tags");
    tagged.tags = { "work" };
    const Task healthy = okValue(store->addTask(tagged), Task{}, "addTask");
    const Task broken = okValue(store->addTask(draftTask("broken tags")), Task{}, "addTask");

    // 'not json' is what makes a JSON walk of the column raise; a substring
    // search would have scanned it happily. Setting a corrupt tags cell must
    // not turn the tag filter into a query that fails for every other row.
    ASSERT_EQ(1, overwriteColumnRaw(path, broken.id, "tags", "not json"));
    EXPECT_EQ("text", storageClassRaw(path, broken.id, "tags"));

    TaskFilter filter;
    filter.tag = "work";
    const std::vector<Task> found = okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    ASSERT_EQ(1U, found.size()) << "one corrupt tags cell must neither fail nor widen the query";
    EXPECT_EQ(healthy.id, found.front().id);

    // The corrupt row is still readable and still present, just untagged — the
    // same degradation parseTags documents.
    const Task read_back = okValue(store->getTask(broken.id), Task{}, "getTask");
    EXPECT_TRUE(read_back.tags.empty());

    // A bare JSON string is the other shape worth pinning: it is one element to
    // json_each but not an array to parseTags, and the filter follows parseTags.
    ASSERT_EQ(1, overwriteColumnRaw(path, broken.id, "tags", "\"work\""));
    const std::vector<Task> scalar = okValue(store->listTasks(filter), std::vector<Task>{}, "listTasks");
    ASSERT_EQ(1U, scalar.size());
    EXPECT_EQ(healthy.id, scalar.front().id);
}

// ---------------------------------------------------------------------------
// updateTask
// ---------------------------------------------------------------------------

TEST(TaskStoreTest, UpdateAppliesEachFieldIndependently)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    Task draft = draftTask("original", 2);
    draft.notes = "original notes";
    draft.blocks = 1;
    draft.tags = { "one" };
    draft.due_at = 1000;
    const Task before = okValue(store->addTask(draft), Task{}, "addTask");

    TaskPatch renaming;
    renaming.title = "renamed";
    const Task renamed = okValue(store->updateTask(before.id, renaming), Task{}, "updateTask");
    EXPECT_EQ("renamed", renamed.title);
    EXPECT_EQ("original notes", renamed.notes);
    EXPECT_EQ(2, renamed.importance);
    EXPECT_EQ(1, renamed.blocks);
    ASSERT_TRUE(renamed.due_at.has_value());
    EXPECT_EQ(1000, *renamed.due_at);
    EXPECT_EQ(std::vector<std::string>{ "one" }, renamed.tags);

    TaskPatch noting;
    noting.notes = "second thoughts";
    const Task noted = okValue(store->updateTask(before.id, noting), Task{}, "updateTask");
    EXPECT_EQ("renamed", noted.title);
    EXPECT_EQ("second thoughts", noted.notes);

    TaskPatch reweighted;
    reweighted.importance = 5;
    reweighted.blocks = 7;
    const Task weighted = okValue(store->updateTask(before.id, reweighted), Task{}, "updateTask");
    EXPECT_EQ(5, weighted.importance);
    EXPECT_EQ(7, weighted.blocks);
    EXPECT_EQ("second thoughts", weighted.notes);

    TaskPatch retagged;
    retagged.tags = { "two", "three" };
    const Task tagged = okValue(store->updateTask(before.id, retagged), Task{}, "updateTask");
    EXPECT_EQ(std::vector<std::string>({ "two", "three" }), tagged.tags);

    // A patch that sets nothing is still a touch, and it is legal.
    const TaskPatch empty_patch;
    const Task touched = okValue(store->updateTask(before.id, empty_patch), Task{}, "updateTask");
    EXPECT_EQ(before.id, touched.id);
    EXPECT_EQ("renamed", touched.title);
}

TEST(TaskStoreTest, UpdateDueAtHasThreeStates)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    Task draft = draftTask("deadline juggling");
    draft.due_at = 1000;
    const Task task = okValue(store->addTask(draft), Task{}, "addTask");

    // State 1: neither member set leaves the column alone.
    TaskPatch unrelated;
    unrelated.notes = "a patch with no opinion about due_at";
    const Task untouched = okValue(store->updateTask(task.id, unrelated), Task{}, "updateTask");
    ASSERT_TRUE(untouched.due_at.has_value());
    EXPECT_EQ(1000, *untouched.due_at);

    // State 2: due_at sets it.
    TaskPatch setting;
    setting.due_at = 2000;
    const Task set = okValue(store->updateTask(task.id, setting), Task{}, "updateTask");
    ASSERT_TRUE(set.due_at.has_value());
    EXPECT_EQ(2000, *set.due_at);

    // State 3: clear_due_at NULLs it.
    TaskPatch clearing;
    clearing.clear_due_at = true;
    const Task cleared = okValue(store->updateTask(task.id, clearing), Task{}, "updateTask");
    EXPECT_FALSE(cleared.due_at.has_value());

    // A cleared deadline can be set again.
    const Task reset = okValue(store->updateTask(task.id, setting), Task{}, "updateTask");
    ASSERT_TRUE(reset.due_at.has_value());
    EXPECT_EQ(2000, *reset.due_at);

    // Both members at once is a caller bug, not a precedence rule, and it
    // leaves the stored row alone.
    TaskPatch contradictory;
    contradictory.due_at = 3000;
    contradictory.clear_due_at = true;
    const Result<Task> rejected = store->updateTask(task.id, contradictory);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(ErrorCode::kInvalidArgument, rejected.error().code);
    EXPECT_NE(std::string::npos, rejected.error().message.find("due_at"));

    const Task unchanged = okValue(store->getTask(task.id), Task{}, "getTask");
    ASSERT_TRUE(unchanged.due_at.has_value());
    EXPECT_EQ(2000, *unchanged.due_at);
}

TEST(TaskStoreTest, CompletingStampsCompletedAtAndReopeningClearsIt)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    const Task task = okValue(store->addTask(draftTask("finish me")), Task{}, "addTask");
    EXPECT_FALSE(task.completed_at.has_value());

    TaskPatch complete;
    complete.status = TaskStatus::kDone;
    const Task done = okValue(store->updateTask(task.id, complete), Task{}, "updateTask");
    ASSERT_TRUE(done.completed_at.has_value());
    const std::int64_t stamp = *done.completed_at;
    EXPECT_GE(stamp, done.created_at);

    // Re-saving an already-done task must not rewrite the stamp: the first
    // completion is the historical fact.
    TaskPatch retitle;
    retitle.title = "finish me (still done)";
    const Task still_done = okValue(store->updateTask(task.id, retitle), Task{}, "updateTask");
    ASSERT_TRUE(still_done.completed_at.has_value());
    EXPECT_EQ(stamp, *still_done.completed_at);

    // Reopening clears it, so nothing reading the column sees a live task as
    // finished.
    TaskPatch reopen;
    reopen.status = TaskStatus::kOpen;
    const Task reopened = okValue(store->updateTask(task.id, reopen), Task{}, "updateTask");
    EXPECT_EQ(TaskStatus::kOpen, reopened.status);
    EXPECT_FALSE(reopened.completed_at.has_value());

    // Archiving is likewise not completion.
    TaskPatch archive;
    archive.status = TaskStatus::kArchived;
    const Task archived = okValue(store->updateTask(task.id, archive), Task{}, "updateTask");
    EXPECT_FALSE(archived.completed_at.has_value());
}

TEST(TaskStoreTest, AddTaskAlreadyDoneStampsCompletedAt)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    Task draft = draftTask("imported as done");
    draft.status = TaskStatus::kDone;
    const Task stored = okValue(store->addTask(draft), Task{}, "addTask");
    ASSERT_TRUE(stored.completed_at.has_value());
    EXPECT_GE(*stored.completed_at, stored.created_at);

    // And a draft that arrives with a forged completion stamp has it dropped.
    Task forged = draftTask("forged history");
    forged.completed_at = 1;
    const Task clean = okValue(store->addTask(forged), Task{}, "addTask");
    EXPECT_FALSE(clean.completed_at.has_value());
}

TEST(TaskStoreTest, UpdateRefreshesUpdatedAtWithoutTouchingCreatedAt)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    const Task before = okValue(store->addTask(draftTask("touch me")), Task{}, "addTask");

    TaskPatch patch;
    patch.notes = "touched";
    const Task after = okValue(store->updateTask(before.id, patch), Task{}, "updateTask");

    // The store samples a real clock, so within one second an update can land
    // on the same timestamp; it must never move backwards.
    EXPECT_GE(after.updated_at, before.updated_at);
    EXPECT_EQ(before.created_at, after.created_at);
}

TEST(TaskStoreTest, UpdateRejectsAnInvalidMergedStateAndChangesNothing)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    const Task task = okValue(store->addTask(draftTask("valid", 3)), Task{}, "addTask");

    TaskPatch bad_importance;
    bad_importance.importance = 9;
    const Result<Task> bad_weight = store->updateTask(task.id, bad_importance);
    ASSERT_FALSE(bad_weight.ok());
    EXPECT_EQ(ErrorCode::kInvalidArgument, bad_weight.error().code);
    EXPECT_NE(std::string::npos, bad_weight.error().message.find("importance"));

    TaskPatch blank_title;
    blank_title.title = "   ";
    const Result<Task> bad_title = store->updateTask(task.id, blank_title);
    ASSERT_FALSE(bad_title.ok());
    EXPECT_EQ(ErrorCode::kInvalidArgument, bad_title.error().code);
    EXPECT_NE(std::string::npos, bad_title.error().message.find("title"));

    TaskPatch bad_tags;
    bad_tags.tags = { "fine", "  " };
    const Result<Task> bad_tag = store->updateTask(task.id, bad_tags);
    ASSERT_FALSE(bad_tag.ok());
    EXPECT_EQ(ErrorCode::kInvalidArgument, bad_tag.error().code);
    EXPECT_NE(std::string::npos, bad_tag.error().message.find("tags"));

    // A valid field travelling with an invalid one must not be written either:
    // the update is all-or-nothing, so the row is exactly as it was.
    TaskPatch mixed;
    mixed.title = "should not land";
    mixed.importance = 0;
    const Result<Task> bad_mixed = store->updateTask(task.id, mixed);
    ASSERT_FALSE(bad_mixed.ok());
    EXPECT_EQ(ErrorCode::kInvalidArgument, bad_mixed.error().code);

    const Task unchanged = okValue(store->getTask(task.id), Task{}, "getTask");
    EXPECT_EQ("valid", unchanged.title);
    EXPECT_EQ(3, unchanged.importance);
    EXPECT_EQ(task.updated_at, unchanged.updated_at);
}

TEST(TaskStoreTest, MissingIdsReportNotFound)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    const Result<Task> missing = store->getTask(4242);
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(ErrorCode::kNotFound, missing.error().code);
    EXPECT_NE(std::string::npos, missing.error().message.find("4242"));

    TaskPatch patch;
    patch.title = "ghost";
    const Result<Task> ghost = store->updateTask(4242, patch);
    ASSERT_FALSE(ghost.ok());
    EXPECT_EQ(ErrorCode::kNotFound, ghost.error().code);
}

// ---------------------------------------------------------------------------
// deleteTask
// ---------------------------------------------------------------------------

TEST(TaskStoreTest, DeletingTwiceReportsNotFoundTheSecondTime)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    const Task task = okValue(store->addTask(draftTask("temporary")), Task{}, "addTask");
    expectOk(store->deleteTask(task.id), "first delete");

    const Result<Task> gone = store->getTask(task.id);
    EXPECT_FALSE(gone.ok());
    EXPECT_EQ(ErrorCode::kNotFound, gone.error().code);

    // A silent second success would hide a double-delete bug in the caller.
    const Status second = store->deleteTask(task.id);
    ASSERT_FALSE(second.ok());
    EXPECT_EQ(ErrorCode::kNotFound, second.error().code);

    // Deleting one task leaves the others alone.
    const Task kept = okValue(store->addTask(draftTask("kept")), Task{}, "addTask");
    const Status third = store->deleteTask(task.id);
    ASSERT_FALSE(third.ok());
    EXPECT_EQ(ErrorCode::kNotFound, third.error().code);
    const Task survivor = okValue(store->getTask(kept.id), Task{}, "getTask");
    EXPECT_EQ("kept", survivor.title);
}

TEST(TaskStoreTest, DeleteLeavesATombstoneNamingTheUid)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    const Task task = okValue(store->addTask(draftTask("delete me")), Task{}, "addTask");
    EXPECT_EQ(0U, okValue(store->tombstoneCount(), std::size_t{0}, "tombstoneCount"));

    expectOk(store->deleteTask(task.id), "deleteTask");

    // The row is gone from the task side and the deletion is recorded against
    // the uid. Without that record the other machine's next export would
    // insert the task straight back, and the delete would appear to undo
    // itself overnight.
    EXPECT_EQ(1U, okValue(store->tombstoneCount(), std::size_t{0}, "tombstoneCount"));
    const Result<Task> gone = store->getTaskByUid(task.uid);
    ASSERT_FALSE(gone.ok());
    EXPECT_EQ(ErrorCode::kNotFound, gone.error().code);

    // The tombstone is a record in the export, carrying the uid and a stamp at
    // least as new as the task it replaces — which is what makes a delete beat
    // a same-second edit on the other machine.
    const std::string exported = okValue(store->exportJsonl(), std::string{}, "exportJsonl");
    const std::vector<std::string> lines = splitLines(exported);
    ASSERT_EQ(1U, lines.size());
    const nlohmann::json record = nlohmann::json::parse(lines.front());
    EXPECT_EQ(task.uid, record.at("uid").get<std::string>());
    EXPECT_TRUE(record.at("deleted").get<bool>());
    EXPECT_GE(record.at("updated_at").get<std::int64_t>(), task.updated_at);

    // A second delete reports kNotFound even though that tombstone exists:
    // from the caller's side "it is gone" is the truth, and the tombstone is
    // bookkeeping rather than a task — see TaskStore.hpp.
    const Status second = store->deleteTask(task.id);
    ASSERT_FALSE(second.ok());
    EXPECT_EQ(ErrorCode::kNotFound, second.error().code);
    EXPECT_EQ(1U, okValue(store->tombstoneCount(), std::size_t{0}, "tombstoneCount"));
}

// ---------------------------------------------------------------------------
// Synchronisation — the export format and the merge
// ---------------------------------------------------------------------------
//
// A merge is the one operation here that can destroy work rather than fail
// loudly, so these cases are written around what a wrong merge does to a real
// backlog: two machines whose local ids collide, an edit that must not be
// overwritten by a stale copy, a delete that has to travel, and a dry run whose
// report has to be the truth.

TEST(TaskStoreTest, ExportJsonlIsSortedByUidOneLinePerUid)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    // Created in an order unrelated to uid order, so a passing sort assertion
    // cannot be an artifact of insertion order.
    const Task alpha = okValue(store->addTask(draftTask("alpha")), Task{}, "addTask");
    const Task beta = okValue(store->addTask(draftTask("beta")), Task{}, "addTask");
    const Task gamma = okValue(store->addTask(draftTask("gamma")), Task{}, "addTask");
    expectOk(store->deleteTask(beta.id), "deleteTask");

    const std::string exported = okValue(store->exportJsonl(), std::string{}, "exportJsonl");

    // Newline-terminated, last line included: a file that lost its final byte
    // is then visibly truncated rather than silently valid.
    ASSERT_FALSE(exported.empty());
    EXPECT_EQ('\n', exported.back());

    const std::vector<std::string> lines = splitLines(exported);
    ASSERT_EQ(3U, lines.size()); // two live tasks plus one tombstone

    std::vector<std::string> uids;
    for (const std::string &line : lines) {
        // No line may carry the local integer id: it is this database's row
        // number, and a reader that matched on it would pair two machines'
        // unrelated "row 2"s — the silent data loss the format exists to stop.
        EXPECT_EQ(std::string::npos, line.find("\"id\""))
            << "an export line carries the local id: " << line;
        // Each line stands alone as JSON; the format is not text to be split.
        const nlohmann::json record = nlohmann::json::parse(line);
        EXPECT_EQ(1, record.at("v").get<int>());
        uids.push_back(record.at("uid").get<std::string>());
    }

    // Sorted by uid — the property that lets git merge two machines' exports
    // line by line instead of conflicting wholesale, because a change to one
    // task moves exactly one line.
    std::vector<std::string> expected{ alpha.uid, beta.uid, gamma.uid };
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(expected, uids);

    // One line per uid, even though beta changed state from task to tombstone:
    // the file is a snapshot keyed by uid, not a log of events.
    const std::set<std::string> distinct(uids.begin(), uids.end());
    EXPECT_EQ(3U, distinct.size());
}

TEST(TaskStoreTest, MergeRoundTripKeepsBothMachinesBacklogs)
{
    const std::unique_ptr<TaskStore> a = openStore(":memory:");
    const std::unique_ptr<TaskStore> b = openStore(":memory:");
    ASSERT_NE(nullptr, a);
    ASSERT_NE(nullptr, b);

    // Two machines that have never met. Each numbers its own rows from 1 —
    // that is the collision the feature exists to fix, so assert it rather
    // than assume it, because a test that cannot reproduce the bug cannot
    // prove it is fixed.
    const Task a1 = okValue(a->addTask(draftTask("a one")), Task{}, "addTask");
    const Task a2 = okValue(a->addTask(draftTask("a two")), Task{}, "addTask");
    const Task b1 = okValue(b->addTask(draftTask("b one")), Task{}, "addTask");
    const Task b2 = okValue(b->addTask(draftTask("b two")), Task{}, "addTask");
    ASSERT_EQ(1, a1.id);
    ASSERT_EQ(2, a2.id);
    ASSERT_EQ(1, b1.id);
    ASSERT_EQ(2, b2.id);
    EXPECT_NE(a1.uid, b1.uid);

    // B learns A's two tasks, then A learns all four of B's.
    const MergeReport into_b = mergeFrom(*a, *b);
    EXPECT_EQ(2U, into_b.inserted);
    EXPECT_EQ(0U, into_b.updated);
    EXPECT_EQ(0U, into_b.skipped);
    EXPECT_FALSE(into_b.clean());

    const MergeReport into_a = mergeFrom(*b, *a);
    EXPECT_EQ(2U, into_a.inserted) << "the tasks A already had must not be inserted twice";
    EXPECT_EQ(2U, into_a.skipped) << "a task that came back unchanged is not an update";
    EXPECT_EQ(0U, into_a.updated);

    // Both machines hold all four — the whole point of the feature.
    const std::set<std::string> expected{ "a one", "a two", "b one", "b two" };
    for (TaskStore *store : { a.get(), b.get() }) {
        EXPECT_EQ(4U, tasksByUid(*store).size()) << "a machine lost a task in the merge";
        EXPECT_EQ(4U, uidSet(*store).size()) << "two tasks ended up sharing a uid";
        // Four distinct local row numbers. The incoming records carried no
        // local id by design, so the merge had to assign new ones — an
        // implementation that reused or copied ids would collide here.
        EXPECT_EQ(4U, idSet(*store).size()) << "local ids collided after the merge";
        EXPECT_EQ(expected, titleSet(*store)) << "a task was overwritten rather than added";
    }

    // A's own rows are untouched, stamps included: a skipped record must not
    // even count as a touch, or every sync would look like an edit to the
    // other machine.
    const Task a1_after = okValue(a->getTask(a1.id), Task{}, "getTask");
    EXPECT_EQ(a1.uid, a1_after.uid);
    EXPECT_EQ(a1.title, a1_after.title);
    EXPECT_EQ(a1.created_at, a1_after.created_at);
    EXPECT_EQ(a1.updated_at, a1_after.updated_at);
    const Task a2_after = okValue(a->getTask(a2.id), Task{}, "getTask");
    EXPECT_EQ(a2.uid, a2_after.uid);
    EXPECT_EQ(a2.updated_at, a2_after.updated_at);

    // The sync has converged: a further pass in either direction is a no-op.
    const MergeReport quiet = mergeFrom(*a, *b);
    EXPECT_TRUE(quiet.clean());
    EXPECT_EQ(4U, quiet.skipped);
}

TEST(TaskStoreTest, MergeTakesTheNewerEditAndSkipsTheOlder)
{
    const std::unique_ptr<TaskStore> a = openStore(":memory:");
    const std::unique_ptr<TaskStore> b = openStore(":memory:");
    ASSERT_NE(nullptr, a);
    ASSERT_NE(nullptr, b);

    const Task t1 = okValue(a->addTask(draftTask("t1 original")), Task{}, "addTask");
    const Task t2 = okValue(a->addTask(draftTask("t2 original")), Task{}, "addTask");
    ASSERT_EQ(2U, mergeFrom(*a, *b).inserted);

    // One export carrying both outcomes: a strictly newer record for t1, a
    // strictly older one for t2. The stamps are hand-picked because that is
    // what last-write-wins compares — "now" on both sides would make the
    // outcome depend on the clock's resolution.
    const std::string jsonl =
        liveRecord(t1.uid, "t1 edited later", t1.created_at, t1.updated_at + 500) + "\n" +
        liveRecord(t2.uid, "t2 edited earlier", t2.created_at, t2.updated_at - 500) + "\n";

    const MergeReport report = okValue(b->mergeJsonl(jsonl, false), MergeReport{}, "mergeJsonl");
    EXPECT_EQ(0U, report.inserted);
    EXPECT_EQ(1U, report.updated);
    EXPECT_EQ(1U, report.skipped);
    EXPECT_FALSE(report.clean());
    ASSERT_EQ(1U, report.updated_titles.size());
    EXPECT_EQ("t1 edited later", report.updated_titles.front());
    // The loser is not listed among the examples: it changed nothing, and a
    // dry run that named it would be describing a merge that did not happen.
    EXPECT_TRUE(report.deleted_titles.empty());

    const Task t1_in_b = okValue(b->getTaskByUid(t1.uid), Task{}, "getTaskByUid");
    EXPECT_EQ("t1 edited later", t1_in_b.title);
    EXPECT_EQ(t1.updated_at + 500, t1_in_b.updated_at);
    // created_at is history: the merge copied the fields a user can change and
    // left the birth stamp alone.
    EXPECT_EQ(t1.created_at, t1_in_b.created_at);

    const Task t2_in_b = okValue(b->getTaskByUid(t2.uid), Task{}, "getTaskByUid");
    EXPECT_EQ("t2 original", t2_in_b.title) << "an older record overwrote a newer task";
    EXPECT_EQ(t2.updated_at, t2_in_b.updated_at);

    // The other direction: the same file against the machine that produced the
    // original stamps. Same outcome, because last-write-wins is a property of
    // the stamps and not of who is merging.
    const MergeReport mirrored = okValue(a->mergeJsonl(jsonl, false), MergeReport{}, "mergeJsonl");
    EXPECT_EQ(1U, mirrored.updated);
    EXPECT_EQ(1U, mirrored.skipped);
    EXPECT_EQ("t1 edited later", okValue(a->getTaskByUid(t1.uid), Task{}, "getTaskByUid").title);
    EXPECT_EQ("t2 original", okValue(a->getTaskByUid(t2.uid), Task{}, "getTaskByUid").title);

    // Both sides now agree, so replaying the same file changes nothing: this is
    // the convergence a sync is supposed to reach, and it is also what stops
    // two machines from trading the same edit back and forth.
    const MergeReport again = okValue(a->mergeJsonl(jsonl, false), MergeReport{}, "mergeJsonl");
    EXPECT_TRUE(again.clean());
    EXPECT_EQ(2U, again.skipped);
}

TEST(TaskStoreTest, MergeCarriesADeletionAcrossMachines)
{
    const std::unique_ptr<TaskStore> a = openStore(":memory:");
    const std::unique_ptr<TaskStore> b = openStore(":memory:");
    ASSERT_NE(nullptr, a);
    ASSERT_NE(nullptr, b);

    const Task task = okValue(a->addTask(draftTask("delete this one")), Task{}, "addTask");
    ASSERT_EQ(1U, mergeFrom(*a, *b).inserted);
    ASSERT_TRUE(b->getTaskByUid(task.uid).ok());

    expectOk(a->deleteTask(task.id), "deleteTask");
    const MergeReport deletion = mergeFrom(*a, *b);
    EXPECT_EQ(1U, deletion.deleted);
    EXPECT_EQ(0U, deletion.inserted) << "the tombstone was read as a new task";
    ASSERT_EQ(1U, deletion.deleted_titles.size());
    EXPECT_EQ("delete this one", deletion.deleted_titles.front());

    // Gone on B as well, and recorded as gone there: without the tombstone B
    // would keep exporting the task and A would keep re-inserting it, which
    // looks exactly like the tool ignoring a delete.
    const Result<Task> gone = b->getTaskByUid(task.uid);
    ASSERT_FALSE(gone.ok());
    EXPECT_EQ(ErrorCode::kNotFound, gone.error().code);
    EXPECT_EQ(1U, okValue(b->tombstoneCount(), std::size_t{0}, "tombstoneCount"));

    // B's export now describes the deletion, and the uid appears exactly once —
    // as the tombstone, never as both a task and a tombstone.
    const std::string b_export = okValue(b->exportJsonl(), std::string{}, "exportJsonl");
    const std::vector<std::string> lines = splitLines(b_export);
    ASSERT_EQ(1U, lines.size());
    const nlohmann::json record = nlohmann::json::parse(lines.front());
    EXPECT_EQ(task.uid, record.at("uid").get<std::string>());
    EXPECT_TRUE(record.at("deleted").get<bool>());

    // Sending the tombstone back is a no-op, not a resurrection and not a
    // second tombstone.
    const MergeReport echo = mergeFrom(*b, *a);
    EXPECT_TRUE(echo.clean());
    EXPECT_EQ(1U, okValue(a->tombstoneCount(), std::size_t{0}, "tombstoneCount"));
}

TEST(TaskStoreTest, MergeResurrectsATaskEditedAfterItsDelete)
{
    const std::unique_ptr<TaskStore> a = openStore(":memory:");
    const std::unique_ptr<TaskStore> b = openStore(":memory:");
    ASSERT_NE(nullptr, a);
    ASSERT_NE(nullptr, b);

    const Task task = okValue(a->addTask(draftTask("kept in sync")), Task{}, "addTask");
    ASSERT_EQ(1U, mergeFrom(*a, *b).inserted);

    // A deletes it, and B has not synced yet.
    expectOk(a->deleteTask(task.id), "deleteTask");
    ASSERT_EQ(1U, okValue(a->tombstoneCount(), std::size_t{0}, "tombstoneCount"));
    ASSERT_FALSE(a->getTaskByUid(task.uid).ok());

    // B edits the task to a strictly newer stamp. Delete is not a permanent
    // ban, it is just the latest write, so a later edit outranks it
    // (TaskSync.hpp). The stamp is chosen, not sampled: the test has to know it
    // is strictly newer than the tombstone.
    const std::string edit =
        liveRecord(task.uid, "edited after the delete", task.created_at, task.updated_at + 1000);
    const MergeReport edited = okValue(b->mergeJsonl(edit, false), MergeReport{}, "mergeJsonl");
    ASSERT_EQ(1U, edited.updated);

    // Now A merges B: a tombstone on A, a strictly newer live record arriving.
    const MergeReport resurrected = mergeFrom(*b, *a);
    EXPECT_EQ(1U, resurrected.resurrected);
    EXPECT_EQ(0U, resurrected.inserted) << "a resurrection is not an insert: the tombstone had to go too";
    EXPECT_EQ(0U, resurrected.deleted);
    ASSERT_EQ(1U, resurrected.resurrected_titles.size());
    EXPECT_EQ("edited after the delete", resurrected.resurrected_titles.front());

    const Task back = okValue(a->getTaskByUid(task.uid), Task{}, "getTaskByUid");
    EXPECT_EQ("edited after the delete", back.title);
    EXPECT_EQ(task.updated_at + 1000, back.updated_at);
    EXPECT_EQ(task.created_at, back.created_at);
    // Re-created as a local row: the resurrected task carries a fresh id
    // because the old row no longer exists, and nothing may depend on the id
    // surviving a delete.
    EXPECT_NE(task.id, back.id);

    // The tombstone is gone from the table and from the export, so this machine
    // no longer claims the task is deleted anywhere.
    EXPECT_EQ(0U, okValue(a->tombstoneCount(), std::size_t{0}, "tombstoneCount"));
    const std::string exported = okValue(a->exportJsonl(), std::string{}, "exportJsonl");
    const std::vector<std::string> lines = splitLines(exported);
    ASSERT_EQ(1U, lines.size());
    const nlohmann::json record = nlohmann::json::parse(lines.front());
    EXPECT_FALSE(record.at("deleted").get<bool>());
    EXPECT_EQ("edited after the delete", record.at("title").get<std::string>());

    // And the two machines agree again.
    const MergeReport echo = mergeFrom(*a, *b);
    EXPECT_TRUE(echo.clean());
}

// This case replaces one that asserted a tombstone for an unknown uid changed
// nothing — the behaviour TaskSync.hpp's kRecordTombstone comment now argues
// against, because a machine that stores nothing republishes a file with no line
// for that uid, and an absent line is indistinguishable from "never deleted".
// The chain below is deliberately the whole story: create, delete, hop to a
// machine that never held the uid, and read that machine's OWN export.
TEST(TaskStoreTest, ATombstoneForAnUnknownUidIsRecordedAndTravels)
{
    const std::unique_ptr<TaskStore> a = openStore(":memory:");
    const std::unique_ptr<TaskStore> c = openStore(":memory:");
    ASSERT_NE(nullptr, a);
    ASSERT_NE(nullptr, c);

    // A creates a task and deletes it, so its export holds exactly one line: the
    // tombstone. C has never held that uid — neither as a task nor as a
    // tombstone — which is the one state where "I know nothing about it" and "I
    // am adopting a deletion" can be confused. The confusion is not symmetric:
    // adopting the line costs one row, skipping it destroys the only evidence
    // the deletion ever happened.
    const Task task = okValue(a->addTask(draftTask("deleted on A")), Task{}, "addTask");
    expectOk(a->deleteTask(task.id), "deleteTask");
    const std::string a_export = okValue(a->exportJsonl(), std::string{}, "exportJsonl");
    ASSERT_EQ(0U, okValue(c->tombstoneCount(), std::size_t{0}, "tombstoneCount"));

    const MergeReport adopted = okValue(c->mergeJsonl(a_export, false), MergeReport{}, "mergeJsonl");
    EXPECT_EQ(1U, adopted.inserted) << "a tombstone for an unknown uid was not recorded";
    EXPECT_EQ(0U, adopted.skipped) << "the deletion was skipped rather than stored";
    EXPECT_FALSE(adopted.clean()) << "adopting a deletion is a change, not a no-op";

    // Stored: the uid is present as a tombstone, and no task appeared for it.
    EXPECT_EQ(1U, okValue(c->tombstoneCount(), std::size_t{0}, "tombstoneCount"));
    EXPECT_TRUE(tasksByUid(*c).empty()) << "a tombstone for an unknown uid created a task";

    // ...and it SURVIVES into C's OWN export. This is the assertion the whole
    // rule turns on. A machine that stores nothing republishes a file with no
    // line for this uid; a file with no line is indistinguishable from one where
    // the task was never deleted; git propagates the omission as a deletion of
    // the tombstone; and every machine still holding the task live learns
    // nothing, keeps it and republishes it — so the deleted task comes back
    // everywhere, while the machine that dropped the line reported a clean
    // merge of one skipped record.
    const std::string c_export = okValue(c->exportJsonl(), std::string{}, "exportJsonl");
    EXPECT_EQ(a_export, c_export) << "the record did not survive the hop";
    const std::vector<std::string> lines = splitLines(c_export);
    ASSERT_EQ(1U, lines.size());
    const nlohmann::json record = nlohmann::json::parse(lines.front());
    EXPECT_EQ(task.uid, record.at("uid").get<std::string>());
    EXPECT_TRUE(record.at("deleted").get<bool>());

    // The echo back is a no-op: one record per uid, so this is neither a second
    // tombstone nor an insert (TaskSync.hpp, property 3).
    const MergeReport echo = mergeFrom(*c, *a);
    EXPECT_TRUE(echo.clean());
    EXPECT_EQ(1U, echo.skipped);
    EXPECT_EQ(1U, okValue(a->tombstoneCount(), std::size_t{0}, "tombstoneCount"));
}

TEST(TaskStoreTest, TwoMachinesDeletingOneTaskConvergeOnTheNewerStamp)
{
    const std::unique_ptr<TaskStore> a = openStore(":memory:");
    const std::unique_ptr<TaskStore> b = openStore(":memory:");
    ASSERT_NE(nullptr, a);
    ASSERT_NE(nullptr, b);

    // One task, learned by both machines from the same record, so the two rows
    // are identical and only the deletions below differ.
    const std::string uid = newUid();
    const std::int64_t created = 1700000000;
    const std::string shared = liveRecord(uid, "delete on both", created, created);
    ASSERT_EQ(1U, okValue(a->mergeJsonl(shared + "\n", false), MergeReport{}, "mergeJsonl").inserted);
    ASSERT_EQ(1U, okValue(b->mergeJsonl(shared + "\n", false), MergeReport{}, "mergeJsonl").inserted);

    // Each machine deletes it during its own second. A tombstone's stamp is only
    // ever written by deleting a LIVE row — a second delete is kNotFound — so if
    // no merge could revise a stamp, these two would stay different forever: the
    // machines never converge and git conflicts on that line at every sync. The
    // stamps are chosen because the test has to know which one is newer.
    const std::int64_t older = created + 5;
    const std::int64_t newer = created + 9;
    ASSERT_EQ(1U, okValue(a->mergeJsonl(tombstoneRecord(uid, older) + "\n", false), MergeReport{},
                          "mergeJsonl")
                      .deleted);
    ASSERT_EQ(1U, okValue(b->mergeJsonl(tombstoneRecord(uid, newer) + "\n", false), MergeReport{},
                          "mergeJsonl")
                      .deleted);

    // A → B: B's stamp is the newer one, so B has nothing to learn and must not
    // revoke its own.
    const MergeReport into_b = mergeFrom(*a, *b);
    EXPECT_EQ(1U, into_b.skipped);
    EXPECT_EQ(0U, into_b.updated);

    // B → A: strictly newer — the one action that revises a tombstone's stamp.
    const MergeReport into_a = mergeFrom(*b, *a);
    EXPECT_EQ(1U, into_a.updated);
    EXPECT_EQ(0U, into_a.inserted);
    EXPECT_EQ(0U, into_a.deleted);

    // Both sides now describe the deletion with the SAME stamp, and their
    // exports are byte-identical: that equality is the point, because two stamps
    // would mean the two files differ on this line permanently.
    const std::string a_export = okValue(a->exportJsonl(), std::string{}, "exportJsonl");
    EXPECT_EQ(a_export, okValue(b->exportJsonl(), std::string{}, "exportJsonl"));
    const std::vector<std::string> lines = splitLines(a_export);
    ASSERT_EQ(1U, lines.size());
    const nlohmann::json record = nlohmann::json::parse(lines.front());
    EXPECT_TRUE(record.at("deleted").get<bool>());
    EXPECT_EQ(newer, record.at("updated_at").get<std::int64_t>()) << "the older stamp won";

    // Converged: a further pass in either direction revises nothing, so two
    // machines cannot trade stamps back and forth.
    EXPECT_TRUE(mergeFrom(*b, *a).clean());
    EXPECT_TRUE(mergeFrom(*a, *b).clean());
    EXPECT_EQ(1U, okValue(a->tombstoneCount(), std::size_t{0}, "tombstoneCount"));
    EXPECT_EQ(1U, okValue(b->tombstoneCount(), std::size_t{0}, "tombstoneCount"));
}

TEST(TaskStoreTest, AnEqualStampDisagreementIsBrokenByContentNotBySide)
{
    const std::unique_ptr<TaskStore> a = openStore(":memory:");
    const std::unique_ptr<TaskStore> b = openStore(":memory:");
    ASSERT_NE(nullptr, a);
    ASSERT_NE(nullptr, b);

    // One task, one second, two different edits, so the stamps are EQUAL and the
    // timestamps carry no ordering at all. Stamps are wall clock at one-second
    // resolution, so this is reachable for any pair of machines rather than a
    // contrived race.
    const std::string uid = newUid();
    const std::int64_t created = 1700000000;
    const std::int64_t same_second = created + 60;
    const std::string option_a = liveRecord(uid, "offsite: option A", created, same_second);
    const std::string option_b = liveRecord(uid, "offsite: option B", created, same_second);
    ASSERT_EQ(1U, okValue(a->mergeJsonl(option_a + "\n", false), MergeReport{}, "mergeJsonl").inserted);
    ASSERT_EQ(1U, okValue(b->mergeJsonl(option_b + "\n", false), MergeReport{}, "mergeJsonl").inserted);

    // Each machine merges the other's copy. Exactly ONE of the two directions may
    // apply anything: if both did, each would adopt the other's record and the
    // pair would trade copies forever; if neither did, the machines would stay
    // forked on that uid with the merge reporting "skipped: 2, clean". Which
    // direction wins is decided by CONTENT — and the store's half of that is
    // handing decide() the local row (LocalState::task), without which both
    // directions skip and the fork is permanent.
    const MergeReport into_b = mergeFrom(*a, *b);
    const MergeReport into_a = mergeFrom(*b, *a);
    EXPECT_EQ(1U, into_b.updated + into_a.updated)
        << "the tie-break must pick one winner, not an update in each direction";

    // Both machines now hold the SAME record, decided by content alone and never
    // by which side happens to be asking.
    const std::string converged = okValue(a->exportJsonl(), std::string{}, "exportJsonl");
    EXPECT_EQ(converged, okValue(b->exportJsonl(), std::string{}, "exportJsonl"))
        << "the two machines converged on different records";
    const std::vector<std::string> lines = splitLines(converged);
    ASSERT_EQ(1U, lines.size());
    const nlohmann::json record = nlohmann::json::parse(lines.front());
    ASSERT_EQ(1, record.at("v").get<int>());
    EXPECT_EQ(same_second, record.at("updated_at").get<std::int64_t>())
        << "the tie-break moved the stamp instead of choosing a record";
    const std::string winner = record.at("title").get<std::string>();
    EXPECT_TRUE(winner == "offsite: option A" || winner == "offsite: option B")
        << "neither record won: " << winner;

    // And the pair is quiet afterwards: a further exchange applies nothing, so
    // the winner is stable rather than something the machines keep re-deciding.
    EXPECT_TRUE(mergeFrom(*a, *b).clean());
    EXPECT_TRUE(mergeFrom(*b, *a).clean());
}

TEST(TaskStoreTest, ALocalEditNeverMovesARowsStampBackwards)
{
    const std::unique_ptr<TaskStore> peer = openStore(":memory:");
    const std::unique_ptr<TaskStore> local = openStore(":memory:");
    ASSERT_NE(nullptr, peer);
    ASSERT_NE(nullptr, local);

    // A peer whose clock runs ahead: a fast clock, a laptop resumed from suspend,
    // a VM restored from a snapshot. An hour is far more than this case takes to
    // run, so the row stays "stamped in the future" for its whole duration, and
    // the stamp is well inside the window a transported stamp has to be in.
    const std::string uid = newUid();
    const std::int64_t created = systemNow();
    const std::int64_t ahead = created + 3600;
    ASSERT_EQ(1U, okValue(peer->mergeJsonl(liveRecord(uid, "from the fast peer", created, ahead) + "\n",
                                           false),
                          MergeReport{}, "mergeJsonl")
                      .inserted);

    // What this machine receives is the OTHER machine's export, not a hand-built
    // record: the case is about a real sync, and the imported row keeps the
    // peer's stamp verbatim (that is what makes the row's stamp the peer's
    // clock rather than ours).
    const std::string peer_export = okValue(peer->exportJsonl(), std::string{}, "exportJsonl");
    ASSERT_EQ(1U, okValue(local->mergeJsonl(peer_export, false), MergeReport{}, "mergeJsonl").inserted);
    const Task row = okValue(local->getTaskByUid(uid), Task{}, "getTaskByUid");
    ASSERT_EQ(ahead, row.updated_at);

    // The local edit. Stamp the row with this machine's own clock and the result
    // is an hour older than what the row already carried.
    TaskPatch retitle;
    retitle.title = "edited here";
    const Task edited = okValue(local->updateTask(row.id, retitle), Task{}, "updateTask");
    EXPECT_EQ("edited here", edited.title);
    EXPECT_GT(edited.updated_at, ahead)
        << "a local write moved the row's stamp backwards, so the export now advertises a "
           "record older than the one it was derived from";

    // The peer's export, re-imported unchanged: it still carries the row as it
    // was BEFORE the edit. This is the user-visible failure — an edit stamped
    // with the local clock looked older than the record it came from, so the
    // merge took the peer's version and the change disappeared on the machine
    // that made it.
    const MergeReport back =
        okValue(local->mergeJsonl(peer_export, false), MergeReport{}, "mergeJsonl");
    EXPECT_TRUE(back.clean());
    EXPECT_EQ(0U, back.updated);
    EXPECT_EQ(1U, back.skipped);
    const Task survived = okValue(local->getTaskByUid(uid), Task{}, "getTaskByUid");
    EXPECT_EQ("edited here", survived.title)
        << "the local edit was overwritten by the record it came from";
    EXPECT_EQ(edited.updated_at, survived.updated_at);

    // The edit then travels, which is what the fix also buys: the peer takes it
    // on the next sync. An incoming record older than the row it already holds
    // is skipped, so on the old stamping the peer never learned about it either.
    const MergeReport to_peer = mergeFrom(*local, *peer);
    EXPECT_EQ(1U, to_peer.updated);
    EXPECT_EQ("edited here", okValue(peer->getTaskByUid(uid), Task{}, "getTaskByUid").title);
}

TEST(TaskStoreTest, DeletingARowFromAFastClockPeerStampsTheTombstoneAheadOfIt)
{
    const std::unique_ptr<TaskStore> peer = openStore(":memory:");
    const std::unique_ptr<TaskStore> local = openStore(":memory:");
    ASSERT_NE(nullptr, peer);
    ASSERT_NE(nullptr, local);

    // The task arrived from a machine whose clock is an hour ahead, so the row
    // carries a stamp this machine's own clock has not reached.
    const std::string uid = newUid();
    const std::int64_t created = systemNow();
    const std::int64_t ahead = created + 3600;
    ASSERT_EQ(1U, okValue(peer->mergeJsonl(liveRecord(uid, "delete me", created, ahead) + "\n", false),
                          MergeReport{}, "mergeJsonl")
                      .inserted);
    const std::string peer_export = okValue(peer->exportJsonl(), std::string{}, "exportJsonl");
    ASSERT_EQ(1U, okValue(local->mergeJsonl(peer_export, false), MergeReport{}, "mergeJsonl").inserted);

    const Task doomed = okValue(local->getTaskByUid(uid), Task{}, "getTaskByUid");
    ASSERT_EQ(ahead, doomed.updated_at);
    expectOk(local->deleteTask(doomed.id), "deleteTask");

    // The tombstone's stamp, read back through the export the next machine will
    // see. It has to be strictly greater than the row's: a tombstone wins only a
    // tie or better, so one born older than the row it removes is skipped by
    // every merge and the deletion never travels at all.
    const std::string local_export = okValue(local->exportJsonl(), std::string{}, "exportJsonl");
    const std::vector<std::string> lines = splitLines(local_export);
    ASSERT_EQ(1U, lines.size());
    const nlohmann::json record = nlohmann::json::parse(lines.front());
    ASSERT_TRUE(record.at("deleted").get<bool>());
    const std::int64_t deleted_at = record.at("updated_at").get<std::int64_t>();
    EXPECT_GT(deleted_at, ahead) << "the tombstone was born older than the row it removed";

    // The direction that used to lose the deletion: the peer still holds the live
    // task, at the old stamp. It has to be removed there.
    const MergeReport carried = mergeFrom(*local, *peer);
    EXPECT_EQ(1U, carried.deleted);
    EXPECT_EQ(0U, carried.resurrected) << "the peer kept the task instead of deleting it";
    EXPECT_FALSE(peer->getTaskByUid(uid).ok());
    EXPECT_EQ(1U, okValue(peer->tombstoneCount(), std::size_t{0}, "tombstoneCount"));

    // And the echo cannot bring the task back on the machine that deleted it,
    // which is what an older tombstone caused: the peer's live row was strictly
    // newer than the tombstone, so kResurrect won and the deletion undid itself.
    const MergeReport echo = mergeFrom(*peer, *local);
    EXPECT_TRUE(echo.clean());
    EXPECT_FALSE(local->getTaskByUid(uid).ok()) << "the deleted task came back";
    EXPECT_EQ(1U, okValue(local->tombstoneCount(), std::size_t{0}, "tombstoneCount"));
}

TEST(TaskStoreTest, UidBackfillDerivesTheSameIdentitiesOnTwoCopies)
{
    const TempDir directory;
    const std::string first_path = directory.file("machine_a.db");
    const std::string second_path = directory.file("machine_b.db");

    // The same pre-uid backlog, twice: one database copied to another machine —
    // a restored backup, an rsync, a file carried on a stick. The rows, their
    // local ids and their timestamps are identical, and no human decided
    // anything, so no machine may distinguish them from one another.
    ASSERT_TRUE(buildVersionZeroDatabase(first_path)) << "could not plant the first copy";
    ASSERT_TRUE(buildVersionZeroDatabase(second_path)) << "could not plant the second copy";

    const std::unique_ptr<TaskStore> first = openStore(first_path);
    const std::unique_ptr<TaskStore> second = openStore(second_path);
    ASSERT_NE(nullptr, first);
    ASSERT_NE(nullptr, second);

    // Each machine migrates on its own, and both must arrive at the SAME uid for
    // every row. A freshly generated value per row and per machine is the defect
    // this pins: the two copies then assert identities neither has seen, every
    // row arrives as new on the other machine, and the first sync inserts a
    // second copy of the whole backlog.
    const std::vector<Task> first_tasks = tasksByUid(*first);
    ASSERT_EQ(2U, first_tasks.size());
    for (const Task &task : first_tasks) {
        const Task same_row = okValue(second->getTask(task.id), Task{}, "getTask");
        EXPECT_EQ(task.title, same_row.title) << "row " << task.id << " is not the same row";
        EXPECT_EQ(task.uid, same_row.uid)
            << "row " << task.id << " was given a different identity on each copy";
        EXPECT_TRUE(looksLikeUuidV4(task.uid)) << "derived uid is not a v4 uuid: " << task.uid;
    }
    EXPECT_NE(first_tasks.front().uid, first_tasks.back().uid) << "two rows share one identity";

    // The property the derivation buys: the first sync between the two copies is
    // a clean no-op. Both directions are checked because a first sync runs in
    // whichever order the operator happens to run it.
    const MergeReport into_second = mergeFrom(*first, *second);
    EXPECT_TRUE(into_second.clean()) << "the same rows arrived as new tasks on the other machine";
    EXPECT_EQ(2U, into_second.skipped);
    EXPECT_EQ(0U, into_second.inserted);
    EXPECT_EQ(2U, tasksByUid(*second).size()) << "the merge duplicated the backlog";

    EXPECT_TRUE(mergeFrom(*second, *first).clean());
    EXPECT_EQ(2U, tasksByUid(*first).size());
    // Both sides now hold literally the same file, which is what a sync that
    // recognises every row looks like from the outside.
    EXPECT_EQ(okValue(first->exportJsonl(), std::string{}, "exportJsonl"),
              okValue(second->exportJsonl(), std::string{}, "exportJsonl"));

    // Reopening re-derives nothing: the ids are already assigned, so a second
    // open cannot re-identify rows a peer has already learned.
    const std::unique_ptr<TaskStore> reopened = openStore(first_path);
    ASSERT_NE(nullptr, reopened);
    const std::vector<Task> reopened_tasks = tasksByUid(*reopened);
    ASSERT_EQ(first_tasks.size(), reopened_tasks.size());
    for (std::size_t index = 0; index < reopened_tasks.size(); ++index) {
        EXPECT_EQ(first_tasks[index].uid, reopened_tasks[index].uid);
    }
}

TEST(TaskStoreTest, DryRunReportsTheMergeWithoutPerformingIt)
{
    const std::unique_ptr<TaskStore> a = openStore(":memory:");
    const std::unique_ptr<TaskStore> b = openStore(":memory:");
    ASSERT_NE(nullptr, a);
    ASSERT_NE(nullptr, b);

    const Task one = okValue(a->addTask(draftTask("one")), Task{}, "addTask");
    const Task two = okValue(a->addTask(draftTask("two")), Task{}, "addTask");
    ASSERT_EQ(2U, mergeFrom(*a, *b).inserted);

    // A file that would insert one, update one and delete one, so the reported
    // counts are not trivially zero.
    const std::string fresh_uid = newUid();
    const std::string jsonl =
        liveRecord(fresh_uid, "brand new", 1700000000, 1700000000) + "\n" +
        liveRecord(one.uid, "one edited", one.created_at, one.updated_at + 100) + "\n" +
        tombstoneRecord(two.uid, two.updated_at) + "\n";

    const MergeReport predicted = okValue(b->mergeJsonl(jsonl, true), MergeReport{}, "mergeJsonl");
    EXPECT_EQ(1U, predicted.inserted);
    EXPECT_EQ(1U, predicted.updated);
    EXPECT_EQ(1U, predicted.deleted);
    EXPECT_EQ(0U, predicted.skipped);
    EXPECT_FALSE(predicted.clean());

    // Nothing happened. Every assertion below fails if the dry run wrote
    // anything: the insert is absent, "one" is unedited, "two" is still there
    // and no tombstone appeared. That is what makes --dry-run trustworthy.
    EXPECT_EQ(2U, tasksByUid(*b).size());
    const Result<Task> absent = b->getTaskByUid(fresh_uid);
    ASSERT_FALSE(absent.ok());
    EXPECT_EQ(ErrorCode::kNotFound, absent.error().code);
    const Task one_before = okValue(b->getTaskByUid(one.uid), Task{}, "getTaskByUid");
    EXPECT_EQ("one", one_before.title);
    EXPECT_EQ(one.updated_at, one_before.updated_at);
    const Task two_before = okValue(b->getTaskByUid(two.uid), Task{}, "getTaskByUid");
    EXPECT_EQ("two", two_before.title);
    EXPECT_EQ(0U, okValue(b->tombstoneCount(), std::size_t{0}, "tombstoneCount"));

    // The real merge produces the SAME report — byte for byte through the wire
    // serializer — which is the property TaskStore.hpp says a dry run has to
    // have to be worth anything.
    const MergeReport applied = okValue(b->mergeJsonl(jsonl, false), MergeReport{}, "mergeJsonl");
    EXPECT_EQ(toJson(predicted).dump(), toJson(applied).dump());

    // And it really applied.
    EXPECT_EQ(2U, tasksByUid(*b).size()); // "one" was updated in place, "two" removed
    const Task fresh = okValue(b->getTaskByUid(fresh_uid), Task{}, "getTaskByUid");
    EXPECT_EQ("brand new", fresh.title);
    const Task one_after = okValue(b->getTaskByUid(one.uid), Task{}, "getTaskByUid");
    EXPECT_EQ("one edited", one_after.title);
    EXPECT_EQ(one.updated_at + 100, one_after.updated_at);
    EXPECT_FALSE(b->getTaskByUid(two.uid).ok());
    EXPECT_EQ(1U, okValue(b->tombstoneCount(), std::size_t{0}, "tombstoneCount"));
}

TEST(TaskStoreTest, AMalformedExportLeavesTheStoreUntouched)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);
    const Task known = okValue(store->addTask(draftTask("untouched")), Task{}, "addTask");

    // Line 1 is a perfectly good record; line 2 is truncated JSON. The parse
    // fails the whole call before any write and names the line, because a
    // partial import would leave the store holding a task the file never fully
    // described — and the next sync would treat that fragment as truth.
    const std::string jsonl =
        liveRecord(newUid(), "would have been inserted", 1700000000, 1700000000) + "\n" +
        "{\"v\":1,\"uid\":\"" + newUid() + "\",";
    const Result<MergeReport> merged = store->mergeJsonl(jsonl, false);
    ASSERT_FALSE(merged.ok());
    EXPECT_EQ(ErrorCode::kInvalidArgument, merged.error().code);
    EXPECT_NE(std::string::npos, merged.error().message.find("2"))
        << "the error must name the offending line: " << merged.error().message;

    const std::vector<Task> tasks = tasksByUid(*store);
    ASSERT_EQ(1U, tasks.size()) << "the merge wrote part of a file it rejected";
    EXPECT_EQ(known.uid, tasks.front().uid);
    EXPECT_EQ("untouched", tasks.front().title);
    EXPECT_EQ(known.updated_at, tasks.front().updated_at);
    EXPECT_EQ(0U, okValue(store->tombstoneCount(), std::size_t{0}, "tombstoneCount"));
}

TEST(TaskStoreTest, AnInvalidRecordAbortsTheWholeMerge)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);
    const Task known = okValue(store->addTask(draftTask("untouched")), Task{}, "addTask");

    // A readable file whose second record breaks a rule addTask enforces. The
    // first record must not land either: the header promises no partial merge,
    // and validating every record before the transaction opens is what makes
    // that structural rather than a matter of careful bookkeeping.
    const std::string jsonl =
        liveRecord(newUid(), "valid and tempting", 1700000000, 1700000000) + "\n" +
        liveRecord(newUid(), "invalid", 1700000001, 1700000001, "open", 9) + "\n";
    const Result<MergeReport> merged = store->mergeJsonl(jsonl, false);
    ASSERT_FALSE(merged.ok());
    EXPECT_EQ(ErrorCode::kInvalidArgument, merged.error().code);
    EXPECT_NE(std::string::npos, merged.error().message.find("importance"))
        << "the error must name the rule that broke: " << merged.error().message;
    EXPECT_NE(std::string::npos, merged.error().message.find("2"))
        << "the error must name the offending record: " << merged.error().message;

    const std::vector<Task> tasks = tasksByUid(*store);
    ASSERT_EQ(1U, tasks.size());
    EXPECT_EQ(known.uid, tasks.front().uid);
    EXPECT_EQ(0U, okValue(store->tombstoneCount(), std::size_t{0}, "tombstoneCount"));
}

TEST(TaskStoreTest, MergeReportExamplesAreCappedButCountsAreExact)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    const std::size_t total = kMergeReportTitleLimit + 7;
    std::string jsonl;
    for (std::size_t index = 0; index < total; ++index) {
        jsonl += liveRecord(newUid(), "task " + std::to_string(index), 1700000000, 1700000000);
        jsonl += '\n';
    }

    const MergeReport report = okValue(store->mergeJsonl(jsonl, false), MergeReport{}, "mergeJsonl");
    EXPECT_EQ(total, report.inserted) << "the counts must stay exact when the examples are capped";
    EXPECT_EQ(kMergeReportTitleLimit, report.inserted_titles.size())
        << "a first sync of a large file must not turn the report into the file again";
    EXPECT_EQ(total, tasksByUid(*store).size());
}

TEST(TaskStoreTest, MergeReportSerializesWithStableKeys)
{
    MergeReport report;
    report.inserted = 2;
    report.updated = 1;
    report.deleted = 3;
    report.resurrected = 4;
    report.skipped = 5;
    report.stamps_adjusted = 6;
    report.inserted_titles = { "a", "b" };
    report.deleted_titles = { "gone" };

    const nlohmann::json json = toJson(report);
    EXPECT_EQ(2, json.at("inserted").get<int>());
    EXPECT_EQ(1, json.at("updated").get<int>());
    EXPECT_EQ(3, json.at("deleted").get<int>());
    EXPECT_EQ(4, json.at("resurrected").get<int>());
    EXPECT_EQ(5, json.at("skipped").get<int>());
    EXPECT_EQ(6, json.at("stamps_adjusted").get<int>());
    EXPECT_EQ(2U, json.at("inserted_titles").size());
    EXPECT_EQ(1U, json.at("deleted_titles").size());
    EXPECT_TRUE(json.at("updated_titles").empty());
    EXPECT_TRUE(json.at("resurrected_titles").empty());
    EXPECT_FALSE(json.at("clean").get<bool>());
    EXPECT_EQ(11U, json.size()); // six counts, four title lists, and clean

    // A report with nothing to do is the common case (a no-op sync), and it
    // says so in both places a caller might look.
    const MergeReport quiet;
    EXPECT_TRUE(quiet.clean());
    EXPECT_TRUE(toJson(quiet).at("clean").get<bool>());
}

// ---------------------------------------------------------------------------
// weights
// ---------------------------------------------------------------------------

TEST(TaskStoreTest, WeightsAreSeededWithDefaultsAndRoundTrip)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    const Weights defaults;
    const Weights seeded = okValue(store->weights(), Weights{}, "weights");
    EXPECT_DOUBLE_EQ(defaults.importance, seeded.importance);
    EXPECT_DOUBLE_EQ(defaults.urgency, seeded.urgency);
    EXPECT_DOUBLE_EQ(defaults.age, seeded.age);
    EXPECT_DOUBLE_EQ(defaults.blocks, seeded.blocks);
    EXPECT_DOUBLE_EQ(defaults.urgency_horizon_days, seeded.urgency_horizon_days);

    Weights tuned;
    tuned.importance = 1.5;
    tuned.urgency = 0.0;
    tuned.age = 12.25;
    tuned.blocks = 3.5;
    tuned.urgency_horizon_days = 2.5;
    expectOk(store->setWeights(tuned), "setWeights");

    const Weights read_back = okValue(store->weights(), Weights{}, "weights");
    EXPECT_DOUBLE_EQ(tuned.importance, read_back.importance);
    EXPECT_DOUBLE_EQ(tuned.urgency, read_back.urgency);
    EXPECT_DOUBLE_EQ(tuned.age, read_back.age);
    EXPECT_DOUBLE_EQ(tuned.blocks, read_back.blocks);
    EXPECT_DOUBLE_EQ(tuned.urgency_horizon_days, read_back.urgency_horizon_days);
}

TEST(TaskStoreTest, SetWeightsRejectsInvalidValuesAndKeepsTheStoredSet)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    Weights good;
    good.importance = 7.0;
    good.age = 1.25;
    expectOk(store->setWeights(good), "setWeights");

    Weights negative = good;
    negative.age = -1.0;
    const Status bad_negative = store->setWeights(negative);
    ASSERT_FALSE(bad_negative.ok());
    EXPECT_EQ(ErrorCode::kInvalidArgument, bad_negative.error().code);

    Weights zero_horizon = good;
    zero_horizon.urgency_horizon_days = 0.0;
    const Status bad_horizon = store->setWeights(zero_horizon);
    ASSERT_FALSE(bad_horizon.ok());
    EXPECT_EQ(ErrorCode::kInvalidArgument, bad_horizon.error().code);

    // The stored set is the last VALID one: a rejected call must not leave a
    // half-applied edit behind, because a weight change that silently did
    // nothing is indistinguishable from one that worked.
    const Weights stored = okValue(store->weights(), Weights{}, "weights");
    EXPECT_DOUBLE_EQ(7.0, stored.importance);
    EXPECT_DOUBLE_EQ(1.25, stored.age);
    EXPECT_DOUBLE_EQ(good.urgency_horizon_days, stored.urgency_horizon_days);
}

// ---------------------------------------------------------------------------
// stats
// ---------------------------------------------------------------------------

TEST(TaskStoreTest, StatsCountStatusesAndDeadlines)
{
    const std::unique_ptr<TaskStore> store = openStore(":memory:");
    ASSERT_NE(nullptr, store);

    const std::int64_t now = 1'700'000'000;
    const std::int64_t day = kSecondsPerDay;

    // A hand-built mix: one of everything that the tallies must separate.
    ASSERT_TRUE(addWith(*store, "overdue", TaskStatus::kOpen, now - 3600).ok());
    ASSERT_TRUE(addWith(*store, "due soon", TaskStatus::kInProgress, now + 3600).ok());
    ASSERT_TRUE(addWith(*store, "due later", TaskStatus::kOpen, now + 3 * day).ok());
    ASSERT_TRUE(addWith(*store, "no deadline", TaskStatus::kOpen, std::nullopt).ok());
    ASSERT_TRUE(addWith(*store, "finished late", TaskStatus::kDone, now - 100).ok());
    ASSERT_TRUE(addWith(*store, "parked", TaskStatus::kArchived, now + 100).ok());
    // Boundaries: exactly now is not overdue but is still "within 24h", and
    // exactly 24h out is not "within 24h".
    ASSERT_TRUE(addWith(*store, "due right now", TaskStatus::kOpen, now).ok());
    ASSERT_TRUE(addWith(*store, "due in exactly a day", TaskStatus::kOpen, now + day).ok());

    const Stats stats = okValue(store->stats(now), Stats{}, "stats");
    EXPECT_EQ(5U, stats.open);
    EXPECT_EQ(1U, stats.in_progress);
    EXPECT_EQ(1U, stats.done);
    EXPECT_EQ(1U, stats.archived);
    EXPECT_EQ(1U, stats.overdue);
    EXPECT_EQ(2U, stats.due_within_24h);
}

TEST(TaskStoreTest, StatsSerializeWithStableKeys)
{
    Stats stats;
    stats.open = 3;
    stats.in_progress = 1;
    stats.done = 4;
    stats.archived = 2;
    stats.overdue = 1;
    stats.due_within_24h = 2;

    const nlohmann::json json = toJson(stats);
    EXPECT_EQ(3, json.at("open").get<int>());
    EXPECT_EQ(1, json.at("in_progress").get<int>());
    EXPECT_EQ(4, json.at("done").get<int>());
    EXPECT_EQ(2, json.at("archived").get<int>());
    EXPECT_EQ(1, json.at("overdue").get<int>());
    EXPECT_EQ(2, json.at("due_within_24h").get<int>());
    EXPECT_EQ(6U, json.size());
}

} // namespace
} // namespace taskpilot
