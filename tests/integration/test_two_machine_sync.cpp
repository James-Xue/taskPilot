// tests/integration/test_two_machine_sync.cpp
//
// The acceptance test for cross-machine synchronisation, written as the file
// that fails if the feature does not actually solve the problem it was built
// for.
//
// Two real TaskStore instances, each on its own database FILE, are used the way
// two machines are used: each one accumulates work while it cannot see the
// other, and they meet only through the export text — one string of JSONL,
// handed across exactly as git would carry it. No git, no network, no mocks.
//
// Two machines are the common shape, not the only one that matters. The
// sequences that actually destroy work need MORE of them: a machine that never
// held the task and is handed its tombstone, and a machine that synced before
// the deletion and still holds the task live. Those cases open the extra stores
// they need inside the same temp directory, and each one ends by asserting that
// the machines' exports are BYTE-IDENTICAL — "the machines agree" is the
// property the design exists for, and a case that only asserts a title has not
// shown it.
//
// Why file-backed rather than ":memory:": the daemon opens a file, and every
// question this feature raises is a question about what is on disk — is a uid
// assigned once at creation, does a tombstone outlive the row it replaced, does
// a reopen see the same rows. An in-memory store would exercise none of it.
//
// Why the transported artifact is a std::string: the export is the interface
// BETWEEN machines. Merging two stores inside one process, or merging from an
// already-parsed vector, would not be what the user does on a pull, and would
// skip the parsing that turns a line git carried into a record.
//
// What the cases prove, in order:
//   1. integer ids collide across machines and the merge survives it — identity
//      is uid, not the row number;
//   2. merging the same export twice changes nothing the second time;
//   3. the records survive the round trip through the file text;
//   4. a deletion travels to every machine and does not resurrect;
//   5. a conflict resolves to the same winner on both machines;
//   6. a dry run reports what the real merge does and writes nothing;
//   7. a corrupt line aborts the whole merge and leaves the store untouched;
//   8. THE THIRD MACHINE. A machine that never held the task adopts the
//      tombstone it is handed, and the deletion still reaches a machine that
//      was holding the task live. This is the sequence that loses the task when
//      a receiver declines to store evidence about a uid it does not know:
//      the receiver's export omits the line, an omission is indistinguishable
//      from "never deleted", and the task resurrects everywhere;
//   9. two machines that each delete the same task, at their own second,
//      converge on ONE stamp — instead of each keeping its own forever, which
//      leaves the two exports differing on that line permanently;
//  10. two edits made in the same second, with different content, converge on
//      the same winner whichever machine is holding which record — the
//      tie-break is by content, not by which side happens to be ours;
//  11. two copies of one database (a restored backup, an rsync) migrate to the
//      SAME uids and merge to nothing, instead of duplicating every task;
//  12. a stamp in MILLISECONDS is refused, naming the value, and a stamp in
//      seconds is accepted — the unit is what is pinned, not the rejection;
//  13. a local edit made after importing a record from a fast clock is not
//      reverted by the other machine's stale copy.
//
// Cases 8 to 13 each end with the idempotence check case 2 states on its own:
// one more merge in each direction changes nothing. A sync that is not
// idempotent turns every routine pull into an edit and every edit into a
// conflict, so "clean the second time" is part of the property under test
// rather than a formality.
//
// The helpers report failures with ADD_FAILURE and return a harmless value
// instead of throwing: a failed store call is a contract break that the
// assertions after it should also see, and a test that dies inside a helper
// names the helper rather than the behaviour under test.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <stdlib.h>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "core/Clock.hpp"
#include "core/Result.hpp"
#include "core/Task.hpp"
#include "core/TaskStore.hpp"
#include "core/TaskSync.hpp"
#include "core/Uuid.hpp"

namespace taskpilot
{
namespace
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// One-line rendering of an error for assertion messages.
///
/// ErrorCode is a SCOPED enum, so it does not stream: without this, a failure
/// would read "expected kInvalidArgument, got 4-byte object <00 00 00 00>",
/// which names the wrong thing. The code goes out through toString(), the same
/// stable name the wire carries.
[[nodiscard]] std::string describe(const Error &error)
{
    return toString(error.code) + ": " + error.message;
}

/// A unique scratch directory that removes itself, so a failing assertion
/// cannot leave databases behind for the next run to trip over.
///
/// A fresh directory per TEST, not per process: each case opens the two
/// machines' databases inside it (plus a third machine's, where a case needs
/// one), so a case never has to invent a path and never has to clean one up.
class TempDirectory
{
  public:
    TempDirectory()
    {
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        std::string pattern = (base / "taskpilot-sync-XXXXXX").string();

        // mkdtemp is the only portable way to get a name that a concurrently
        // running test cannot also pick; a name built from a clock reading can
        // be chosen twice when two runs start in the same millisecond.
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');

        char *created = ::mkdtemp(buffer.data());
        if (nullptr != created)
        {
            m_path = created;
        }
    }

    ~TempDirectory()
    {
        // The error_code overload cannot throw: a destructor must not, and a
        // leftover temp directory is not worth failing a run over.
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }

    TempDirectory(const TempDirectory &) = delete;
    TempDirectory &operator=(const TempDirectory &) = delete;

    /// False when mkdtemp failed. The fixture refuses to run rather than
    /// falling back to a relative path, which would write into the build
    /// directory — or worse, over whatever file happened to be there.
    [[nodiscard]] bool valid() const { return !m_path.empty(); }

    [[nodiscard]] std::string file(const std::string &name) const
    {
        return (m_path / name).string();
    }

