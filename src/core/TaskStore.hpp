#pragma once
// TaskStore.hpp — SQLite persistence for tasks and ranking weights
//
// Schema (created on first open; see TaskStore.cpp for the DDL):
//   tasks(id, uid, title, notes, status, importance, due_at, blocks, tags,
//         created_at, updated_at, completed_at)
//   tombstones(uid, deleted_at)   -- records of deletions, see below
//   settings(key, value)          -- 'weights' (JSON), 'schema_version'
//
// Schema version 1 added `uid` and `tombstones`. open() migrates a version-0
// database in place: it adds the column, backfills a uuid for every existing
// row, and creates the table. The migration is idempotent, so a database that
// has already been migrated (or one created fresh at version 1) is untouched.
//
// Why tombstones exist: without a record of a deletion, the deletion cannot
// travel between machines. The other machine still has the task, exports it,
// the merge sees a uid it does not know, inserts it — and the thing you
// deliberately deleted comes back. So deleteTask removes the row from `tasks`
// AND leaves a tombstone, and absence from one side is never read as evidence
// of deletion. Tombstones are tiny and never expire; a uid that is gone is
// gone, so there is nothing to garbage-collect and no window in which a
// resurrection can slip through.
//
// Layering: this layer knows SQL and the Task struct. It does NOT rank, and
// it does NOT know about JSON-RPC or MCP. Whatever comes out of listTasks is
// an unordered set — ordering is PriorityEngine's job. Keeping that boundary
// means a scoring change can never require a storage migration.
//
// Thread-safety: the RPC server runs one thread per connection, so several
// threads reach one store concurrently. SQLite can be compiled in any
// threading mode, so this class does not rely on the library's own locking:
// every public method takes m_mutex for the duration of its statement. That
// serializes access rather than allowing read concurrency — acceptable at
// this scale (a personal backlog), and it removes an entire class of
// hard-to-reproduce corruption bugs.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/Result.hpp"
#include "core/Task.hpp"
#include "core/Weights.hpp"

struct sqlite3; // forward declaration keeps <sqlite3.h> out of this header

namespace taskpilot
{

/// Which tasks listTasks should return.
struct TaskFilter
{
    std::optional<TaskStatus> status; ///< nullopt = every status.
    std::optional<std::string> tag;   ///< nullopt = every tag.
    std::size_t limit{ 0 };           ///< 0 = no limit.
};

/// A partial update. Each absent optional leaves its column untouched, so a
/// caller can change one field without having to re-send the whole task.
///
/// due_at needs two members because a plain optional cannot express the three
/// distinct states a nullable column has:
///   - neither set          -> leave due_at alone
///   - due_at = 12345       -> set it to 12345
///   - clear_due_at = true  -> set it to NULL
/// Passing both is a kInvalidArgument error rather than a silent precedence
/// rule, because the two together are always a caller bug.
struct TaskPatch
{
    std::optional<std::string> title;
    std::optional<std::string> notes;
    std::optional<TaskStatus> status;
    std::optional<int> importance;
    std::optional<std::int64_t> due_at;
    bool clear_due_at{ false };
    std::optional<int> blocks;
    std::optional<std::vector<std::string>> tags;
};

/// Counts for the status/stats endpoints. These are raw tallies straight from
/// SQL — anything derived (top of queue, highest score) belongs to the caller,
/// which has the engine.
struct Stats
{
    std::size_t open{ 0 };
    std::size_t in_progress{ 0 };
    std::size_t done{ 0 };
    std::size_t archived{ 0 };
    std::size_t overdue{ 0 };      ///< Actionable and due_at < now.
    std::size_t due_within_24h{ 0 }; ///< Actionable, due_at in [now, now+1d).
};

/// What a merge did, or would do under `dry_run`.
///
/// The title lists exist for the dry run, which is the whole reason the report
/// is not just counts: a merge is the one operation here that can destroy
/// work, so a caller must be able to SEE which tasks are about to be
/// overwritten or removed before agreeing to it. They are capped (see
/// kMergeReportTitleLimit) so a first sync of a large file cannot turn into a
/// multi-megabyte reply.
struct MergeReport
{
    std::size_t inserted{ 0 };    ///< New uids created locally.
    std::size_t updated{ 0 };     ///< Existing uids overwritten by a newer record.
    std::size_t deleted{ 0 };     ///< Existing uids removed by a newer tombstone.
    std::size_t resurrected{ 0 }; ///< Tombstoned uids brought back by a newer edit.
    std::size_t skipped{ 0 };     ///< Records the local copy already won.

