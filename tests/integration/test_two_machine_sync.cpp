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
//   7. a corrupt line aborts the whole merge and leaves the store untouched.
//
// The helpers report failures with ADD_FAILURE and return a harmless value
// instead of throwing: a failed store call is a contract break that the
// assertions after it should also see, and a test that dies inside a helper
// names the helper rather than the behaviour under test.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
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
    EXPECT_EQ(1u, first.inserted) << "only the surviving task is new to B";
    EXPECT_EQ(1u, first.skipped) << "a tombstone for a task B never had is a no-op";

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

} // namespace
} // namespace taskpilot