  private:
    std::filesystem::path m_path;
};

/// The real clock, read the same way the store reads it. The store stamps
/// updated_at from SystemClock, so a test that reasons about stamps must sample
/// that source instead of inventing a second notion of "now".
[[nodiscard]] std::int64_t systemNowSeconds()
{
    const SystemClock clock;
    return clock.nowEpochSeconds();
}

/// Block until the real clock's second is past `stamp`.
///
/// The store stamps updated_at from the wall clock at one-second resolution, so
/// two offline edits made inside the same second carry the SAME stamp — and a
/// tie is not something the merge resolves: with equal stamps each machine
/// keeps its own copy. The conflict case therefore waits for the next whole
/// second before the second edit, which makes the two stamps deliberately
/// different instead of accidentally equal. Bounded, so a clock that never
/// advances fails the test instead of hanging it.
void waitForNextSecondAfter(std::int64_t stamp)
{
    constexpr std::chrono::milliseconds kPollInterval{ 10 };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{ 5 };

    while (systemNowSeconds() <= stamp && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(kPollInterval);
    }

    EXPECT_LT(stamp, systemNowSeconds())
        << "the wall clock did not advance past " << stamp << " within five seconds";
}

/// Open a store, reporting a failure instead of throwing so the test body can
/// decide what to do about it.
[[nodiscard]] std::unique_ptr<TaskStore> openStore(const std::string &path)
{
    Result<std::unique_ptr<TaskStore>> opened = TaskStore::open(path);
    if (!opened.ok())
    {
        ADD_FAILURE() << "opening " << path << ": " << describe(opened.error());
        return nullptr;
    }

    return std::move(opened).value();
}

/// Create a task on a machine, the way the daemon does: the caller supplies a
/// title, the store supplies uid, id and every timestamp.
[[nodiscard]] Task addTask(TaskStore &store, const std::string &title)
{
    Task draft;
    draft.title = title;

    Result<Task> created = store.addTask(draft);
    if (!created.ok())
    {
        ADD_FAILURE() << "addTask(" << title << "): " << describe(created.error());
        return Task{};
    }

    // Identity is the whole feature, so the uid the store assigned is checked
    // here rather than trusted: a uid that is not a v4 uuid cannot be matched
    // across machines, and the damage would only appear as data loss in a sync.
    EXPECT_TRUE(looksLikeUuidV4(created.value().uid))
        << "the store assigned a uid that is not a v4 uuid: " << created.value().uid;
    return created.value();
}

void deleteById(TaskStore &store, std::int64_t id)
{
    const Status removed = store.deleteTask(id);
    EXPECT_TRUE(removed.ok()) << "deleting task " << id << ": " << describe(removed.error());
}

/// This machine's complete backlog state, in the format git carries.
[[nodiscard]] std::string exportOf(const TaskStore &store)
{
    Result<std::string> exported = store.exportJsonl();
    if (!exported.ok())
    {
        ADD_FAILURE() << "exportJsonl failed: " << describe(exported.error());
        return {};
    }

    return exported.value();
}

/// Merge an export into a store: what a machine does with the file git just
/// delivered.
[[nodiscard]] MergeReport mergeInto(TaskStore &store, const std::string &jsonl)
{
    Result<MergeReport> applied = store.mergeJsonl(jsonl, false);
    if (!applied.ok())
    {
        ADD_FAILURE() << "mergeJsonl failed: " << describe(applied.error());
        return MergeReport{};
    }

    return applied.value();
}

/// Ask what a merge would do, without doing it.
[[nodiscard]] MergeReport dryMergeInto(TaskStore &store, const std::string &jsonl)
{
    Result<MergeReport> reported = store.mergeJsonl(jsonl, true);
    if (!reported.ok())
    {
        ADD_FAILURE() << "dry mergeJsonl failed: " << describe(reported.error());
        return MergeReport{};
    }

    return reported.value();
}

/// Every task the machine holds. listTasks is unordered — ordering is the
/// ranking engine's job — so nothing here may assert on the order.
[[nodiscard]] std::vector<Task> tasksOf(const TaskStore &store)
{
    Result<std::vector<Task>> listed = store.listTasks(TaskFilter{});
    if (!listed.ok())
    {
        ADD_FAILURE() << "listTasks failed: " << describe(listed.error());
        return {};
    }

    return listed.value();
}

/// The task with this uid, or nullopt when the machine does not have it.
/// Absence is an answer here, so only a different error is reported.
[[nodiscard]] std::optional<Task> findByUid(const TaskStore &store, const std::string &uid)
{
    Result<Task> found = store.getTaskByUid(uid);
    if (found.ok())
    {
        return found.value();
    }

    if (ErrorCode::kNotFound != found.error().code)
    {
        ADD_FAILURE() << "getTaskByUid(" << uid << "): " << describe(found.error());
    }

    return std::nullopt;
}

[[nodiscard]] std::string titleOfUid(const TaskStore &store, const std::string &uid)
{
    const std::optional<Task> found = findByUid(store, uid);
    if (!found.has_value())
    {
        ADD_FAILURE() << "no task with uid " << uid << " on this machine";
        return {};
    }

    return found->title;
}

/// Assert that no task on this machine carries `title`. Used to prove a losing
/// edit is gone everywhere rather than merely absent from one store.
void expectNoTitle(const TaskStore &store, const std::string &title)
{
    for (const Task &task : tasksOf(store))
    {
        EXPECT_NE(title, task.title) << "a title that should have lost is still stored";
    }
}

[[nodiscard]] std::size_t tombstonesOf(const TaskStore &store)
{
    Result<std::size_t> counted = store.tombstoneCount();
    if (!counted.ok())
    {
        ADD_FAILURE() << "tombstoneCount failed: " << describe(counted.error());
        return 0;
    }

    return counted.value();
}

/// Local ids must be unique WITHIN a store and mean nothing BETWEEN stores —
/// that asymmetry is the entire reason uid exists. A duplicate here would mean
/// a merge reused another row's row number.
void expectUniqueLocalIds(const TaskStore &store)
{
    std::set<std::int64_t> ids;
    for (const Task &task : tasksOf(store))
    {
        EXPECT_GT(task.id, std::int64_t{ 0 }) << "a stored task must have a local row number";
        EXPECT_TRUE(ids.insert(task.id).second) << "local id " << task.id << " appears twice";
    }
}

[[nodiscard]] std::set<std::string> uidsOf(const std::vector<Task> &tasks)
{
    std::set<std::string> uids;
    for (const Task &task : tasks)
    {
        uids.insert(task.uid);
    }
    return uids;
}

/// The export's records, one per line, with blank lines dropped: a text file
/// that ends with a newline produces one when split, and trailing whitespace
/// must not change what a test counts.
[[nodiscard]] std::vector<std::string> nonBlankLines(const std::string &text)
{
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
    {
        if (!line.empty())
        {
            lines.push_back(line);
        }
    }

    return lines;
}

/// The export as (uid, record text) pairs, in file order. The record TEXT is
/// kept so a test can prove a line survived the trip through the file instead
/// of merely a field or two of it.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> recordsOf(const std::string &jsonl)
{
    std::vector<std::pair<std::string, std::string>> records;
    for (const std::string &line : nonBlankLines(jsonl))
    {
        Result<SyncRecord> parsed = parseJsonLine(line);
        if (!parsed.ok())
        {
            ADD_FAILURE() << "the export is unreadable: " << describe(parsed.error());
            continue;
        }

        records.emplace_back(parsed.value().uid, line);
    }

    return records;
}

/// The record text for `uid`, or nullptr when the export does not carry it.
[[nodiscard]] const std::string *lineForUid(
    const std::vector<std::pair<std::string, std::string>> &records, const std::string &uid)
{
    for (const auto &record : records)
    {
        if (record.first == uid)
        {
            return &record.second;
        }
    }

    return nullptr;
}

/// The parsed record for `uid`, or nullopt when the export does not carry it.
[[nodiscard]] std::optional<SyncRecord> recordOfUid(const std::string &jsonl,
                                                    const std::string &uid)
{
    for (const std::string &line : nonBlankLines(jsonl))
    {
        Result<SyncRecord> parsed = parseJsonLine(line);
        if (!parsed.ok())
        {
            ADD_FAILURE() << "the export is unreadable: " << describe(parsed.error());
            return std::nullopt;
        }

        if (parsed.value().uid == uid)
        {
            return parsed.value();
        }
    }

    return std::nullopt;
}

/// Sorted by uid is load-bearing, not cosmetic: it is what keeps a change to
/// one task on one line, so git merges two machines' files record by record
/// instead of conflicting wholesale.
void expectSortedByUid(const std::vector<std::pair<std::string, std::string>> &records)
{
    for (std::size_t index = 1; index < records.size(); ++index)
    {
        EXPECT_LT(records[index - 1].first, records[index].first)
            << "the export is not sorted by uid, so git cannot merge it line by line";
    }
}

/// A dry run is only worth trusting if its report is the report the real merge
/// produces, field by field — including the title lists, which are the part a
/// user reads before agreeing to overwrite work.
void expectSameReport(const MergeReport &reported, const MergeReport &applied)
{
    EXPECT_EQ(reported.inserted, applied.inserted);
    EXPECT_EQ(reported.updated, applied.updated);
    EXPECT_EQ(reported.deleted, applied.deleted);
    EXPECT_EQ(reported.resurrected, applied.resurrected);
    EXPECT_EQ(reported.skipped, applied.skipped);
    EXPECT_EQ(reported.clean(), applied.clean());

    EXPECT_EQ(reported.inserted_titles, applied.inserted_titles);
    EXPECT_EQ(reported.updated_titles, applied.updated_titles);
    EXPECT_EQ(reported.deleted_titles, applied.deleted_titles);
    EXPECT_EQ(reported.resurrected_titles, applied.resurrected_titles);
}

/// Assert a failure's code without streaming the scoped enum: this gtest has no
/// enum printer, so a raw EXPECT_EQ renders the actual value as
/// "4-byte object <00 00 00 00>" — a failure message that names the wrong
/// thing. This prints the stable name and the store's own message.
void expectErrorCode(ErrorCode expected, const Error &error)
{
    EXPECT_TRUE(expected == error.code)
        << "expected " << toString(expected) << ", got " << describe(error);
}

/// A live record as the one-record JSONL file an export would carry, with the
/// title and the stamp CHOSEN rather than read from a clock.
///
/// An edit's stamp cannot be dictated through updateTask — that samples the
/// wall clock — so every sequence that needs two machines to disagree at the
/// SAME second, or needs a stamp in the wrong unit or far in the future, is
/// constructed here and fed through the normal import path instead of raced
/// for. Nothing test-only reaches the store: the merge sees exactly what it
/// would see in a line git delivered, which is also how a hand-edited file (or
/// a machine whose clock is wrong) arrives.
///
/// `created_at` is separate from `stamp` so a case can be wrong about the unit
/// of ONE field and not the other, and the text is newline-terminated so it is
/// a complete file rather than a line the caller has to remember to finish.
[[nodiscard]] std::string craftedLiveJsonl(const std::string &uid, std::int64_t stamp,
                                           std::int64_t created_at, const std::string &title)
{
    nlohmann::json record;
    record["v"] = kExportFormatVersion;
    record["uid"] = uid;
    record["title"] = title;
    record["notes"] = "";
    record["status"] = "open";
    record["importance"] = 3;
    record["due_at"] = nullptr;
    record["blocks"] = 0;
    record["tags"] = nlohmann::json::array();
    record["created_at"] = created_at;
    record["updated_at"] = stamp;
    record["completed_at"] = nullptr;
    record["deleted"] = false;

    return record.dump() + "\n";
}

/// Plant a database in the shape the build before uids wrote: a `tasks` table
/// with no `uid` column, the settings row that build maintained, and rows that
/// predate the identity the sync era needs. Its settings row claims version 1
/// exactly as the old build left it, which is why the migration keys on the
/// SHAPE of the table rather than on that claim.
///
/// Raw SQL, because nothing in this build can create that shape any more — and
/// that is the point: it is the input the migration exists for. Planting it
/// through TaskStore would mean testing the migration against a database the
/// migration had already fixed.
[[nodiscard]] bool buildVersionZeroDatabase(const std::string &path)
{
    sqlite3 *db = nullptr;
    if (SQLITE_OK != sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                                     nullptr))
    {
        // sqlite3_open_v2 hands back a handle even when it fails, so the
        // failure path closes it rather than leaking the descriptor. A null
        // handle is a harmless no-op for sqlite3_close.
        sqlite3_close(db);
        return false;
    }