    std::vector<std::string> inserted_titles;
    std::vector<std::string> updated_titles;
    std::vector<std::string> deleted_titles;
    std::vector<std::string> resurrected_titles;

    /// True when the merge did (or would) change nothing. A no-op sync is the
    /// common case, and it should be distinguishable from a failure at a glance.
    [[nodiscard]] bool clean() const
    {
        return 0 == (inserted + updated + deleted + resurrected);
    }
};

/// How many titles each MergeReport list carries before it is truncated.
/// The counts stay exact; only the examples are capped.
inline constexpr std::size_t kMergeReportTitleLimit{ 50 };

/// Owns the SQLite handle and every statement against it.
class TaskStore
{
  public:
    /// Open (creating on first use) the database at `path`, apply the schema,
    /// and seed default weights if the settings row is absent.
    ///
    /// `path` may be ":memory:" for a throwaway database, which is what the
    /// unit tests use. Returns kStorageFailure if the file is unreadable or
    /// the schema cannot be applied.
    [[nodiscard]] static Result<std::unique_ptr<TaskStore>> open(const std::string &path);

    ~TaskStore();

    TaskStore(const TaskStore &) = delete;
    TaskStore &operator=(const TaskStore &) = delete;
    TaskStore(TaskStore &&) = delete;
    TaskStore &operator=(TaskStore &&) = delete;

    /// Insert a task. The store assigns uid, id, created_at and updated_at —
    /// whatever the caller put in those fields is overwritten, so a client
    /// cannot forge history or, worse, choose its own uid and collide with a
    /// task arriving from another machine. `completed_at` is set when the
    /// incoming status is kDone.
    ///
    /// Validation (shared by every write path, so no caller can bypass it):
    /// 1. title, after trimming surrounding whitespace, must be non-empty.
    /// 2. importance must be in 1..5 inclusive.
    /// 3. blocks must be >= 0.
    /// 4. every tag must be non-empty after trimming.
    [[nodiscard]] Result<Task> addTask(Task draft);

    /// Fetch one task. kNotFound if no such id.
    [[nodiscard]] Result<Task> getTask(std::int64_t id) const;

    /// Fetch many tasks in unspecified order — the caller ranks them.
    [[nodiscard]] Result<std::vector<Task>> listTasks(const TaskFilter &filter) const;

    /// Apply a partial update and return the task as it now stands.
    ///
    /// Side effects driven by status transitions:
    ///   - -> kDone sets completed_at if it is not already set.
    ///   - out of kDone clears completed_at.
    /// updated_at is always refreshed. Validation matches addTask.
    [[nodiscard]] Result<Task> updateTask(std::int64_t id, const TaskPatch &patch);

    /// Remove a task, leaving a tombstone so the deletion can travel to other
    /// machines (see the tombstones note in this file's header). kNotFound if
    /// it was already gone — a delete that silently succeeds on a missing row
    /// hides double-delete bugs.
    ///
    /// A task that is already tombstoned is reported as kNotFound even though
    /// a tombstone for its uid exists: from a caller's point of view "it is
    /// gone" is the truth, and the tombstone is bookkeeping, not a task.
    [[nodiscard]] Status deleteTask(std::int64_t id);

