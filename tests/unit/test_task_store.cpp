// test_task_store.cpp — TaskStore against a real (in-memory) SQLite database
//
// Every case runs on ":memory:" except the durability case, which needs a real
// file and gets one in a self-removing temp directory. No network, no fixed
// port, no shared state between cases: each TEST opens the store it uses.
//
// The store samples its own clock (see TaskStore.cpp), so timestamp assertions
// are written as brackets and monotonicity claims rather than equalities to a
// fixed instant. The one equality asserted is between two columns the store
// fills from a single sample.

#include "core/TaskStore.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
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
[[nodiscard]] sqlite3 *openRawConnection(const std::string &path)
{
    sqlite3 *db = nullptr;
    if (SQLITE_OK != sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr)) {
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