    const char *const kLegacySchema = R"sql(
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

    char *message = nullptr;
    const int executed = sqlite3_exec(db, kLegacySchema, nullptr, nullptr, &message);
    if (nullptr != message)
    {
        ADD_FAILURE() << "planting the version-0 database failed: " << message;
        sqlite3_free(message);
    }
    sqlite3_close(db);

    return SQLITE_OK == executed;
}

/// One text cell from a raw query against a database FILE. Used for the one
/// thing a store cannot be asked about its own input: whether the file it was
/// opened on really was the old shape, and whether the migration has since
/// added the column. Without that, a migration case would keep passing after
/// someone planted a database that needed no migrating.
[[nodiscard]] std::string rawScalarText(const std::string &path, const char *sql)
{
    sqlite3 *db = nullptr;
    if (SQLITE_OK != sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr))
    {
        sqlite3_close(db);
        ADD_FAILURE() << "cannot open " << path << " for a raw query";
        return {};
    }

    sqlite3_stmt *stmt = nullptr;
    std::string value;
    if (SQLITE_OK != sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr))
    {
        ADD_FAILURE() << "cannot prepare [" << sql << "]: " << sqlite3_errmsg(db);
    }
    else if (SQLITE_ROW == sqlite3_step(stmt))
    {
        const unsigned char *cell = sqlite3_column_text(stmt, 0);
        if (nullptr != cell)
        {
            value = reinterpret_cast<const char *>(cell);
        }
    }
    else
    {
        ADD_FAILURE() << "no row for [" << sql << "]: " << sqlite3_errmsg(db);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return value;
}

/// One round of a same-second disagreement: `left` is handed `left_file` and
/// `right` is handed `right_file` — two records for ONE uid, carrying the SAME
/// stamp and different content — and the two machines then sync both ways.
/// Returns the title they converged on.
///
/// Split out so a caller can run the round twice with the sides SWAPPED and
/// compare the two winners: two records that differ only in content must pick
/// the same winner whichever machine is holding which, and that comparison is
/// what proves the tie-break is by content rather than by which side happens to
/// be ours.
[[nodiscard]] std::string convergeOnASameSecondDisagreement(TaskStore &left, TaskStore &right,
                                                           const std::string &left_file,
                                                           const std::string &right_file,
                                                           const std::string &uid)
{
    const MergeReport seeded_left = mergeInto(left, left_file);
    EXPECT_EQ(1u, seeded_left.inserted) << "the left machine did not take the record it was given";
    const MergeReport seeded_right = mergeInto(right, right_file);
    EXPECT_EQ(1u, seeded_right.inserted) << "the right machine did not take the record it was given";

    // Both inputs captured before either merge, exactly as git would have them
    // on the two branches.
    const std::string export_left = exportOf(left);
    const std::string export_right = exportOf(right);
    EXPECT_NE(export_left, export_right)
        << "the two machines must start out disagreeing for this case to mean anything";

    const MergeReport into_left = mergeInto(left, export_right);
    const MergeReport into_right = mergeInto(right, export_left);

    // Exactly one side adopts the other's record. Both machines compare the
    // SAME pair of digests from the same two records and take the larger, so
    // only one direction can be an update — the other keeps its own copy. Two
    // updates would mean each machine took the other's record, i.e. they
    // swapped in opposite directions and converged on nothing.
    EXPECT_EQ(1u, into_left.updated + into_right.updated)
        << "the tie-break must pick one winner, not an update in each direction";

    EXPECT_EQ(exportOf(left), exportOf(right)) << "the two machines converged on different records";
    const std::string converged = titleOfUid(left, uid);
    EXPECT_EQ(converged, titleOfUid(right, uid));

    return converged;
}

/// The check cases 8 to 13 end with, and the one case 2 states on its own: with
/// both machines in their settled state, one more merge in each direction
/// changes nothing. A sync that is not idempotent turns every routine pull into
/// an edit and every edit into a conflict, so the second merge being clean is
/// part of the property under test rather than a formality.
void expectCleanSecondMerge(TaskStore &left, TaskStore &right)
{
    const std::string export_left = exportOf(left);
    const std::string export_right = exportOf(right);

    const MergeReport into_right = mergeInto(right, export_left);
    const MergeReport into_left = mergeInto(left, export_right);

    EXPECT_TRUE(into_right.clean()) << "a repeated merge reported work on the right machine";
    EXPECT_TRUE(into_left.clean()) << "a repeated merge reported work on the left machine";
    EXPECT_EQ(0u, into_right.inserted);
    EXPECT_EQ(0u, into_right.updated);
    EXPECT_EQ(0u, into_right.deleted);
    EXPECT_EQ(0u, into_right.resurrected);
    EXPECT_EQ(0u, into_left.inserted);
    EXPECT_EQ(0u, into_left.updated);
    EXPECT_EQ(0u, into_left.deleted);
    EXPECT_EQ(0u, into_left.resurrected);

    // Nothing was rewritten either: a merge that reports clean but re-stamps a
    // row has still changed the file the next commit carries.
    EXPECT_EQ(export_left, exportOf(left)) << "a clean merge rewrote the left machine's export";
    EXPECT_EQ(export_right, exportOf(right)) << "a clean merge rewrote the right machine's export";
}

// ---------------------------------------------------------------------------
// Fixture: two machines, each with its own database file inside one temporary
// directory that removes itself.
// ---------------------------------------------------------------------------

class TwoMachineSyncTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_directory.valid()) << "could not create a unique temp directory";

        m_a = openStore(m_directory.file("machine-a.db"));
        m_b = openStore(m_directory.file("machine-b.db"));

        ASSERT_TRUE(nullptr != m_a) << "the test cannot run without machine A";
        ASSERT_TRUE(nullptr != m_b) << "the test cannot run without machine B";
    }

    /// Member order is lifetime order: the destructor runs in reverse, so the
    /// stores close their databases before the directory holding the files is
    /// removed. Declared first, destroyed last.
    TempDirectory m_directory;
    std::unique_ptr<TaskStore> m_a;
    std::unique_ptr<TaskStore> m_b;
};

// ---------------------------------------------------------------------------
// 1. The collision the feature exists to prevent. Both machines work offline;
//    both number their rows from 1; the merge must keep everything anyway.
// ---------------------------------------------------------------------------
TEST_F(TwoMachineSyncTest, IntegerIdsCollideAcrossMachinesAndTheMergeKeepsEveryTask)
{
    // Two tasks each, created while neither machine knows the other exists.
    const Task a1 = addTask(*m_a, "write the migration notes");
    const Task a2 = addTask(*m_a, "review the new schema");
    const Task b1 = addTask(*m_b, "book the venue");
    const Task b2 = addTask(*m_b, "order the whiteboard");

    // The collision, asserted rather than assumed. AUTOINCREMENT means "the
    // nth row THIS database created", so both machines' first task is id 1 and
    // they are different tasks. A merge that matched on id would silently throw
    // one of these four away, which is exactly the data loss the uid prevents.
    EXPECT_EQ(std::int64_t{ 1 }, a1.id);
    EXPECT_EQ(std::int64_t{ 1 }, b1.id);
    EXPECT_NE(a1.uid, b1.uid);

    const std::string export_a = exportOf(*m_a);
    const std::string export_b = exportOf(*m_b);

    // The git step, without git: each machine folds in the text the other one
    // would have committed, both texts captured before either merge.
    const MergeReport into_b = mergeInto(*m_b, export_a);
    const MergeReport into_a = mergeInto(*m_a, export_b);

    EXPECT_EQ(2u, into_b.inserted) << "machine B did not take both of A's tasks";
    EXPECT_EQ(0u, into_b.updated) << "nothing was in B to update";
    EXPECT_EQ(0u, into_b.skipped);
    EXPECT_EQ(2u, into_a.inserted) << "machine A did not take both of B's tasks";
    EXPECT_EQ(0u, into_a.updated);

    // Each machine now holds all four, and the uid sets agree.
    const std::vector<Task> tasks_a = tasksOf(*m_a);
    const std::vector<Task> tasks_b = tasksOf(*m_b);
    ASSERT_EQ(4u, tasks_a.size()) << "a task was lost on machine A";
    ASSERT_EQ(4u, tasks_b.size()) << "a task was lost on machine B";

    const std::set<std::string> uids_a = uidsOf(tasks_a);
    const std::set<std::string> uids_b = uidsOf(tasks_b);
    EXPECT_EQ(4u, uids_a.size()) << "a duplicated uid means one task overwrote another";
    EXPECT_EQ(4u, uids_b.size()) << "a duplicated uid means one task overwrote another";
    EXPECT_EQ(uids_a, uids_b);

    const std::set<std::string> created{ a1.uid, a2.uid, b1.uid, b2.uid };
    EXPECT_EQ(created, uids_a);
    EXPECT_EQ(created, uids_b);

    // Nothing was overwritten: every task still carries the title its creator
    // gave it, on both machines.
    EXPECT_EQ(std::string("write the migration notes"), titleOfUid(*m_a, a1.uid));
    EXPECT_EQ(std::string("review the new schema"), titleOfUid(*m_a, a2.uid));
    EXPECT_EQ(std::string("book the venue"), titleOfUid(*m_a, b1.uid));
    EXPECT_EQ(std::string("order the whiteboard"), titleOfUid(*m_a, b2.uid));
    EXPECT_EQ(std::string("write the migration notes"), titleOfUid(*m_b, a1.uid));
    EXPECT_EQ(std::string("review the new schema"), titleOfUid(*m_b, a2.uid));
    EXPECT_EQ(std::string("book the venue"), titleOfUid(*m_b, b1.uid));
    EXPECT_EQ(std::string("order the whiteboard"), titleOfUid(*m_b, b2.uid));

    // Local row numbers stay unique within each store. They are NOT comparable
    // across stores, which is why they are never transported.
    expectUniqueLocalIds(*m_a);
    expectUniqueLocalIds(*m_b);

    // After a full sync the two files are identical, which is what makes the
    // next sync a no-op instead of a fresh conflict.
    EXPECT_EQ(exportOf(*m_a), exportOf(*m_b));
}