    /// Export every task and every tombstone as JSONL sorted by uid — this
    /// machine's complete backlog state, in the format TaskSync.hpp documents.
    ///
    /// One record per uid, sorted, is not cosmetic: it is what lets git merge
    /// two machines' exports line by line instead of conflicting wholesale.
    [[nodiscard]] Result<std::string> exportJsonl() const;

    /// Merge an export into this store, or report what a merge would do.
    ///
    /// `dry_run` takes every decision and counts every outcome but writes
    /// nothing, so its report is exactly what the same call with dry_run=false
    /// would produce. A dry run is only worth trusting if that holds — see
    /// tests/unit/test_task_store.cpp, which asserts the two agree.
    ///
    /// The whole merge is ONE transaction. Stopping halfway would leave the
    /// backlog in a state that is neither the old one nor the new one, and the
    /// next sync would treat that mixture as truth. A failure anywhere rolls
    /// everything back and reports the first problem, rather than applying the
    /// records that happened to parse.
    ///
    /// An unparseable line therefore fails the entire call with kInvalidArgument
    /// naming the line number — there is no partial merge.
    [[nodiscard]] Result<MergeReport> mergeJsonl(const std::string &jsonl,
                                                 bool dry_run);

    /// Fetch a task by its cross-machine uid. kNotFound if absent.
    ///
    /// The companion to getTask() for anything that arrived from another
    /// machine, where the integer id means nothing.
    [[nodiscard]] Result<Task> getTaskByUid(const std::string &uid) const;

    /// Number of tombstones, for get_status. Exposed because a tombstone count
    /// that only grows is the expected shape and a useful sanity signal — if it
    /// ever drops, a merge has rewritten history.
    [[nodiscard]] Result<std::size_t> tombstoneCount() const;

    /// Current ranking weights (from the settings table).
    [[nodiscard]] Result<Weights> weights() const;

    /// Persist new weights after validating them.
    [[nodiscard]] Status setWeights(const Weights &weights);

    /// Merge a partial weights patch onto the stored weights, validate the
    /// result and persist it — all under a single lock acquisition.
    ///
    /// This exists for the same reason updateTask is one locked
    /// read-modify-write rather than a getter plus a setter: a caller that read
    /// the weights, merged its own keys and wrote them back as separate calls
    /// releases the lock between the read and the write, so two concurrent
    /// partial updates can each snapshot the same value and the later write
    /// silently discards the earlier caller's key. A lost update like that is
    /// invisible from either side of the wire, which is what makes it worth a
    /// dedicated method instead of leaving the merge to the caller.
    ///
    /// Keys absent from `patch` keep the value already stored. Anything the
    /// weight parser rejects (a non-number, a non-finite or negative value, a
    /// horizon that is not > 0) fails with kInvalidArgument and leaves the
    /// stored weights exactly as they were — a rejected change is never
    /// half-applied.
    [[nodiscard]] Result<Weights> patchWeights(const nlohmann::json &patch);

    /// Tallies for the stats endpoint. `now` is passed in rather than read
    /// from a clock so "overdue" is measured against the same instant the
    /// caller used for ranking.
    [[nodiscard]] Result<Stats> stats(std::int64_t now) const;

    /// Filesystem path this store was opened with (":memory:" for tests).
    [[nodiscard]] const std::string &path() const { return m_path; }

  private:
    TaskStore(sqlite3 *db, std::string path);

    /// Execute one statement with no result rows; used for DDL and pragmas.
    [[nodiscard]] Status exec(const char *sql) const;

    sqlite3 *m_db{ nullptr };
    std::string m_path;

    /// Guards m_db for the lifetime of every public method. Mutable so const
    /// queries can lock too.
    mutable std::mutex m_mutex;
};

/// Serialize stats for the wire.
[[nodiscard]] nlohmann::json toJson(const Stats &stats);

/// Serialize a merge report for the wire. Titles are included so a dry run can
/// be reviewed; see MergeReport for why that matters.
[[nodiscard]] nlohmann::json toJson(const MergeReport &report);

} // namespace taskpilot