// ---------------------------------------------------------------------------
// 2. Idempotence. A sync that is not idempotent turns every routine pull into
//    an edit, and every edit into a conflict.
// ---------------------------------------------------------------------------
TEST_F(TwoMachineSyncTest, MergingTheSameExportTwiceChangesNothingTheSecondTime)
{
    const Task keep = addTask(*m_a, "keep me");
    const Task drop = addTask(*m_a, "drop me");
    deleteById(*m_a, drop.id);
    const Task b_local = addTask(*m_b, "machine B's own task");

    // A's export is one live record and one tombstone.
    const std::string export_a = exportOf(*m_a);

    const MergeReport first = mergeInto(*m_b, export_a);

    // A tombstone for a uid this machine never held is NOT a no-op: B was
    // handed the only evidence that the task was deleted, and it has to keep
    // it, or B's own export omits the line — and an omission is
    // indistinguishable from "never deleted". Case 8 is what that omission
    // costs.
    //
    // Both records count as insertions — a uid this store did not hold is now
    // one it does, and the report has no counter of its own for a tombstone
    // (MergeReport, and docs/sync.md section 6.4). The count is pinned here
    // rather than left open because this report is what a person reads before
    // agreeing to a merge that can overwrite work: a record in the file going
    // uncounted is a defect, not a detail of the store's internals.
    EXPECT_EQ(2u, first.inserted) << "the live task and the adopted tombstone are both new to B";
    EXPECT_EQ(0u, first.skipped) << "a record in the file was skipped without being applied";

    EXPECT_FALSE(findByUid(*m_b, drop.uid).has_value()) << "the tombstone must not become a task";
    EXPECT_EQ(1u, tombstonesOf(*m_b)) << "B did not record the deletion it was told about";
    const std::optional<SyncRecord> carried_by_b = recordOfUid(exportOf(*m_b), drop.uid);
    ASSERT_TRUE(carried_by_b.has_value()) << "B's export dropped the tombstone it was handed";
    EXPECT_TRUE(carried_by_b->deleted);

    const std::string b_after_first = exportOf(*m_b);
    ASSERT_NE(std::string(), b_after_first);

    // The same file again. Every record is one this machine has already
    // resolved, so there is nothing left to do.
    const MergeReport second = mergeInto(*m_b, export_a);
    EXPECT_TRUE(second.clean()) << "a repeated merge reported work";
    EXPECT_EQ(0u, second.inserted);
    EXPECT_EQ(0u, second.updated);
    EXPECT_EQ(0u, second.deleted);
    EXPECT_EQ(0u, second.resurrected);
    EXPECT_EQ(2u, second.skipped);

    EXPECT_EQ(b_after_first, exportOf(*m_b)) << "a clean merge must not rewrite the store";
    EXPECT_EQ(std::string("machine B's own task"), titleOfUid(*m_b, b_local.uid));
    EXPECT_EQ(std::string("keep me"), titleOfUid(*m_b, keep.uid));
    EXPECT_FALSE(findByUid(*m_b, drop.uid).has_value());
}

// ---------------------------------------------------------------------------
// 3. The round trip through the file text: what comes back out must contain
//    everything that went in, exactly once per uid, unchanged.
// ---------------------------------------------------------------------------
TEST_F(TwoMachineSyncTest, AnExportAfterAMergeIsASupersetWithOneLinePerUid)
{
    const Task a1 = addTask(*m_a, "A's first task");
    const Task a2 = addTask(*m_a, "A's second task");
    const Task b1 = addTask(*m_b, "B's only task");

    const std::string export_a = exportOf(*m_a);
    const std::string export_b = exportOf(*m_b);

    const MergeReport sync = mergeInto(*m_b, export_a);
    EXPECT_EQ(2u, sync.inserted) << "B did not take both of A's tasks";
    const std::string round_tripped = exportOf(*m_b);

    const std::vector<std::pair<std::string, std::string>> a_records = recordsOf(export_a);
    const std::vector<std::pair<std::string, std::string>> b_records = recordsOf(round_tripped);
    ASSERT_EQ(2u, a_records.size());
    ASSERT_EQ(3u, b_records.size()) << "B's export must be the union of both machines";

    // One line per uid, sorted: the two properties that let git merge this
    // file line by line. A second line for the same uid, or an unsorted file,
    // turns every future sync into a conflict.
    expectSortedByUid(a_records);
    expectSortedByUid(b_records);

    // Exactly one line per uid. Rather than trusting the sortedness check to
    // imply it, count the uids outright so a duplicate fails with a message
    // about duplicates.
    std::set<std::string> exported_uids;
    for (const auto &record : b_records)
    {
        exported_uids.insert(record.first);
    }
    EXPECT_EQ(3u, exported_uids.size()) << "a uid appears on more than one line";

    // Superset: every record A exported is present on B, and its text survived
    // character for character — the merge transported the record instead of
    // re-deriving it from a local clock.
    for (const auto &record : a_records)
    {
        const std::string *found = lineForUid(b_records, record.first);
        ASSERT_NE(nullptr, found) << "uid " << record.first << " did not survive the merge";
        EXPECT_EQ(record.second, *found) << "the record changed on the way through the file";
    }

    // B's own task is still there: a superset, not a replacement — and its
    // line is untouched, so absorbing A's export did not rewrite local work.
    const std::vector<std::pair<std::string, std::string>> b_own_records = recordsOf(export_b);
    ASSERT_EQ(1u, b_own_records.size());
    const std::string *b_own_line = lineForUid(b_records, b1.uid);
    ASSERT_NE(nullptr, b_own_line);
    EXPECT_EQ(b_own_records[0].second, *b_own_line)
        << "B's own record changed while B absorbed A's export";
    EXPECT_EQ(std::string("B's only task"), titleOfUid(*m_b, b1.uid));
    EXPECT_EQ(std::string("A's first task"), titleOfUid(*m_b, a1.uid));
    EXPECT_EQ(std::string("A's second task"), titleOfUid(*m_b, a2.uid));

    // A handed the file over and has not folded anything back in, so A still
    // holds exactly its own two tasks while B holds the union.
    EXPECT_EQ(2u, uidsOf(tasksOf(*m_a)).size())
        << "A's store changed while it was only exporting";
    EXPECT_EQ(3u, uidsOf(tasksOf(*m_b)).size());
}

// ---------------------------------------------------------------------------
// 4. Deleting is the case a merge gets wrong by default: absence from one side
//    is not evidence of deletion, so the deletion has to be carried.
// ---------------------------------------------------------------------------
TEST_F(TwoMachineSyncTest, ADeletionTravelsAndStaysDeletedThroughTheRoundTrip)
{
    const Task doomed = addTask(*m_a, "cancel the old subscription");
    const Task keeper = addTask(*m_a, "renew the domain");
    const std::string before_delete = exportOf(*m_a);

    // B is in sync before the deletion...
    const MergeReport seed_b = mergeInto(*m_b, before_delete);
    EXPECT_EQ(2u, seed_b.inserted);
    ASSERT_TRUE(findByUid(*m_b, doomed.uid).has_value());

    // ...and so is a third machine that will be offline for the deletion. It is
    // the machine a delete that fails to travel re-seeds the backlog from.
    const std::unique_ptr<TaskStore> offline = openStore(m_directory.file("offline-machine.db"));
    ASSERT_TRUE(nullptr != offline);
    const std::string offline_path = offline->path();
    EXPECT_EQ(m_directory.file("offline-machine.db"), offline_path);
    const MergeReport seed_offline = mergeInto(*offline, before_delete);
    EXPECT_EQ(2u, seed_offline.inserted);
    ASSERT_TRUE(findByUid(*offline, doomed.uid).has_value());

    // A deletes while offline.
    deleteById(*m_a, doomed.id);
    const std::string a_after_delete = exportOf(*m_a);

    EXPECT_FALSE(findByUid(*m_a, doomed.uid).has_value());
    const std::optional<SyncRecord> tombstone = recordOfUid(a_after_delete, doomed.uid);
    ASSERT_TRUE(tombstone.has_value())
        << "A's export says nothing about the deleted task, so no other machine can learn of it";
    EXPECT_TRUE(tombstone->deleted) << "the record for a deleted task must be a tombstone";

    // B syncs and loses the task.
    const MergeReport on_b = mergeInto(*m_b, a_after_delete);
    EXPECT_EQ(1u, on_b.deleted);
    EXPECT_FALSE(findByUid(*m_b, doomed.uid).has_value()) << "the deletion did not reach B";
    EXPECT_TRUE(findByUid(*m_b, keeper.uid).has_value()) << "the deletion took the wrong task";
    EXPECT_GE(tombstonesOf(*m_b), 1u)
        << "B must record the deletion, or the next sync re-imports the task";

    // The deletion keeps travelling: B's export carries the tombstone, or a
    // machine that was offline would re-import the task from its own copy.
    const std::string b_after_delete = exportOf(*m_b);
    const std::optional<SyncRecord> carried = recordOfUid(b_after_delete, doomed.uid);
    ASSERT_TRUE(carried.has_value()) << "B's export dropped the tombstone";
    EXPECT_TRUE(carried->deleted);

    const MergeReport on_offline = mergeInto(*offline, b_after_delete);
    EXPECT_EQ(1u, on_offline.deleted);
    EXPECT_FALSE(findByUid(*offline, doomed.uid).has_value())
        << "the task survived on a machine that only heard about the deletion second-hand";

    // And back to A: the round trip must not resurrect it. This is the
    // assertion that fails when tombstones are missing — B would never have
    // lost the task, its export would still carry it live, and merging that
    // export back into A would undo the delete.
    const MergeReport back_on_a = mergeInto(*m_a, b_after_delete);
    EXPECT_TRUE(back_on_a.clean());
    EXPECT_FALSE(findByUid(*m_a, doomed.uid).has_value())
        << "the deleted task resurrected through the round trip";
    EXPECT_TRUE(findByUid(*m_a, keeper.uid).has_value());
}

// ---------------------------------------------------------------------------
// 5. A conflict. Both machines edit one task offline; both must end up with the
//    same winner, or the backlog has silently forked into two files that git
//    can never reconcile.
// ---------------------------------------------------------------------------
TEST_F(TwoMachineSyncTest, AConflictResolvesToTheSameWinnerOnBothMachines)
{
    const char *const kLoserTitle = "pick the offsite date (A: June)";
    const char *const kWinnerTitle = "pick the offsite date (B: July)";

    const Task shared = addTask(*m_a, "pick the offsite date");
    const MergeReport seed_b = mergeInto(*m_b, exportOf(*m_a));
    EXPECT_EQ(1u, seed_b.inserted) << "B must start from the same copy of the shared task";
    ASSERT_TRUE(findByUid(*m_b, shared.uid).has_value());

    // Both machines edit the same task while offline. A's copy is edited first
    // and then the clock is allowed to move on, so B's edit is strictly newer —
    // a deliberate difference, because equal stamps are not resolved by the
    // merge at all (each machine would keep its own copy) and the test must not
    // depend on which second the edits happened to land in.
    TaskPatch patch_a;
    patch_a.title = kLoserTitle;
    Result<Task> edited_on_a = m_a->updateTask(shared.id, patch_a);
    ASSERT_TRUE(edited_on_a.ok()) << "editing on A: " << describe(edited_on_a.error());

    waitForNextSecondAfter(edited_on_a.value().updated_at);

    TaskPatch patch_b;
    patch_b.title = kWinnerTitle;
    const std::optional<Task> shared_on_b = findByUid(*m_b, shared.uid);
    ASSERT_TRUE(shared_on_b.has_value());
    Result<Task> edited_on_b = m_b->updateTask(shared_on_b->id, patch_b);
    ASSERT_TRUE(edited_on_b.ok()) << "editing on B: " << describe(edited_on_b.error());

    ASSERT_GT(edited_on_b.value().updated_at, edited_on_a.value().updated_at)
        << "the two offline edits must carry different stamps for this case to mean anything";

    // The two inputs, captured before either merge, exactly as git would have
    // them on the two branches.
    const std::string export_a = exportOf(*m_a);
    const std::string export_b = exportOf(*m_b);

    const MergeReport into_b = mergeInto(*m_b, export_a);
    const MergeReport into_a = mergeInto(*m_a, export_b);

    // The older edit loses on both machines: B refuses it (its own copy is
    // newer) and A overwrites its own copy with B's record.
    EXPECT_TRUE(into_b.clean()) << "A's record is older, so it must be a no-op on B";
    EXPECT_EQ(1u, into_a.updated) << "A must adopt B's newer record";

    EXPECT_EQ(std::string(kWinnerTitle), titleOfUid(*m_a, shared.uid));
    EXPECT_EQ(std::string(kWinnerTitle), titleOfUid(*m_b, shared.uid));
    expectNoTitle(*m_a, kLoserTitle);
    expectNoTitle(*m_b, kLoserTitle);

    // Neither machine holds anything else, and the two agree field by field —
    // including updated_at, which is what a merge that re-stamped the record
    // from its own clock would get wrong.
    ASSERT_EQ(1u, tasksOf(*m_a).size());
    ASSERT_EQ(1u, tasksOf(*m_b).size());
    const std::optional<Task> winner_on_a = findByUid(*m_a, shared.uid);
    const std::optional<Task> winner_on_b = findByUid(*m_b, shared.uid);
    ASSERT_TRUE(winner_on_a.has_value());
    ASSERT_TRUE(winner_on_b.has_value());
    EXPECT_EQ(winner_on_b->updated_at, winner_on_a->updated_at);

    // The decisive assertion. Divergence here means each machine kept its own
    // edit: two files that grew apart, where the next sync re-fights the same
    // conflict forever.
    EXPECT_EQ(exportOf(*m_a), exportOf(*m_b))
        << "the two machines resolved the conflict differently";
}

// ---------------------------------------------------------------------------
// 6. The dry run is an offer, not an act: it must report what the real merge
//    would do and leave the store exactly as it found it.
// ---------------------------------------------------------------------------
TEST_F(TwoMachineSyncTest, TheDryRunReportsWhatTheMergeWouldDoAndChangesNothing)
{
    const Task t1 = addTask(*m_a, "task one");
    const Task t2 = addTask(*m_a, "task two");
    const Task b_local = addTask(*m_b, "machine B's own task");

    const std::string export_a = exportOf(*m_a);
    const std::string b_before = exportOf(*m_b);

    const MergeReport dry = dryMergeInto(*m_b, export_a);

    // Nothing was written: the store's own export is the strictest witness,
    // because it shows every task AND every tombstone.
    EXPECT_EQ(b_before, exportOf(*m_b)) << "the dry run wrote to the store";
    EXPECT_EQ(1u, tasksOf(*m_b).size());
    EXPECT_EQ(0u, tombstonesOf(*m_b));

    const MergeReport real = mergeInto(*m_b, export_a);

    // The report a user reads before agreeing must be the report they get after
    // agreeing, or the dry run is a lie that costs them work.
    expectSameReport(dry, real);
    EXPECT_EQ(2u, dry.inserted) << "the dry run undercounted what it was offering to do";
    EXPECT_FALSE(dry.clean());

    // ...and the real merge did happen, so the comparison was not between two
    // empty reports.
    EXPECT_EQ(3u, tasksOf(*m_b).size());
    EXPECT_EQ(std::string("task one"), titleOfUid(*m_b, t1.uid));
    EXPECT_EQ(std::string("task two"), titleOfUid(*m_b, t2.uid));
    EXPECT_TRUE(findByUid(*m_b, b_local.uid).has_value());
}

// ---------------------------------------------------------------------------
// 7. A corrupt line. "Some records could not be read" and "the file was
//    genuinely short" are indistinguishable once a partial parse is applied, so
//    the whole call must fail and nothing may be written.
// ---------------------------------------------------------------------------
TEST_F(TwoMachineSyncTest, ACorruptLineAbortsTheWholeMergeAndLeavesTheStoreUntouched)
{
    const Task first = addTask(*m_a, "first");
    const Task second = addTask(*m_a, "second");
    const Task third = addTask(*m_a, "third");
    const Task b_local = addTask(*m_b, "machine B's own task");

    const std::string export_a = exportOf(*m_a);
    const std::vector<std::string> lines = nonBlankLines(export_a);
    ASSERT_EQ(3u, lines.size());

    // Line 1 and line 3 are perfectly good records that B has never seen; the
    // middle line is garbage. A merge that applied the records around the bad
    // one would leave B holding tasks its user never agreed to import.
    const std::string garbage{ "{\"v\":1,\"uid\": this is not a record" };
    const std::string corrupt = lines[0] + "\n" + garbage + "\n" + lines[2] + "\n";

    Result<SyncRecord> first_line = parseJsonLine(lines[0]);
    ASSERT_TRUE(first_line.ok()) << describe(first_line.error());

    const std::string b_before = exportOf(*m_b);
    const std::size_t tombstones_before = tombstonesOf(*m_b);

    Result<MergeReport> refused = m_b->mergeJsonl(corrupt, false);
    ASSERT_FALSE(refused.ok()) << "a corrupt line must not be reported as a successful merge";
    expectErrorCode(ErrorCode::kInvalidArgument, refused.error());
    EXPECT_NE(std::string::npos, refused.error().message.find("2"))
        << "the error must name the offending line (line 2 of this file), got: "
        << refused.error().message;

    // Byte-identical: no task, no tombstone, no row number moved.
    EXPECT_EQ(b_before, exportOf(*m_b)) << "a rejected merge wrote to the store";
    EXPECT_EQ(tombstones_before, tombstonesOf(*m_b));
    EXPECT_EQ(1u, tasksOf(*m_b).size());
    EXPECT_FALSE(findByUid(*m_b, first_line.value().uid).has_value())
        << "the record before the bad line was applied anyway";

    // The store survived the rejection rather than being left mid-transaction:
    // the next honest merge still works and brings in all three tasks.
    const MergeReport after = mergeInto(*m_b, export_a);
    EXPECT_EQ(3u, after.inserted);
    EXPECT_EQ(4u, tasksOf(*m_b).size());
    EXPECT_EQ(std::string("first"), titleOfUid(*m_b, first.uid));
    EXPECT_EQ(std::string("second"), titleOfUid(*m_b, second.uid));
    EXPECT_EQ(std::string("third"), titleOfUid(*m_b, third.uid));
    EXPECT_TRUE(findByUid(*m_b, b_local.uid).has_value()) << "the local task must survive too";
}

// ---------------------------------------------------------------------------
// 8. THE THIRD MACHINE. The sequence the feature exists for, and the one the
//    cases above cannot see: each of those machines had the task BEFORE the
//    deletion, so its store could resolve the tombstone against a row it held.
//
//    C never held the task. It receives a tombstone and nothing else — one
//    record, about a uid it has never seen — and if it declines to store that,
//    its own export omits the line. An omission is indistinguishable from
//    "this task was never deleted", so git propagates it as a deletion of the
//    deletion record, and D — which synced before the deletion and still holds
//    the task LIVE — learns nothing from the file, keeps the task, and
//    republishes it live. The deleted task comes back on every machine, and the
//    machine that caused it reported a clean merge.
//
//    So C's OWN export must still carry the tombstone, and D must delete the
//    task on the strength of it.
// ---------------------------------------------------------------------------
TEST_F(TwoMachineSyncTest, ATombstoneForATaskAMachineNeverHeldIsAdoptedAndStillDeletesIt)
{
    const Task doomed = addTask(*m_a, "cancel the old subscription");
    const std::string before_delete = exportOf(*m_a);

    // D synced BEFORE the deletion, so it holds the task live. It is the
    // machine a deletion that fails to travel re-seeds the backlog from.
    const std::unique_ptr<TaskStore> d = openStore(m_directory.file("machine-d.db"));
    ASSERT_TRUE(nullptr != d);
    const MergeReport seeded_d = mergeInto(*d, before_delete);
    EXPECT_EQ(1u, seeded_d.inserted);
    ASSERT_TRUE(findByUid(*d, doomed.uid).has_value());

    // A deletes before that task has been synced to anyone.
    deleteById(*m_a, doomed.id);
    const std::string a_after_delete = exportOf(*m_a);

    const std::optional<SyncRecord> on_a = recordOfUid(a_after_delete, doomed.uid);
    ASSERT_TRUE(on_a.has_value()) << "A's export must carry the tombstone";
    EXPECT_TRUE(on_a->deleted);

    // C has never held the task: an empty store, one record, and the record is
    // about a uid C has never seen.
    const std::unique_ptr<TaskStore> c = openStore(m_directory.file("machine-c.db"));
    ASSERT_TRUE(nullptr != c);
    ASSERT_TRUE(tasksOf(*c).empty()) << "C must start empty for this sequence to mean anything";

    const MergeReport on_c = mergeInto(*c, a_after_delete);

    // The adoption is a tombstone's bookkeeping, not a task's: no task appears
    // and none is removed, and case 2 pins how it is counted. The STATE below
    // is what this case is for — the count would look identical either way.
    EXPECT_EQ(0u, on_c.updated);
    EXPECT_EQ(0u, on_c.deleted);
    EXPECT_EQ(0u, on_c.resurrected);

    // THE ASSERTION THIS CASE EXISTS FOR. C keeps the evidence it was handed: a
    // tombstone is a statement about a uid, not a delta against a local row.
    const std::optional<SyncRecord> kept_by_c = recordOfUid(exportOf(*c), doomed.uid);
    ASSERT_TRUE(kept_by_c.has_value())
        << "C dropped the tombstone, so C's export omits the uid entirely — and an omitted line is "
           "indistinguishable from a task that was never deleted";
    EXPECT_TRUE(kept_by_c->deleted);
    EXPECT_EQ(on_a->stamp, kept_by_c->stamp)
        << "C must adopt the stamp it was handed, not invent one of its own";
    EXPECT_FALSE(findByUid(*c, doomed.uid).has_value()) << "a tombstone is not a task";
    EXPECT_EQ(1u, tombstonesOf(*c));

    // C's own export is now exactly A's: the two machines agree on the record
    // character for character, which is what makes the next commit a no-op
    // rather than a conflict.
    EXPECT_EQ(a_after_delete, exportOf(*c));

    // D receives the evidence second-hand, through C.
    const MergeReport on_d = mergeInto(*d, exportOf(*c));
    EXPECT_EQ(1u, on_d.deleted) << "D must carry out the deletion the tombstone describes";
    EXPECT_FALSE(findByUid(*d, doomed.uid).has_value())
        << "the task survived on the machine that had synced before the deletion";
    EXPECT_EQ(a_after_delete, exportOf(*d))
        << "D's tombstone must carry A's stamp, not one of D's own";

    // The resurrection cannot happen: no machine holds the task, all three
    // exports are identical, and folding any of them back into another is a
    // clean no-op — there is no machine left whose copy would put the task
    // back.
    EXPECT_FALSE(findByUid(*m_a, doomed.uid).has_value());
    EXPECT_EQ(a_after_delete, exportOf(*m_a));
    expectCleanSecondMerge(*m_a, *c);
    expectCleanSecondMerge(*m_a, *d);
    expectCleanSecondMerge(*c, *d);
}

// ---------------------------------------------------------------------------
// 9. Two deletions of one task. Both machines delete the same task locally, in
//    their own second — the shape two machines syncing around the same time
//    produce. Neither can revise its own stamp afterwards: deleting an already
//    deleted task is kNotFound, and a tombstone's stamp is only ever written by
//    deleting a LIVE row. So without a merge rule for tombstone against
//    tombstone the two stamps disagree forever, the exports differ on that line
//    permanently, and git conflicts on it at every single sync.
// ---------------------------------------------------------------------------
TEST_F(TwoMachineSyncTest, TwoDeletionsOfOneTaskConvergeOnOneStamp)
{
    const Task shared = addTask(*m_a, "cancel the duplicate booking");
    const MergeReport seeded = mergeInto(*m_b, exportOf(*m_a));
    ASSERT_EQ(1u, seeded.inserted);
    const std::optional<Task> shared_on_b = findByUid(*m_b, shared.uid);
    ASSERT_TRUE(shared_on_b.has_value());

    // A deletes first.
    deleteById(*m_a, shared.id);
    const std::optional<SyncRecord> deleted_on_a = recordOfUid(exportOf(*m_a), shared.uid);
    ASSERT_TRUE(deleted_on_a.has_value());
    ASSERT_TRUE(deleted_on_a->deleted);
    const std::int64_t stamp_a = deleted_on_a->stamp;

    // ...and B deletes the same task in a LATER second, so the two stamps
    // genuinely differ. The clock is waited on rather than raced against: a
    // case that depends on two edits landing in different seconds by luck is a
    // case that fails on a fast machine.
    waitForNextSecondAfter(stamp_a);

    deleteById(*m_b, shared_on_b->id);
    const std::optional<SyncRecord> deleted_on_b = recordOfUid(exportOf(*m_b), shared.uid);
    ASSERT_TRUE(deleted_on_b.has_value());
    ASSERT_TRUE(deleted_on_b->deleted);
    const std::int64_t stamp_b = deleted_on_b->stamp;
    ASSERT_LT(stamp_a, stamp_b);

    const std::string export_a = exportOf(*m_a);
    const std::string export_b = exportOf(*m_b);
    ASSERT_NE(export_a, export_b) << "the two machines must start out differing on that line";

    // The git step, without git: both texts captured before either merge.
    const MergeReport into_b = mergeInto(*m_b, export_a);
    const MergeReport into_a = mergeInto(*m_a, export_b);

    // The older tombstone is nothing to do. Revising the newer one changes a
    // tombstone's own stamp, so it inserts no task and brings none back; those
    // are the two counters asserted here, because the stamp converging below is
    // what this case is for and a resurrect would show up in both of them.
    EXPECT_TRUE(into_b.clean()) << "A's tombstone is older, so it must be a no-op on B";
    EXPECT_EQ(0u, into_a.inserted) << "revising a tombstone must not create a task";
    EXPECT_EQ(0u, into_a.resurrected) << "the sync undid a deletion";

    const std::optional<SyncRecord> converged_a = recordOfUid(exportOf(*m_a), shared.uid);
    const std::optional<SyncRecord> converged_b = recordOfUid(exportOf(*m_b), shared.uid);
    ASSERT_TRUE(converged_a.has_value());
    ASSERT_TRUE(converged_b.has_value());
    EXPECT_TRUE(converged_a->deleted);
    EXPECT_EQ(std::max(stamp_a, stamp_b), converged_a->stamp)
        << "the surviving tombstone must carry the newer stamp, not the local one";
    EXPECT_EQ(converged_a->stamp, converged_b->stamp) << "the two machines kept different stamps";
    EXPECT_EQ(exportOf(*m_a), exportOf(*m_b))
        << "the machines still differ on the tombstone's line, so git conflicts on it at every "
           "sync";
    EXPECT_FALSE(findByUid(*m_a, shared.uid).has_value());
    EXPECT_FALSE(findByUid(*m_b, shared.uid).has_value());

    expectCleanSecondMerge(*m_a, *m_b);
}

// ---------------------------------------------------------------------------
// 10. Two edits in the SAME second, with different content. "Equal stamp means
//     agreement" is false for two machines that each wrote in the same epoch
//     second, and nothing raises a stamp on its own — so a merge that skips on
//     equality leaves each machine holding its own copy forever while reporting
//     "skipped". The stamps are constructed rather than raced for, and the
//     round is run TWICE with the sides swapped: a tie-break that is a function
//     of the content picks the same winner both times, while "ours wins" picks
//     a different one.
// ---------------------------------------------------------------------------
TEST_F(TwoMachineSyncTest, ASameSecondDisagreementPicksTheSameWinnerWhicheverMachineIsAsking)
{
    const std::string uid = "11111111-1111-4111-8111-111111111111";
    const std::int64_t stamp = systemNowSeconds() - 60;
    const std::string file_a = craftedLiveJsonl(uid, stamp, stamp, "offsite: option A, same second");
    const std::string file_b = craftedLiveJsonl(uid, stamp, stamp, "offsite: option B, same second");

    // Round one: the record this machine happens to have is A's.
    const std::string winner_in_round_one =
        convergeOnASameSecondDisagreement(*m_a, *m_b, file_a, file_b, uid);
    ASSERT_FALSE(winner_in_round_one.empty()) << "the rounds must converge on a real record";
    expectCleanSecondMerge(*m_a, *m_b);

    // Round two: the SAME scenario with the roles swapped — same uid, same
    // stamp, the two records exchanged between the machines. Nothing about the
    // inputs has changed except which side is offering which.
    const std::unique_ptr<TaskStore> swapped_a = openStore(m_directory.file("swapped-a.db"));
    const std::unique_ptr<TaskStore> swapped_b = openStore(m_directory.file("swapped-b.db"));
    ASSERT_TRUE(nullptr != swapped_a);
    ASSERT_TRUE(nullptr != swapped_b);

    const std::string winner_in_round_two =
        convergeOnASameSecondDisagreement(*swapped_a, *swapped_b, file_b, file_a, uid);
    expectCleanSecondMerge(*swapped_a, *swapped_b);

    // The decisive comparison. A tie-break that depends on which machine is
    // asking would pick the other record here, and the backlog would fork on
    // every machine that re-ran the sync.
    EXPECT_EQ(winner_in_round_one, winner_in_round_two)
        << "the same pair of records picked a different winner when the sides were swapped, so the "
           "tie-break depends on which side is asking";
}

// ---------------------------------------------------------------------------
// 11. A COPY OF THE DATABASE. One version-0 database, copied file for file to a
//     second location and opened twice — a restored backup, an rsync, a clone.
//     The two machines hold the SAME rows, so backfilling a fresh random uuid
//     per row per machine gives one task two identities, and the first sync
//     inserts what it sees as two unknown uids and duplicates the whole
//     backlog. The backfill has to be a function of the ROW.
// ---------------------------------------------------------------------------
TEST_F(TwoMachineSyncTest, TwoCopiesOfOneVersionZeroDatabaseMigrateToTheSameUidsAndMergeToNothing)
{
    const std::string original = m_directory.file("legacy.db");
    const std::string copy = m_directory.file("legacy-copy.db");
    ASSERT_TRUE(buildVersionZeroDatabase(original)) << "could not plant the version-0 database";

    // The planted file really is the old shape, asserted rather than assumed:
    // if this ever became a version-1 database, the case would pass without
    // migrating anything, which is the classic way a migration test rots.
    EXPECT_EQ("0", rawScalarText(original, "SELECT COUNT(*) FROM pragma_table_info('tasks') "
                                           "WHERE name = 'uid'"));

    std::error_code copy_error;
    std::filesystem::copy_file(original, copy, std::filesystem::copy_options::overwrite_existing,
                              copy_error);
    ASSERT_FALSE(copy_error) << "could not copy the database: " << copy_error.message();

    // Both machines open their own file and migrate it independently.
    const std::unique_ptr<TaskStore> first = openStore(original);
    const std::unique_ptr<TaskStore> second = openStore(copy);
    ASSERT_TRUE(nullptr != first);
    ASSERT_TRUE(nullptr != second);
    EXPECT_EQ("1", rawScalarText(first->path(), "SELECT COUNT(*) FROM pragma_table_info('tasks') "
                                                "WHERE name = 'uid'"));

    const std::vector<Task> rows_first = tasksOf(*first);
    const std::vector<Task> rows_second = tasksOf(*second);
    ASSERT_EQ(2u, rows_first.size()) << "the migrated copy lost a row";
    ASSERT_EQ(2u, rows_second.size());

    std::map<std::string, std::string> uid_by_title_first;
    std::map<std::string, std::string> uid_by_title_second;
    std::set<std::string> uids_first;
    for (const Task &task : rows_first)
    {
        EXPECT_TRUE(looksLikeUuidV4(task.uid)) << "migrated uid is not a v4 uuid: " << task.uid;
        uid_by_title_first.emplace(task.title, task.uid);
        uids_first.insert(task.uid);
    }
    for (const Task &task : rows_second)
    {
        EXPECT_TRUE(looksLikeUuidV4(task.uid)) << "migrated uid is not a v4 uuid: " << task.uid;
        uid_by_title_second.emplace(task.title, task.uid);
    }

    EXPECT_EQ(2u, uids_first.size()) << "two rows were given one identity";
    EXPECT_EQ(uid_by_title_first, uid_by_title_second)
        << "the two copies of one database derived different uids for the same rows, so the first "
           "sync would duplicate every task";

    const std::string export_first = exportOf(*first);
    const std::string export_second = exportOf(*second);
    EXPECT_EQ(export_first, export_second) << "two copies of one database must export one file";

    // The merge that the old backfill turned into a duplication.
    const MergeReport no_op = mergeInto(*second, export_first);
    EXPECT_TRUE(no_op.clean()) << "merging a copy of the database back into it was not a no-op";
    EXPECT_EQ(0u, no_op.inserted) << "every task was inserted a second time";
    EXPECT_EQ(0u, no_op.updated);
    EXPECT_EQ(0u, no_op.deleted);
    EXPECT_EQ(0u, no_op.resurrected);
    EXPECT_EQ(2u, tasksOf(*second).size()) << "the backlog doubled";
    EXPECT_EQ(export_second, exportOf(*second)) << "a clean merge rewrote the file";

    expectCleanSecondMerge(*first, *second);
}

// ---------------------------------------------------------------------------
// 12. A stamp in the WRONG UNIT. Nothing in the wire format enforces that a
//     stamp is epoch seconds, and the most likely mistake — a client or a
//     hand-edited file supplying MILLISECONDS — is about a thousand times too
//     large. An accepted one is stored verbatim and compared against every
//     future write, so it outranks every local edit from then on and the task
//     freezes. The case pins the unit from both sides: refused one way,
//     accepted the other, so it cannot pass by refusing everything.
// ---------------------------------------------------------------------------
TEST_F(TwoMachineSyncTest, AMillisecondStampIsRepairedToTheInstantItMeant)
{
    const std::string uid = "55555555-5555-4555-8555-555555555555";
    const std::int64_t now = systemNowSeconds();

    // A MILLISECOND stamp is REPAIRED, not refused. Refusing it failed the whole
    // file — and because export_tasks re-parses its own output, the machine
    // holding the bad row could no longer export at all, with no API able to
    // clear it: update_task floored the poison upward, a corrected record lost
    // to it on stamp order, and a delete only moved it into a tombstone that
    // could never be resurrected. The defence survives the repair: the value is
    // clamped inside the window before it is stored, so it still cannot outrank
    // every future real edit.
    const std::int64_t millisecond_stamp = now * 1000;
    const std::string millisecond_export =
        craftedLiveJsonl(uid, millisecond_stamp, now, "stamped in milliseconds");

    const MergeReport repaired = mergeInto(*m_b, millisecond_export);
    EXPECT_EQ(1u, repaired.inserted) << "a millisecond stamp must be repaired, not lost";
    EXPECT_EQ(1u, repaired.stamps_adjusted)
        << "the repair must be COUNTED, or the wrong unit stays invisible and the "
           "peer keeps sending it forever";

    const std::optional<SyncRecord> clamped = recordOfUid(exportOf(*m_b), uid);
    ASSERT_TRUE(clamped.has_value());
    EXPECT_EQ(millisecond_stamp / 1000, clamped->stamp)
        << "a millisecond value must be divided back to the instant the sender "
           "meant; pinning it to the ceiling would leave the task frozen for decades";
    EXPECT_FALSE(clamped->stamp_adjusted)
        << "the repair is applied on the way in, so the stored record is already clean";

    // A second uid: the repaired record above now carries its true instant, so
    // a later record for the SAME uid would correctly lose to it, and this half
    // of the test is about a legitimate stamp arriving untouched.
    const std::string sane_uid = "66666666-6666-4666-8666-666666666666";
    const std::int64_t seconds_stamp = now - 60;
    const std::string seconds_export =
        craftedLiveJsonl(sane_uid, seconds_stamp, seconds_stamp, "stamped in seconds");

    const MergeReport accepted = mergeInto(*m_b, seconds_export);
    EXPECT_EQ(1u, accepted.inserted) << "a legitimate seconds stamp was refused";
    EXPECT_EQ(0u, accepted.stamps_adjusted) << "a legitimate stamp must not be touched";
    EXPECT_EQ(std::string("stamped in seconds"), titleOfUid(*m_b, sane_uid));

    // The accepted stamp travels on unchanged. A stored value that differed
    // from the one on the wire would make two machines disagree about when the
    // record changed, which is the same defect in slower motion.
    const std::optional<SyncRecord> stored = recordOfUid(exportOf(*m_b), sane_uid);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(seconds_stamp, stored->stamp);

    // Idempotence, in the form this case has: the same file merged again.
    const std::string b_settled = exportOf(*m_b);
    const MergeReport again = mergeInto(*m_b, seconds_export);
    EXPECT_TRUE(again.clean()) << "re-importing the same record reported work";
    EXPECT_EQ(0u, again.inserted);
    EXPECT_EQ(0u, again.updated);
    EXPECT_EQ(0u, again.deleted);
    EXPECT_EQ(0u, again.resurrected);
    EXPECT_EQ(b_settled, exportOf(*m_b));
}

// ---------------------------------------------------------------------------
// 13. CLOCK SKEW. A record arrives carrying a stamp far in the future — a peer
//     whose clock is fast, a laptop resumed from suspend, a hand-edited file.
//     It is inside the plausibility window (that check catches a wrong UNIT,
//     not a wrong clock), so it is stored verbatim, and the machine that made a
//     local edit LAST is then stamping it from a clock that is behind the row
//     it is editing. A stamp written straight from that clock moves BACKWARDS:
//     the edit loses the next merge to the record it was made from, and the
//     user watches their own edit disappear.
// ---------------------------------------------------------------------------
TEST_F(TwoMachineSyncTest, AnEditMadeAfterAFutureStampedImportIsNotRevertedByTheOtherMachinesCopy)
{
    const char *const kSkewedTitle = "renew the passport (the other machine's version)";
    const char *const kLocalEditTitle = "renew the passport (edited here, after the skew)";

    const Task shared = addTask(*m_a, "renew the passport");
    const MergeReport seeded = mergeInto(*m_b, exportOf(*m_a));
    ASSERT_EQ(1u, seeded.inserted);

    const std::int64_t kSkewSeconds = std::int64_t{ 300 } * 24 * 3600;
    const std::int64_t future_stamp = systemNowSeconds() + kSkewSeconds;
    const std::string skewed =
        craftedLiveJsonl(shared.uid, future_stamp, shared.created_at, kSkewedTitle);

    // The other machine holds this version, and A picks it up on the next pull.
    const MergeReport onto_b = mergeInto(*m_b, skewed);
    EXPECT_EQ(1u, onto_b.updated) << "B must hold the skewed version";
    const MergeReport onto_a = mergeInto(*m_a, exportOf(*m_b));
    EXPECT_EQ(1u, onto_a.updated) << "A must pick the skewed version up";
    const std::optional<SyncRecord> skewed_on_a = recordOfUid(exportOf(*m_a), shared.uid);
    ASSERT_TRUE(skewed_on_a.has_value());
    EXPECT_EQ(future_stamp, skewed_on_a->stamp) << "the skewed stamp must land verbatim";

    // The user edits the task on A, whose clock is 300 days BEHIND the stamp
    // the row already carries.
    const std::optional<Task> shared_on_a = findByUid(*m_a, shared.uid);
    ASSERT_TRUE(shared_on_a.has_value());
    TaskPatch edit;
    edit.title = kLocalEditTitle;
    Result<Task> edited = m_a->updateTask(shared_on_a->id, edit);
    ASSERT_TRUE(edited.ok()) << "editing on A: " << describe(edited.error());

    EXPECT_GT(edited.value().updated_at, future_stamp)
        << "the local edit's stamp went backwards relative to the row it replaced; an equal stamp "
           "would leave the next merge to the content tie-break, i.e. to chance";

    // Both exports captured before either merge, as git would deliver them: B
    // is still holding the version the edit was made from.
    const std::string export_a = exportOf(*m_a);
    const std::string export_b = exportOf(*m_b);

    // The stale copy arrives first — the sequence that used to revert the edit.
    const MergeReport stale_into_a = mergeInto(*m_a, export_b);
    EXPECT_TRUE(stale_into_a.clean()) << "the stale copy was applied over the local edit";
    EXPECT_EQ(std::string(kLocalEditTitle), titleOfUid(*m_a, shared.uid))
        << "the user's own edit was reverted by the other machine's stale copy";

    // ...and the edit is what travels on.
    const MergeReport edit_into_b = mergeInto(*m_b, export_a);
    EXPECT_EQ(1u, edit_into_b.updated) << "B must adopt the newer local edit";
    EXPECT_EQ(std::string(kLocalEditTitle), titleOfUid(*m_b, shared.uid));
    expectNoTitle(*m_a, kSkewedTitle);
    expectNoTitle(*m_b, kSkewedTitle);

    // A full round trip: B's own export now carries the edit, and merging it
    // back is a no-op — the edit is the settled state on both machines, and the
    // two files are byte-identical.
    const MergeReport round_trip = mergeInto(*m_a, exportOf(*m_b));
    EXPECT_TRUE(round_trip.clean()) << "the edit came back as a change";
    EXPECT_EQ(std::string(kLocalEditTitle), titleOfUid(*m_a, shared.uid));
    EXPECT_EQ(exportOf(*m_a), exportOf(*m_b))
        << "the two machines disagree after the edit settled";

    expectCleanSecondMerge(*m_a, *m_b);
}

} // namespace
} // namespace taskpilot
