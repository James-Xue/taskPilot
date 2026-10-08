// TaskStore.cpp — SQLite persistence for tasks and ranking weights
//
// Layering (see TaskStore.hpp): this file knows SQL and the Task struct. It
// does not rank and it does not know about JSON-RPC or MCP, so nothing here
// orders rows — ordering is PriorityEngine's job, and keeping that boundary is
// what lets a scoring change ship without a storage migration.
//
// Three rules shape the whole file:
//
// 1. Bind parameters; never build SQL out of user text. A task title is
//    arbitrary input, and a title like `it's 5'; DROP TABLE tasks; --` has to
//    round-trip byte for byte. The only SQL text assembled at runtime is the
//    SET clause of an UPDATE, and that concatenates compile-time constant
//    fragments — values still travel as binds.
// 2. Every public method holds m_mutex for its whole duration. The RPC server
//    runs one thread per connection, so two clients reach one store at once;
//    serializing here costs nothing at personal-backlog scale and removes a
//    class of corruption bugs that only shows up under load.
// 3. Failures are returned as Errors, never thrown and never ignored. An
//    invalid importance, a missing task id and a full disk are all expected
//    outcomes that have to become structured wire errors; an exception is
//    reserved for a genuine bug in this program.
// 4. A row this program did not write is read in the direction that cannot
//    corrupt the queue. `status` and the two nullable timestamps are the only
//    columns that can hold a value this class never produces, and each degrades
//    toward the HARMLESS end — an unrecognised status reads as archived, a
//    non-integer timestamp reads as no timestamp — because the alternative
//    (assuming the row is actionable, or that it is overdue) lets one corrupt
//    cell outrank real work. Every such degradation is reported on stderr, so
//    it is a documented read-path policy rather than a silent guess. See
//    readTaskRow and optionalInt64Column.
// 5. Synchronisation is a value-level operation, not a row-level one. A merge
//    matches on `uid` (a v4 uuid, the task's identity on every machine) and
//    never on `id` (this database's row number). That is why the migration in
//    open() exists, why the unique index is on uid, and why deleteTask leaves a
//    tombstone: without one, the other machine's export re-inserts the row you
//    deliberately deleted. See TaskSync.hpp for the format and the rule.

#include "core/TaskStore.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

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
// Schema
// ---------------------------------------------------------------------------

/// Applied on every open, in order, BEFORE the uid migration below.
///
/// Every statement is IF NOT EXISTS, so an existing database gains only the
/// objects it is missing: a database written by an older build keeps its rows
/// and its shape, and the migration is what changes those.
///
/// The DEFAULT clauses duplicate the struct defaults in Task.hpp on purpose:
/// they are the safety net for a row inserted by something other than this
/// class (a hand-run sqlite3 shell), not the primary path.
///
/// One statement is deliberately ABSENT here: the UNIQUE index on tasks(uid).
/// A version-0 database's tasks table has no uid column, so creating that
/// index at this point would fail with "no such column: uid" before the
/// migration had a chance to add it — the failure mode the ordering comment in
/// migrateUidColumn() describes. The index is created there instead, after the
/// column exists and every row carries a value. The tombstones table has no
/// such dependency, so it is created right here.
///
/// `uid` is nullable, not NOT NULL. Not a preference: SQLite refuses to add a
/// NOT NULL column with ALTER TABLE unless it carries a DEFAULT, and a DEFAULT
/// uuid would give every pre-existing row the same value — the one outcome the
/// migration exists to prevent. Nullability is therefore bought on purpose and
/// the invariant is enforced instead by the unique index plus the fact that
/// every write path here supplies a uid (backfill on migration, generateUuidV4
/// on insert, the record's validated uid on merge).
constexpr const char *kSchemaStatements[] = {
    R"sql(
CREATE TABLE IF NOT EXISTS tasks (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    uid          TEXT,
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
))sql",
    R"sql(
CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
))sql",
    // One row per deleted task, forever: see TaskStore.hpp for why a delete
    // that left no trace would be undone by the next merge (the other machine
    // still holds the task and exports it as an insert). uid is the PRIMARY
    // KEY because a task can be deleted exactly once per name; re-deleting with
    // INSERT OR REPLACE just refreshes the stamp rather than piling up rows.
    R"sql(
CREATE TABLE IF NOT EXISTS tombstones (
    uid        TEXT PRIMARY KEY,
    deleted_at INTEGER NOT NULL
))sql",
    // One index per filter listTasks actually uses. Both are cheap to maintain
    // and turn the two common scans (the queue's actionable set, the overdue
    // tally) into range lookups as the backlog grows.
    "CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status)",
    "CREATE INDEX IF NOT EXISTS idx_tasks_due_at ON tasks(due_at)",
};

/// The schema version this build expects the database to be at, and writes
/// after a successful migration. 1 added `tasks.uid`, the unique index on it,
/// and the tombstones table.
///
/// The stored row is a RECORD, not the authority. A database created by the
/// build that predates the uid column already carries the string "1" here —
/// that build wrote it under a "nothing reads it yet" comment — so a migration
/// that trusted the settings row would find "already at 1" on exactly the
/// database that needs migrating. The authority is instead the shape of the
/// table (does tasks have a uid column?), which cannot lie, and this row is
/// updated to match what the migration actually achieved. See
/// migrateUidColumn().
constexpr const char *kSchemaVersion = "1";

/// Settings key holding the schema version.
constexpr const char *kSchemaVersionKey = "schema_version";

/// Settings key holding the serialized Weights.
constexpr const char *kWeightsKey = "weights";

// ---------------------------------------------------------------------------
// Column layout
// ---------------------------------------------------------------------------

/// Offsets inside kTaskColumns. They are positional, so the column list and
/// this enumeration have to change together — that is why both live side by
/// side here instead of being spelled out in each query.
constexpr int kColId = 0;
constexpr int kColUid = 1;
constexpr int kColTitle = 2;
constexpr int kColNotes = 3;
constexpr int kColStatus = 4;
constexpr int kColImportance = 5;
constexpr int kColDueAt = 6;
constexpr int kColBlocks = 7;
constexpr int kColTags = 8;
constexpr int kColCreatedAt = 9;
constexpr int kColUpdatedAt = 10;
constexpr int kColCompletedAt = 11;

/// The column list every task query selects, in the order readTaskRow expects.
/// `uid` sits next to `id` because they are the two identities a task has, and
/// keeping them adjacent is what stops the next reader from treating one as the
/// other: `id` is this database's row number, `uid` is the task's name
/// everywhere (Task.hpp). Nothing here ever matches on `id` across machines.
constexpr const char *kTaskColumns =
    "id, uid, title, notes, status, importance, due_at, blocks, tags, created_at, updated_at, completed_at";

// ---------------------------------------------------------------------------
// Validation bounds
// ---------------------------------------------------------------------------

/// Boundaries the validation below enforces. Named rather than inlined so the
/// comparison and the error text cannot drift apart.
constexpr int kMinImportance{ 1 };
constexpr int kMaxImportance{ 5 };
constexpr int kMinBlocks{ 0 };

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

/// Strip leading and trailing whitespace. Used only by validation: the store
/// deliberately does not rewrite caller data, because "is this title usable"
/// and "what should this title say" are different questions and only the
/// caller can answer the second one.
[[nodiscard]] std::string trim(const std::string &text)
{
    // std::isspace takes an int in the range of unsigned char; passing a plain
    // char would be undefined for bytes >= 0x80, so widen explicitly.
    const auto not_space = [](unsigned char character) { return 0 == std::isspace(character); };

    const auto first = std::find_if(text.begin(), text.end(), not_space);
    // The reverse search finds the last non-space byte; base() converts back
    // to a forward iterator pointing one past it.
    const auto last = std::find_if(text.rbegin(), text.rend(), not_space).base();
    if (first >= last) {
        return std::string{};
    }
    return std::string(first, last);
}

/// Wall-clock seconds, sampled inside the store.
///
/// Note the asymmetry with the ranking path: PriorityEngine receives `now` as
/// an argument because a score must be reproducible for a given instant, while
/// a stored created_at/updated_at only has to be sane in practice. That is why
/// this class samples its own clock instead of taking one — and why the tests
/// can assert "assigned, not what the caller passed" without a FixedClock.
[[nodiscard]] std::int64_t nowEpochSeconds()
{
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
    return static_cast<std::int64_t>(seconds.count());
}

/// Read a TEXT column as a std::string.
[[nodiscard]] std::string textColumn(sqlite3_stmt *stmt, int index)
{
    const unsigned char *text = sqlite3_column_text(stmt, index);
    if (nullptr == text) {
        return std::string{};
    }
    // SQLite documents that column_bytes must be called after the text has
    // been materialized (as above), and using it rather than strlen keeps text
    // containing an embedded NUL intact.
    const int bytes = sqlite3_column_bytes(stmt, index);
    return std::string(reinterpret_cast<const char *>(text), static_cast<std::size_t>(bytes));
}

/// Read the nullable INTEGER column at `index` for the row whose id is
/// `row_id` (used only to name the row in the warning below).
///
/// Only a real SQLITE_INTEGER yields a value: SQL NULL and every other storage
/// class yield nullopt, i.e. "this cell holds no timestamp".
///
/// That strictness is the whole point. sqlite3_column_int64 silently coerces
/// TEXT and BLOB into an integer — the longest numeric prefix, else 0 — so a
/// due_at cell holding '2026-10-08' reads as 2026 and one holding '' or 'today'
/// reads as 0. Every one of those is a 1970 timestamp, i.e. decades overdue.
/// Absence has to mean ABSENCE here: a task with no deadline scores ZERO
/// urgency (docs/scoring.md's first urgency rule, and PriorityEngine::
/// urgencyFactor's first case), which is exactly what stops undated work from
/// outranking work with a real deadline. A coerced 1970 does the opposite —
/// urgency saturates at 1.0, the largest term in the model — so one corrupt
/// byte would hijack the top of the queue.
///
/// Degrading toward "no deadline" is therefore the safe direction: the worst
/// case is an undated task sinking in the queue, which is visible and harmless,
/// while the alternative promotes corrupt data to the front of it. The warning
/// is what keeps the degradation honest instead of silent — it names the row
/// and the raw text so the cell can be found and repaired, rather than quietly
/// scoring as something it is not.
[[nodiscard]] std::optional<std::int64_t> optionalInt64Column(sqlite3_stmt *stmt, int index,
                                                              const char *column,
                                                              std::int64_t row_id)
{
    // The storage class is sampled before anything renders the cell as text,
    // so the decision below is made on what sqlite actually stored and never on
    // a conversion this function caused.
    const int storage_class = sqlite3_column_type(stmt, index);
    if (SQLITE_NULL == storage_class) {
        return std::nullopt;
    }
    if (SQLITE_INTEGER == storage_class) {
        return sqlite3_column_int64(stmt, index);
    }

    // textColumn covers the remaining classes: sqlite renders a REAL or a BLOB
    // as text on request, so the warning can show whatever the cell holds.
    std::cerr << "taskpilot: task " << row_id << " has a non-integer " << column << " value ('"
              << textColumn(stmt, index) << "'); reading it as no timestamp\n";
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Statement
// ---------------------------------------------------------------------------

/// RAII wrapper around one prepared statement.
///
/// Every early return in this file would otherwise need its own
/// sqlite3_finalize call, and a missed one leaks the statement and (for a
/// SELECT) its read lock. Holding the handle in an object whose destructor
/// finalizes it makes "no leaks on the error path" a property of the type
/// instead of a property of the author's memory.
class Statement
{
  public:
    Statement() = default;
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;
    Statement(Statement &&) = delete;
    Statement &operator=(Statement &&) = delete;

    ~Statement() { finalize(); }

    /// Compile `sql`. On failure the handle stays null and the error carries
    /// both sqlite's own message and the offending text.
    ///
    /// Echoing the SQL is safe because the text is always a constant in this
    /// file or built from constants: user values only ever travel as binds.
    [[nodiscard]] Status prepare(sqlite3 *db, const std::string &sql)
    {
        finalize();
        m_db = db;
        // A length of -1 means "the text is NUL-terminated", which avoids
        // narrowing sql.size() to int for no benefit.
        if (SQLITE_OK != sqlite3_prepare_v2(db, sql.c_str(), -1, &m_stmt, nullptr)) {
            const Error failure = Error::storageFailure(std::string("failed to prepare SQL (") +
                                                       sqlite3_errmsg(db) + "): " + sql);
            finalize();
            return failure;
        }
        return Unit{};
    }

    [[nodiscard]] sqlite3_stmt *handle() const { return m_stmt; }
    /// Borrowed handle, used only to build error messages.
    [[nodiscard]] sqlite3 *db() const { return m_db; }

  private:
    void finalize()
    {
        if (nullptr != m_stmt) {
            sqlite3_finalize(m_stmt);
            m_stmt = nullptr;
        }
    }

    sqlite3_stmt *m_stmt{ nullptr };
    sqlite3 *m_db{ nullptr };
};

// ---------------------------------------------------------------------------
// Statements and transactions against a raw handle
// ---------------------------------------------------------------------------
//
// These take a sqlite3* rather than the store because the open() path needs
// them before a TaskStore is handed to anyone. Locking is the caller's
// business, exactly as it is for the settings helpers below.

/// Execute one statement with no result rows.
///
/// Split out of TaskStore::exec so the open()-path helpers and the transaction
/// class can use it too. `sql` is always a compile-time constant or built from
/// constants in this file; values travel as binds.
[[nodiscard]] Status execWith(sqlite3 *db, const std::string &sql)
{
    char *message = nullptr;
    if (SQLITE_OK != sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message)) {
        // sqlite3_exec hands back an allocated message; copy it before freeing
        // so the returned Error owns its text.
        const std::string detail = (nullptr != message) ? message : sqlite3_errmsg(db);
        sqlite3_free(message);
        return Error::storageFailure(std::string("sqlite3_exec failed: ") + detail);
    }
    return Unit{};
}

/// A write transaction that rolls back by default.
///
/// Both operations that need one — the merge, and deleteTask's row+tombstone
/// pair — have several early returns in between, and every one of them must
/// undo. Holding the transaction in an object makes "nothing half-applied
/// escapes" a property of the type instead of a property of the author's
/// memory, which is the same reason Statement owns its finalize.
///
/// commit() is deliberately explicit. A transaction that committed on scope
/// exit would turn a forgotten line into a half-finished merge, and a
/// half-finished merge is precisely the state the next sync would treat as
/// truth.
class Transaction
{
  public:
    explicit Transaction(sqlite3 *db) : m_db{ db } {}

    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    ~Transaction()
    {
        if (m_finished) {
            return;
        }
        // No error channel out of a destructor, so a failed rollback goes to
        // stderr rather than nowhere: the connection would still be inside a
        // transaction, and the next statement would silently run in it.
        char *message = nullptr;
        if (SQLITE_OK != sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, &message)) {
            std::cerr << "taskpilot: failed to roll back a transaction: "
                      << (nullptr != message ? message : sqlite3_errmsg(m_db)) << "\n";
        }
        sqlite3_free(message);
    }

    /// BEGIN IMMEDIATE, not a deferred BEGIN: a deferred transaction takes the
    /// write lock at its first write, so this transaction could read, then lose
    /// the upgrade and fail with SQLITE_BUSY *after* those reads — a failure
    /// sqlite cannot retry through busy_timeout. Taking the reserved lock up
    /// front moves the wait to BEGIN, where retrying is safe.
    [[nodiscard]] Status begin() { return execWith(m_db, "BEGIN IMMEDIATE;"); }

    /// End the transaction successfully. Marks it finished whether the COMMIT
    /// succeeded or not: after a failed COMMIT sqlite has already rolled the
    /// transaction back, so the destructor must not attempt a second one, and
    /// the caller is the one that has to report the failure.
    [[nodiscard]] Status commit()
    {
        m_finished = true;
        return execWith(m_db, "COMMIT;");
    }

    /// A dry run simply never calls commit(): the destructor's rollback is the
    /// whole of "decide everything, write nothing", so there is no second path
    /// that could get the difference wrong.

  private:
    sqlite3 *m_db{ nullptr };
    bool m_finished{ false };
};

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

[[nodiscard]] Error bindFailure(const char *type, int index, sqlite3 *db)
{
    return Error::storageFailure(std::string("failed to bind ") + type + " parameter " +
                                 std::to_string(index) + ": " + sqlite3_errmsg(db));
}

/// Bind text with SQLITE_TRANSIENT: sqlite copies the bytes, so the std::string
/// does not have to outlive the step. The explicit length (rather than -1)
/// keeps text containing an embedded NUL whole.
[[nodiscard]] Status bindText(const Statement &stmt, int index, const std::string &value)
{
    const int length = static_cast<int>(value.size());
    if (SQLITE_OK != sqlite3_bind_text(stmt.handle(), index, value.c_str(), length, SQLITE_TRANSIENT)) {
        return bindFailure("text", index, stmt.db());
    }
    return Unit{};
}

[[nodiscard]] Status bindInt(const Statement &stmt, int index, int value)
{
    if (SQLITE_OK != sqlite3_bind_int(stmt.handle(), index, value)) {
        return bindFailure("integer", index, stmt.db());
    }
    return Unit{};
}

[[nodiscard]] Status bindInt64(const Statement &stmt, int index, std::int64_t value)
{
    if (SQLITE_OK != sqlite3_bind_int64(stmt.handle(), index, value)) {
        return bindFailure("integer", index, stmt.db());
    }
    return Unit{};
}

[[nodiscard]] Status bindNull(const Statement &stmt, int index)
{
    if (SQLITE_OK != sqlite3_bind_null(stmt.handle(), index)) {
        return bindFailure("null", index, stmt.db());
    }
    return Unit{};
}

/// One value queued for binding.
///
/// updateTask builds its SET clause from whichever patch fields are present,
/// so the SQL text is only final once the whole clause list is assembled — the
/// values therefore cannot be bound as they are decided. Queuing them carries
/// the SQL fragments and their values through one pass in one order, which is
/// the only thing that keeps a dynamic UPDATE correct when a field is added.
struct BoundValue
{
    enum class Kind
    {
        kText,  ///< std::string in `text`.
        kInt,   ///< int in `number`.
        kInt64, ///< std::int64_t in `number`.
        kNull,  ///< SQL NULL; `number` is unused.
    };

    Kind kind{ Kind::kInt };
    std::string text;
    std::int64_t number{ 0 };
};

[[nodiscard]] BoundValue textValue(std::string text)
{
    BoundValue value;
    value.kind = BoundValue::Kind::kText;
    value.text = std::move(text);
    return value;
}

[[nodiscard]] BoundValue intValue(int number)
{
    BoundValue value;
    value.kind = BoundValue::Kind::kInt;
    value.number = number;
    return value;
}

[[nodiscard]] BoundValue int64Value(std::int64_t number)
{
    BoundValue value;
    value.kind = BoundValue::Kind::kInt64;
    value.number = number;
    return value;
}

[[nodiscard]] BoundValue nullValue()
{
    BoundValue value;
    value.kind = BoundValue::Kind::kNull;
    return value;
}

/// Bind `values` to parameters 1..N in order.
[[nodiscard]] Status bindValues(const Statement &stmt, const std::vector<BoundValue> &values)
{
    int index = 1;
    for (const BoundValue &value : values) {
        Status bound = Unit{};
        // Every Kind is handled, so a new kind is caught by -Wswitch rather
        // than by a silently unbound parameter.
        switch (value.kind) {
        case BoundValue::Kind::kText:
            bound = bindText(stmt, index, value.text);
            break;
        case BoundValue::Kind::kInt:
            bound = bindInt(stmt, index, static_cast<int>(value.number));
            break;
        case BoundValue::Kind::kInt64:
            bound = bindInt64(stmt, index, value.number);
            break;
        case BoundValue::Kind::kNull:
            bound = bindNull(stmt, index);
            break;
        }
        if (!bound.ok()) {
            return bound.error();
        }
        ++index;
    }
    return Unit{};
}

/// Step a statement that is expected to complete without returning rows.
[[nodiscard]] Status stepDone(const Statement &stmt, const char *operation)
{
    if (SQLITE_DONE != sqlite3_step(stmt.handle())) {
        return Error::storageFailure(std::string(operation) + " failed: " + sqlite3_errmsg(stmt.db()));
    }
    return Unit{};
}

// ---------------------------------------------------------------------------
// Settings rows
// ---------------------------------------------------------------------------
//
// These take a raw handle rather than the store so open() can use them before
// the store is handed to anyone. Locking is the caller's business: open() is
// single-threaded by construction, and the public methods lock before calling.

[[nodiscard]] Result<bool> settingExists(sqlite3 *db, const char *key)
{
    Statement stmt;
    const Status prepared = stmt.prepare(db, "SELECT 1 FROM settings WHERE key = ?");
    if (!prepared.ok()) {
        return prepared.error();
    }
    const Status bound = bindText(stmt, 1, std::string(key));
    if (!bound.ok()) {
        return bound.error();
    }
    const int result = sqlite3_step(stmt.handle());
    if (SQLITE_ROW == result) {
        return true;
    }
    if (SQLITE_DONE == result) {
        return false;
    }
    return Error::storageFailure(std::string("failed to look up setting '") + key +
                                 "': " + sqlite3_errmsg(db));
}

/// Read one settings row. kNotFound when the key is absent, so the caller
/// decides whether that is fatal (weights) or normal.
[[nodiscard]] Result<std::string> readSetting(sqlite3 *db, const char *key)
{
    Statement stmt;
    const Status prepared = stmt.prepare(db, "SELECT value FROM settings WHERE key = ?");
    if (!prepared.ok()) {
        return prepared.error();
    }
    const Status bound = bindText(stmt, 1, std::string(key));
    if (!bound.ok()) {
        return bound.error();
    }
    const int result = sqlite3_step(stmt.handle());
    if (SQLITE_ROW == result) {
        return textColumn(stmt.handle(), 0);
    }
    if (SQLITE_DONE == result) {
        return Error::notFound(std::string("setting '") + key + "' is not present");
    }
    return Error::storageFailure(std::string("failed to read setting '") + key +
                                 "': " + sqlite3_errmsg(db));
}

/// Insert or replace one settings row.
///
/// ON CONFLICT DO UPDATE rather than INSERT OR REPLACE because it is an
/// explicit upsert: it says "this key now has this value" without also
/// inviting the DELETE-then-INSERT semantics that would fire a row-delete
/// trigger if one ever existed.
[[nodiscard]] Status writeSetting(sqlite3 *db, const char *key, const std::string &value)
{
    Statement stmt;
    const Status prepared = stmt.prepare(db, "INSERT INTO settings(key, value) VALUES(?, ?) "
                                             "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    if (!prepared.ok()) {
        return prepared.error();
    }
    const Status key_bound = bindText(stmt, 1, std::string(key));
    if (!key_bound.ok()) {
        return key_bound.error();
    }
    const Status value_bound = bindText(stmt, 2, value);
    if (!value_bound.ok()) {
        return value_bound.error();
    }
    return stepDone(stmt, "write setting");
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

/// Materialize the current row of `stmt`.
///
/// The columns that can hold a value this class never writes — `status`, and
/// the two nullable timestamps — degrade toward the harmless end rather than
/// failing the query, matching the policy parseTags documents: one bad row must
/// not hide the rest of the backlog. Failing the whole list instead would turn
/// a single hand-edited cell into an outage. See optionalInt64Column for the
/// timestamps, and the status branch below for why it lands on kArchived.
[[nodiscard]] Task readTaskRow(sqlite3_stmt *stmt)
{
    Task task;
    task.id = sqlite3_column_int64(stmt, kColId);
    task.uid = textColumn(stmt, kColUid);
    task.title = textColumn(stmt, kColTitle);
    task.notes = textColumn(stmt, kColNotes);

    // An unrecognised status becomes kArchived — never the kOpen this used to
    // default to. Task.hpp's parser contract returns nullopt precisely so the
    // caller decides, and its comment spells out the harm of the other choice:
    // a corrupt row defaulting to kOpen masquerades as active work in the
    // ranked queue. kArchived is the only status that is both
    //   1. non-actionable, so the row can never be promoted into the queue
    //      (Task::isActionable is false for it), and
    //   2. non-destructive, so the row is not dropped: an unfiltered listTasks
    //      still returns it, which keeps it in view instead of hiding it.
    //
    // Note what kArchived does NOT do here: nothing else in this layer keys on
    // the degraded value. The status filter and the stats tallies both compare
    // the STORED text in SQL, so a corrupt row is outside the filtered view and
    // outside every bucket. The warning below — not the new status — is
    // therefore what makes a corrupt status discoverable, and it is the reason
    // this is a read-path DEGRADATION rather than a silent default: the row is
    // never lost, and an unrecognised one is always reported.
    const std::string raw_status = textColumn(stmt, kColStatus);
    const std::optional<TaskStatus> parsed_status = taskStatusFromString(raw_status);
    if (parsed_status.has_value()) {
        task.status = *parsed_status;
    } else {
        std::cerr << "taskpilot: task " << task.id << " has an unrecognised status ('" << raw_status
                  << "'); reading it as archived\n";
        task.status = TaskStatus::kArchived;
    }

    task.importance = sqlite3_column_int(stmt, kColImportance);
    task.due_at = optionalInt64Column(stmt, kColDueAt, "due_at", task.id);
    task.blocks = sqlite3_column_int(stmt, kColBlocks);
    task.tags = parseTags(textColumn(stmt, kColTags));
    task.created_at = sqlite3_column_int64(stmt, kColCreatedAt);
    task.updated_at = sqlite3_column_int64(stmt, kColUpdatedAt);
    task.completed_at = optionalInt64Column(stmt, kColCompletedAt, "completed_at", task.id);
    return task;
}

/// Fetch one row inside an already-held lock. Used by every public method that
/// needs the row it just wrote; taking the lock again there would deadlock, so
/// the locking lives in the callers.
[[nodiscard]] Result<Task> selectTaskById(sqlite3 *db, std::int64_t id)
{
    Statement stmt;
    const Status prepared =
        stmt.prepare(db, std::string("SELECT ") + kTaskColumns + " FROM tasks WHERE id = ?");
    if (!prepared.ok()) {
        return prepared.error();
    }
    const Status bound = bindInt64(stmt, 1, id);
    if (!bound.ok()) {
        return bound.error();
    }
    const int result = sqlite3_step(stmt.handle());
    if (SQLITE_ROW == result) {
        return readTaskRow(stmt.handle());
    }
    if (SQLITE_DONE == result) {
        return Error::notFound("no task with id " + std::to_string(id));
    }
    return Error::storageFailure("failed to read task " + std::to_string(id) + ": " +
                                 sqlite3_errmsg(db));
}

/// Fetch one row by its cross-machine uid, inside an already-held lock.
///
/// The uid is the match key for everything that arrived from another machine:
/// getTaskByUid, export and merge all funnel through here rather than through
/// selectTaskById, because an integer id means "this database's row 9" and
/// matching two machines on one is the exact confusion Task.hpp warns about.
[[nodiscard]] Result<Task> selectTaskByUid(sqlite3 *db, const std::string &uid)
{
    Statement stmt;
    const Status prepared =
        stmt.prepare(db, std::string("SELECT ") + kTaskColumns + " FROM tasks WHERE uid = ?");
    if (!prepared.ok()) {
        return prepared.error();
    }
    const Status bound = bindText(stmt, 1, uid);
    if (!bound.ok()) {
        return bound.error();
    }
    const int result = sqlite3_step(stmt.handle());
    if (SQLITE_ROW == result) {
        return readTaskRow(stmt.handle());
    }
    if (SQLITE_DONE == result) {
        return Error::notFound("no task with uid " + uid);
    }
    return Error::storageFailure("failed to read the task with uid " + uid + ": " +
                                 sqlite3_errmsg(db));
}

/// Everything the local store knows about one uid, as decide() wants it.
struct LocalLookup
{
    LocalState state;
    Task task; ///< The live row when state.present; default-constructed otherwise.
};

/// Read the local side of one uid: the live row and/or its tombstone.
///
/// Both tables are consulted because the decision table distinguishes "never
/// seen" from "deleted": absence from `tasks` alone cannot tell those apart, and
/// reading a deleted-and-tombstoned uid as new is exactly how a deleted task
/// comes back at the next sync (TaskStore.hpp's tombstones note).
///
/// LocalState carries a single stamp, and decide() reads it against whichever
/// side matched. When a live row AND a tombstone both exist for one uid — only
/// reachable by editing the database by hand, since no path here produces it —
/// the live row owns the stamp: it is the state the merge is reconciling, and
/// the branch decide() will take for it is the live-row one.
[[nodiscard]] Result<LocalLookup> lookupLocal(sqlite3 *db, const std::string &uid)
{
    LocalLookup lookup;

    // 1. The live row, if any. Finding nothing is the normal case for an
    //    unknown uid, not a failure, so absence leaves `present` false instead
    //    of becoming an error.
    const Result<Task> live = selectTaskByUid(db, uid);
    if (live.ok()) {
        lookup.task = live.value();
        lookup.state.present = true;
        lookup.state.stamp = lookup.task.updated_at;
    } else if (ErrorCode::kNotFound != live.error().code) {
        // Only absence is expected here; a failed SELECT must not be read as
        // "the task is not there", which would turn a broken query into an
        // insert of a task that already exists.
        return live.error();
    }

    // 2. The tombstone, if any. Same absence-is-normal shape.
    Statement stmt;
    const Status prepared = stmt.prepare(db, "SELECT deleted_at FROM tombstones WHERE uid = ?");
    if (!prepared.ok()) {
        return prepared.error();
    }
    const Status bound = bindText(stmt, 1, uid);
    if (!bound.ok()) {
        return bound.error();
    }
    const int result = sqlite3_step(stmt.handle());
    if (SQLITE_ROW == result) {
        lookup.state.tombstoned = true;
        if (!lookup.state.present) {
            // deleted_at is NOT NULL, so a row always carries a usable stamp.
            lookup.state.stamp = sqlite3_column_int64(stmt.handle(), 0);
        }
    } else if (SQLITE_DONE != result) {
        return Error::storageFailure("failed to read the tombstone for uid " + uid + ": " +
                                     sqlite3_errmsg(db));
    }

    return lookup;
}

/// The one validation every write path shares.
///
/// addTask and updateTask both funnel their final, merged values through here,
/// which is what makes the header's "no caller can bypass it" true: an update
/// cannot smuggle an importance of 9 into the table by touching another field.
/// Messages name the offending field and value so a client — usually an LLM —
/// can fix the call without guessing.
///
/// Trimming is a test, not a transformation: the store validates "title is not
/// blank" without rewriting what the caller asked to store, because a store
/// that silently edits user text is harder to reason about than one that
/// rejects it.
[[nodiscard]] Status validateTaskFields(const std::string &title, int importance, int blocks,
                                        const std::vector<std::string> &tags)
{
    // 1. Title: whitespace is not content. A title of "   " is a caller bug
    //    (a template that collapsed, usually) and storing it would put an
    //    invisible row in the backlog.
    if (trim(title).empty()) {
        return Error::invalidArgument("title must not be empty or whitespace only (got \"" + title + "\")");
    }
    // 2. Importance is the ranking term with a fixed 1..5 domain; a value
    //    outside it would make two tasks' scores incomparable.
    if (kMinImportance > importance || kMaxImportance < importance) {
        return Error::invalidArgument("importance must be between " + std::to_string(kMinImportance) +
                                      " and " + std::to_string(kMaxImportance) + " (got " +
                                      std::to_string(importance) + ")");
    }
    // 3. blocks counts downstream dependents, so it cannot be negative.
    if (kMinBlocks > blocks) {
        return Error::invalidArgument("blocks must be " + std::to_string(kMinBlocks) + " or more (got " +
                                      std::to_string(blocks) + ")");
    }
    // 4. An empty tag is not a tag: it would also match any filter that passes
    //    an empty string, which is worse than no filter at all.
    for (std::size_t index = 0; index < tags.size(); ++index) {
        if (trim(tags[index]).empty()) {
            // The value is quoted so the caller can see how much whitespace it
            // sent — the delimiters are the only way a blank string shows its
            // shape in a message.
            return Error::invalidArgument("tags[" + std::to_string(index) +
                                          "] must not be empty or whitespace only (got \"" +
                                          tags[index] + "\")");
        }
    }
    return Unit{};
}

// ---------------------------------------------------------------------------
// Carrying out a merge
// ---------------------------------------------------------------------------
//
// The decisions themselves are TaskSync::decide()'s — a total function over
// (local state, incoming record) that is tested without a database. The
// helpers below only execute what it returns, which keeps every SQL statement
// here mechanical: if a merge ever deletes the wrong thing, the bug is in the
// decision table, not in a query.

/// Insert a row from an export record, preserving the record's history.
///
/// The uid comes from the record — that is the entire point of a merge — while
/// the integer id does not: it is a local row number, the record carries none
/// by design (SyncRecord::task explains why a transported id would be a trap),
/// and AUTOINCREMENT assigns the next free one here. created_at, completed_at
/// and the text fields are copied verbatim so a task keeps the history it was
/// created with on the machine it was created on.
///
/// updated_at is taken from `record.stamp` rather than from the task's own
/// copy of the field. For a live record the two are the same value by contract
/// (TaskSync.hpp defines the stamp as task.updated_at), and where they could
/// ever disagree the stamp is the one decide() compared against — storing the
/// other would leave the row disagreeing with the decision that wrote it, and
/// a later merge would compare against a number the first one never saw.
[[nodiscard]] Status insertSyncedTask(sqlite3 *db, const SyncRecord &record)
{
    Statement stmt;
    const Status prepared =
        stmt.prepare(db, "INSERT INTO tasks(uid, title, notes, status, importance, due_at, blocks, "
                         "tags, created_at, updated_at, completed_at) "
                         "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!prepared.ok()) {
        return prepared.error();
    }

    const std::vector<BoundValue> values = {
        textValue(record.uid),
        textValue(record.task.title),
        textValue(record.task.notes),
        textValue(toString(record.task.status)),
        intValue(record.task.importance),
        record.task.due_at.has_value() ? int64Value(*record.task.due_at) : nullValue(),
        intValue(record.task.blocks),
        textValue(serializeTags(record.task.tags)),
        int64Value(record.task.created_at),
        int64Value(record.stamp),
        record.task.completed_at.has_value() ? int64Value(*record.task.completed_at) : nullValue(),
    };
    const Status bound = bindValues(stmt, values);
    if (!bound.ok()) {
        return bound.error();
    }
    return stepDone(stmt, "insert a merged task");
}

/// Overwrite the mutable fields of the row for `record.uid` from the record.
///
/// Deliberately not written: `id` (a local row number that must not move),
/// `uid` (the match key — changing it would delete the identity the row was
/// found by), and `created_at` (the one immutable fact about a task; when the
/// two machines disagree it is because one of them is wrong, and copying a
/// wrong value over a right one buys nothing). Everything a user can change —
/// the text, the ranking fields, the status, the completion stamp — is copied,
/// together with updated_at, which is what makes the local row an exact copy of
/// the record the other machine exported.
[[nodiscard]] Status updateSyncedTask(sqlite3 *db, const SyncRecord &record)
{
    Statement stmt;
    const Status prepared =
        stmt.prepare(db, "UPDATE tasks SET title = ?, notes = ?, status = ?, importance = ?, "
                         "due_at = ?, blocks = ?, tags = ?, updated_at = ?, completed_at = ? "
                         "WHERE uid = ?");
    if (!prepared.ok()) {
        return prepared.error();
    }

    const std::vector<BoundValue> values = {
        textValue(record.task.title),
        textValue(record.task.notes),
        textValue(toString(record.task.status)),
        intValue(record.task.importance),
        record.task.due_at.has_value() ? int64Value(*record.task.due_at) : nullValue(),
        intValue(record.task.blocks),
        textValue(serializeTags(record.task.tags)),
        int64Value(record.stamp),
        record.task.completed_at.has_value() ? int64Value(*record.task.completed_at) : nullValue(),
        textValue(record.uid),
    };
    const Status bound = bindValues(stmt, values);
    if (!bound.ok()) {
        return bound.error();
    }
    return stepDone(stmt, "update a merged task");
}

/// Remove the live row for `uid` (if any) and record the deletion.
///
/// Shared by deleteTask and the merge's kDelete, because both have to leave the
/// database in the same state: no row, and exactly one tombstone whose stamp is
/// at least as new as the row was. Splitting that pair would allow a row to be
/// deleted here and re-inserted by the next merge from the other machine — the
/// resurrection tombstones exist to prevent, and the reason the two statements
/// belong in one transaction (see deleteTask).
///
/// INSERT OR REPLACE rather than INSERT: a uid can be deleted more than once
/// across a delete/resurrect cycle, and the newest stamp is the one the next
/// merge has to compare against.
[[nodiscard]] Status removeTaskByUid(sqlite3 *db, const std::string &uid, std::int64_t deleted_at)
{
    Statement remove;
    const Status prepared_remove = remove.prepare(db, "DELETE FROM tasks WHERE uid = ?");
    if (!prepared_remove.ok()) {
        return prepared_remove.error();
    }
    const Status bound_remove = bindText(remove, 1, uid);
    if (!bound_remove.ok()) {
        return bound_remove.error();
    }
    const Status deleted = stepDone(remove, "delete task");
    if (!deleted.ok()) {
        return deleted.error();
    }

    // A delete that removed nothing is reported the same way deleteTask reports
    // an unknown id: from the caller's side "it is gone" is the truth, but a
    // later tombstone write for a row that never existed here would be a
    // deletion travelling from a state that never was.
    if (0 == sqlite3_changes(db)) {
        return Error::notFound("no live task with uid " + uid);
    }

    Statement remember;
    const Status prepared_remember =
        remember.prepare(db, "INSERT OR REPLACE INTO tombstones(uid, deleted_at) VALUES(?, ?)");
    if (!prepared_remember.ok()) {
        return prepared_remember.error();
    }
    const Status bound_uid = bindText(remember, 1, uid);
    if (!bound_uid.ok()) {
        return bound_uid.error();
    }
    const Status bound_stamp = bindInt64(remember, 2, deleted_at);
    if (!bound_stamp.ok()) {
        return bound_stamp.error();
    }
    return stepDone(remember, "record a deletion");
}

/// Append one example title to a report list, honoring the cap.
///
/// The counts stay exact; only the examples are truncated. The cap is what
/// keeps a first sync of a large file from turning into a multi-megabyte reply
/// (kMergeReportTitleLimit), and it is applied here rather than by the caller
/// so no future count can forget it.
void rememberTitle(std::vector<std::string> &titles, const std::string &title)
{
    if (kMergeReportTitleLimit > titles.size()) {
        titles.push_back(title);
    }
}

/// Carry out decide() for every record, inside an already-open transaction.
///
/// Each record is looked up fresh, in order, rather than against a snapshot
/// taken up front. A file with two lines naming the same uid — hand-edited, or
/// written by a buggy exporter — is then processed as what it is: the second
/// line sees the state the first one left, which is the only reading under
/// which the file's final state is a function of its lines.
///
/// Every branch increments its counter and, except kSkip, records an example
/// title. For kDelete that title comes from the LOCAL row, because the
/// tombstone carries none and the point of a dry run is to show what is about
/// to stop existing.
[[nodiscard]] Status applyMergeRecords(sqlite3 *db, const std::vector<SyncRecord> &records,
                                       MergeReport &report)
{
    for (const SyncRecord &record : records) {
        const Result<LocalLookup> local = lookupLocal(db, record.uid);
        if (!local.ok()) {
            return local.error();
        }

        // No default: label, so a new SyncAction is a build failure here rather
        // than a record silently doing nothing.
        switch (decide(local.value().state, record)) {
        case SyncAction::kInsert: {
            const Status inserted = insertSyncedTask(db, record);
            if (!inserted.ok()) {
                return inserted.error();
            }
            ++report.inserted;
            rememberTitle(report.inserted_titles, record.task.title);
            break;
        }
        case SyncAction::kUpdate: {
            const Status updated = updateSyncedTask(db, record);
            if (!updated.ok()) {
                return updated.error();
            }
            ++report.updated;
            rememberTitle(report.updated_titles, record.task.title);
            break;
        }
        case SyncAction::kDelete: {
            const Status removed = removeTaskByUid(db, record.uid, record.stamp);
            if (!removed.ok()) {
                return removed.error();
            }
            ++report.deleted;
            rememberTitle(report.deleted_titles, local.value().task.title);
            break;
        }
        case SyncAction::kResurrect: {
            // Two writes, in this order: the row comes back and the tombstone
            // goes away. Leaving the tombstone behind would make the next
            // export claim the task is deleted on a machine that is holding it.
            const Status inserted = insertSyncedTask(db, record);
            if (!inserted.ok()) {
                return inserted.error();
            }
            Statement forget;
            const Status prepared = forget.prepare(db, "DELETE FROM tombstones WHERE uid = ?");
            if (!prepared.ok()) {
                return prepared.error();
            }
            const Status bound = bindText(forget, 1, record.uid);
            if (!bound.ok()) {
                return bound.error();
            }
            const Status dropped = stepDone(forget, "drop a tombstone");
            if (!dropped.ok()) {
                return dropped.error();
            }
            ++report.resurrected;
            rememberTitle(report.resurrected_titles, record.task.title);
            break;
        }
        case SyncAction::kSkip:
            ++report.skipped;
            break;
        }
    }
    return Unit{};
}

// ---------------------------------------------------------------------------
// Migration
// ---------------------------------------------------------------------------

/// Does `table` already have a column named `column`?
///
/// PRAGMA table_info yields one row per column, with the name in field 1. The
/// table name is a compile-time constant from this file, never caller data, so
/// interpolating it is not the injection risk binding exists to prevent.
///
/// This is the authority on schema shape, not the schema_version setting: a
/// database written by the build that predates `uid` already claims version 1
/// (see kSchemaVersion), so the settings row can say "migrated" while the table
/// has never seen the column. Asking the table cannot be wrong.
[[nodiscard]] Result<bool> tableHasColumn(sqlite3 *db, const char *table, const char *column)
{
    Statement stmt;
    const Status prepared = stmt.prepare(db, std::string("PRAGMA table_info(") + table + ")");
    if (!prepared.ok()) {
        return prepared.error();
    }

    int result = sqlite3_step(stmt.handle());
    while (SQLITE_ROW == result) {
        if (column == textColumn(stmt.handle(), 1)) {
            return true;
        }
        result = sqlite3_step(stmt.handle());
    }
    if (SQLITE_DONE != result) {
        return Error::storageFailure(std::string("failed to inspect the columns of ") + table + ": " +
                                     sqlite3_errmsg(db));
    }
    return false;
}

/// Bring a database up to schema version 1: `tasks.uid`, its unique index, and
/// the tombstones table (the last of which the schema statements already
/// created, since it depends on nothing).
///
/// The order below is load-bearing, and it is the part that silently breaks on
/// a fresh checkout of an old database:
///
/// 1. Add the column, if it is absent. A plain `ADD COLUMN uid TEXT` — never
///    `ADD COLUMN uid TEXT UNIQUE`, which SQLite rejects outright ("Cannot add
///    a UNIQUE column"), and which would be the wrong tool anyway: every
///    existing row starts out NULL.
/// 2. Backfill every row that still has no usable uid (NULL, or the empty
///    string a hand-written INSERT would leave). One fresh v4 uuid per row —
///    shared or sequential values would re-create exactly the collision the
///    uid exists to prevent, since the point is that two machines can generate
///    identities independently without coordination.
/// 3. Only THEN create the unique index. Creating it first is what looks
///    harmless and is not: a database whose uid cells hold the SAME text — a
///    partially hand-migrated one where several rows say '', which is a value
///    rather than a NULL and therefore *does* collide — would fail the index
///    creation. The backfill exists to repair exactly that shape, so running it
///    first turns a permanent "cannot open this database" into an ordinary
///    repair, and the index then gets built over final, distinct values.
/// 4. The caller (open) records the version once all of this has succeeded, so
///    the setting describes what happened rather than what was attempted.
///
/// Idempotent by construction: an already-migrated database has the column, no
/// rows to backfill, and an index that IF NOT EXISTS skips. Reopening it
/// changes nothing, which the migration test asserts by opening three times.
///
/// The steps are also RESUMABLE, which is why they are not wrapped in a
/// transaction: a failure part-way (a full disk during the backfill, no
/// entropy) leaves a database whose column exists and whose remaining rows are
/// still NULL, and the next open finds the column present, backfills exactly
/// those rows, and creates the index then. Re-running the work that already
/// succeeded is cheap; re-doing it a row at a time from a rolled-back state
/// would not be.
[[nodiscard]] Status migrateUidColumn(sqlite3 *db)
{
    const Result<bool> has_uid = tableHasColumn(db, "tasks", "uid");
    if (!has_uid.ok()) {
        return has_uid.error();
    }

    if (!has_uid.value()) {
        const Status added = execWith(db, "ALTER TABLE tasks ADD COLUMN uid TEXT;");
        if (!added.ok()) {
            return added.error();
        }
    }

    // Collect the ids first, then update: running an UPDATE while a SELECT over
    // the same table is mid-iteration would ask SQLite to rewrite the rows the
    // cursor is walking. The set is small (it shrinks to empty after the first
    // migration), and the update only ever touches rows the SELECT already
    // returned, so there is no risk of missing a row this way.
    std::vector<std::int64_t> unassigned;
    {
        Statement select;
        const Status prepared =
            select.prepare(db, "SELECT id FROM tasks WHERE uid IS NULL OR uid = ''");
        if (!prepared.ok()) {
            return prepared.error();
        }
        int result = sqlite3_step(select.handle());
        while (SQLITE_ROW == result) {
            unassigned.push_back(sqlite3_column_int64(select.handle(), 0));
            result = sqlite3_step(select.handle());
        }
        if (SQLITE_DONE != result) {
            return Error::storageFailure(std::string("failed to inspect tasks without a uid: ") +
                                         sqlite3_errmsg(db));
        }
    }

    if (!unassigned.empty()) {
        Statement update;
        const Status prepared = update.prepare(db, "UPDATE tasks SET uid = ? WHERE id = ?");
        if (!prepared.ok()) {
            return prepared.error();
        }
        for (const std::int64_t id : unassigned) {
            // A failure here fails open() loudly: a row that cannot be given an
            // identity cannot be exported or merged safely, and Uuid.hpp argues
            // against any fallback that might silently repeat a value.
            const Result<std::string> generated = generateUuidV4();
            if (!generated.ok()) {
                return generated.error();
            }
            const Status bound_uid = bindText(update, 1, generated.value());
            if (!bound_uid.ok()) {
                return bound_uid.error();
            }
            const Status bound_id = bindInt64(update, 2, id);
            if (!bound_id.ok()) {
                return bound_id.error();
            }
            const Status written = stepDone(update, "backfill a uid");
            if (!written.ok()) {
                return written.error();
            }
            // The statement is reused for every row, so it has to be reset
            // before the next bind; a statement left at SQLITE_DONE would
            // silently skip the rest of the rows.
            if (SQLITE_OK != sqlite3_reset(update.handle())) {
                return Error::storageFailure(std::string("failed to reset the uid backfill: ") +
                                             sqlite3_errmsg(db));
            }
        }
    }

    // The unique index, now that every row has a distinct value. SQLite gives
    // the index no special treatment for NULL — several NULL uids are all
    // "distinct" to it — so the store's own write paths remain the guarantee
    // that a uid is always present; the index is what makes a DUPLICATE uid
    // impossible at the database level rather than by convention.
    return execWith(db, "CREATE UNIQUE INDEX IF NOT EXISTS idx_tasks_uid ON tasks(uid);");
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Result<std::unique_ptr<TaskStore>> TaskStore::open(const std::string &path)
{
    sqlite3 *raw = nullptr;
    const int opened = sqlite3_open_v2(path.c_str(), &raw,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (SQLITE_OK != opened) {
        // sqlite3_open_v2 hands back a handle even when it fails, so the
        // failure path has to close it or the error leaks a file descriptor.
        // It can also fail to produce one at all (out of memory), hence the
        // null check before asking for the message.
        const std::string detail = (nullptr != raw) ? sqlite3_errmsg(raw) : "out of memory";
        sqlite3_close(raw);
        return Error::storageFailure("cannot open database " + path + ": " + detail);
    }

    // Take ownership immediately: from here on every failure path below closes
    // the handle through the destructor instead of repeating the cleanup.
    std::unique_ptr<TaskStore> store{ new TaskStore(raw, path) };

    // The RPC server has one thread per connection, so concurrent writes are
    // normal. Without a timeout the loser of a write race returns SQLITE_BUSY
    // instantly and the client sees a spurious failure; with it, sqlite
    // retries for five seconds, which is far longer than any statement here
    // takes.
    const Status timeout = store->exec("PRAGMA busy_timeout = 5000;");
    if (!timeout.ok()) {
        return Error::storageFailure("cannot configure database " + path + ": " + timeout.error().message);
    }

    for (const char *statement : kSchemaStatements) {
        const Status applied = store->exec(statement);
        if (!applied.ok()) {
            return Error::storageFailure("cannot apply the schema to " + path + ": " +
                                         applied.error().message);
        }
    }

    // Then the in-place upgrade to version 1. It runs on EVERY open, not only
    // when the settings row says an older version: a fresh database's tasks
    // table already has the column so there is nothing to add, but the unique
    // index and the self-repair of a hand-mangled uid cell are wanted there
    // too, and both statements are idempotent. See migrateUidColumn for why it
    // must not run before the schema statements (the tombstones table it relies
    // on is created by them) or be conditional on the stored version (an old
    // database already claims version 1).
    const Status migrated = migrateUidColumn(raw);
    if (!migrated.ok()) {
        return Error::storageFailure("cannot migrate " + path + " to schema version " +
                                     kSchemaVersion + ": " + migrated.error().message);
    }

    // Seeding is checked-then-written rather than INSERT OR REPLACE: an
    // existing row is the operator's tuning, and overwriting it here would
    // silently reset the weights on every daemon start.
    const Result<bool> has_weights = settingExists(raw, kWeightsKey);
    if (!has_weights.ok()) {
        return Error::storageFailure("cannot inspect the settings table in " + path + ": " +
                                     has_weights.error().message);
    }
    if (!has_weights.value()) {
        const Status seeded = writeSetting(raw, kWeightsKey, toJson(Weights{}).dump());
        if (!seeded.ok()) {
            return Error::storageFailure("cannot seed default weights in " + path + ": " +
                                         seeded.error().message);
        }
    }

    // Written AFTER the migration, so the setting records what the database
    // actually is rather than what this build hoped for. It is an upsert, not
    // an insert-if-absent: the previous build wrote this same key without ever
    // reading it, so a stale value is not history worth preserving — the shape
    // of the table is the history, and this row now describes it.
    const Status versioned = writeSetting(raw, kSchemaVersionKey, kSchemaVersion);
    if (!versioned.ok()) {
        return Error::storageFailure("cannot record the schema version in " + path + ": " +
                                     versioned.error().message);
    }

    return store;
}

TaskStore::TaskStore(sqlite3 *db, std::string path) : m_db{ db }, m_path{ std::move(path) } {}

TaskStore::~TaskStore()
{
    // Every statement this class creates is finalized by its Statement
    // destructor before we get here, so the close has no busy handles to fail
    // on. If a future path leaks one, sqlite3_close returns SQLITE_BUSY and
    // leaves the handle alone — a leak visible in a debugger rather than a
    // crash in production.
    if (nullptr != m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

Status TaskStore::exec(const char *sql) const
{
    // Lock-free by construction: the only callers are on the open() path,
    // before the store escapes to any other thread, and sqlite3_exec runs the
    // whole statement itself. The work lives in execWith so the open()-path
    // helpers can run against the raw handle before this object exists.
    return execWith(m_db, sql);
}

// ---------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------

Result<Task> TaskStore::addTask(Task draft)
{
    // Validate before locking: a rejected write must not perturb the database
    // in any way, including taking the lock for a call that cannot succeed.
    const Status valid = validateTaskFields(draft.title, draft.importance, draft.blocks, draft.tags);
    if (!valid.ok()) {
        return valid.error();
    }

    // The uid is assigned here rather than accepted from the caller, for the
    // same reason id and the timestamps are: a client that can choose its own
    // uid can choose one that collides with a task arriving from another
    // machine, and the collision destroys one of the two at the next merge —
    // silently, and on the other machine. Generated before the lock so the
    // entropy read stays outside the critical section.
    const Result<std::string> uid = generateUuidV4();
    if (!uid.ok()) {
        // Propagated, not substituted: Uuid.hpp explains why a fallback that
        // silently repeats a value is worse than a failed insert.
        return uid.error();
    }

    const std::lock_guard<std::mutex> lock(m_mutex);

    // Sampled once so created_at and updated_at agree exactly on a new row —
    // a task that is already "modified" the instant it is created would be a
    // lie a client can observe.
    const std::int64_t now = nowEpochSeconds();

    // Whatever the caller put in id/uid/created_at/updated_at/completed_at is
    // discarded: those five columns are identity and history, and a client that
    // can write them can forge both. completed_at is stamped here rather than
    // left NULL so that creating a task as done produces the same row shape as
    // completing one later.
    const bool created_done = TaskStatus::kDone == draft.status;

    Statement stmt;
    const Status prepared =
        stmt.prepare(m_db, "INSERT INTO tasks(uid, title, notes, status, importance, due_at, "
                           "blocks, tags, created_at, updated_at, completed_at) "
                           "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!prepared.ok()) {
        return prepared.error();
    }

    std::vector<BoundValue> values;
    values.push_back(textValue(uid.value()));
    values.push_back(textValue(draft.title));
    values.push_back(textValue(draft.notes));
    values.push_back(textValue(toString(draft.status)));
    values.push_back(intValue(draft.importance));
    values.push_back(draft.due_at.has_value() ? int64Value(*draft.due_at) : nullValue());
    values.push_back(intValue(draft.blocks));
    values.push_back(textValue(serializeTags(draft.tags)));
    values.push_back(int64Value(now));
    values.push_back(int64Value(now));
    values.push_back(created_done ? int64Value(now) : nullValue());

    const Status bound = bindValues(stmt, values);
    if (!bound.ok()) {
        return bound.error();
    }
    const Status inserted = stepDone(stmt, "insert task");
    if (!inserted.ok()) {
        return inserted.error();
    }

    // Read the row back rather than reusing the draft: what the caller gets is
    // then by construction what the database holds, including any column
    // default that only SQLite applied.
    return selectTaskById(m_db, sqlite3_last_insert_rowid(m_db));
}

Result<Task> TaskStore::getTask(std::int64_t id) const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return selectTaskById(m_db, id);
}

Result<std::vector<Task>> TaskStore::listTasks(const TaskFilter &filter) const
{
    const std::lock_guard<std::mutex> lock(m_mutex);

    std::string sql = std::string("SELECT ") + kTaskColumns + " FROM tasks";
    std::vector<BoundValue> values;

    // 1. Status filter. The name comes from toString() rather than being
    //    spelled here, so renaming a status in Task.cpp cannot leave this
    //    query silently matching nothing.
    if (filter.status.has_value()) {
        sql += " WHERE status = ?";
        values.push_back(textValue(toString(*filter.status)));
    }

    // 2. Tag filter. Tags live as a JSON array in a TEXT column, so the
    //    question is whether the tag is an ELEMENT of that array, and json_each
    //    answers it exactly: an EXISTS over the array's elements comparing each
    //    one to the bound parameter is true only for a row that actually
    //    carries the tag.
    //
    //    A substring search over the JSON text can never be exact, so the
    //    previous instr(tags, '"<tag>"') test is gone. It matched after the
    //    fact as well as before one: a filter tag of ',' matched the separator
    //    between any two array elements (returning every task with two or more
    //    tags), and '' matched the closing quote of an element whose last
    //    character is escaped. It also under-matched, which its own comment
    //    admitted: a tag containing a double quote or a backslash is stored
    //    escaped (\" and \\) and so was never found. Both directions are wrong
    //    — a filter must not return rows that do not carry the tag, nor miss
    //    rows that do — and an element comparison is exactly why the LIKE-shaped
    //    approximation and its escaping caveat are no longer needed.
    //
    //    The CASE keeps a corrupt cell from being worse than useless. json_each
    //    raises "malformed JSON" on text that is not JSON at all, which would
    //    fail the entire query and hide every healthy row behind one bad cell;
    //    and it yields a single row for a bare scalar, so a tags cell holding
    //    `"work"` would match as if it were the one-element array ["work"] — a
    //    shape parseTags reports as untagged. Feeding it '[]' in both cases
    //    makes the filter agree with what readTaskRow shows the caller.
    if (filter.tag.has_value()) {
        sql += values.empty() ? " WHERE " : " AND ";
        sql += "EXISTS (SELECT 1 FROM json_each(CASE WHEN json_valid(tags)"
               " THEN CASE WHEN 'array' = json_type(tags) THEN tags ELSE '[]' END"
               " ELSE '[]' END) AS tag WHERE tag.value = ?)";
        values.push_back(textValue(*filter.tag));
    }

    // 3. Limit. 0 means "no limit" per the header, and is expressed by leaving
    //    the clause out entirely rather than by binding a sentinel: SQLite
    //    treats LIMIT -1 as unlimited but that is a detail of this engine, not
    //    of this interface.
    if (0 != filter.limit) {
        sql += " LIMIT ?";
        values.push_back(int64Value(static_cast<std::int64_t>(filter.limit)));
    }

    // No ORDER BY: the result is an unordered set by contract, and pretending
    // an order here would invite a caller to depend on one that this layer has
    // no basis for. PriorityEngine sorts what comes back.

    Statement stmt;
    const Status prepared = stmt.prepare(m_db, sql);
    if (!prepared.ok()) {
        return prepared.error();
    }
    const Status bound = bindValues(stmt, values);
    if (!bound.ok()) {
        return bound.error();
    }

    std::vector<Task> tasks;
    int result = sqlite3_step(stmt.handle());
    while (SQLITE_ROW == result) {
        tasks.push_back(readTaskRow(stmt.handle()));
        result = sqlite3_step(stmt.handle());
    }
    if (SQLITE_DONE != result) {
        return Error::storageFailure(std::string("failed to list tasks: ") + sqlite3_errmsg(m_db));
    }
    return tasks;
}

Result<Task> TaskStore::updateTask(std::int64_t id, const TaskPatch &patch)
{
    // due_at has three states and no sensible precedence rule for "both", so
    // the contradictory combination is rejected rather than guessed at: a
    // caller that set both meant one of them and only it knows which.
    if (patch.due_at.has_value() && patch.clear_due_at) {
        return Error::invalidArgument("due_at and clear_due_at cannot both be set (due_at=" +
                                      std::to_string(*patch.due_at) + ")");
    }

    const std::lock_guard<std::mutex> lock(m_mutex);

    // Read-modify-write under one lock. The merged result is what gets
    // validated, so a patch cannot slip an invalid value past the checks by
    // touching a different field than the one it breaks.
    const Result<Task> current = selectTaskById(m_db, id);
    if (!current.ok()) {
        return current.error();
    }
    const Task &before = current.value();

    Task after = before;
    if (patch.title.has_value()) {
        after.title = *patch.title;
    }
    if (patch.notes.has_value()) {
        after.notes = *patch.notes;
    }
    if (patch.status.has_value()) {
        after.status = *patch.status;
    }
    if (patch.importance.has_value()) {
        after.importance = *patch.importance;
    }
    if (patch.due_at.has_value()) {
        after.due_at = *patch.due_at;
    } else if (patch.clear_due_at) {
        after.due_at = std::nullopt;
    }
    if (patch.blocks.has_value()) {
        after.blocks = *patch.blocks;
    }
    if (patch.tags.has_value()) {
        after.tags = *patch.tags;
    }

    // completed_at follows the status transition:
    // 1. Entering kDone stamps it, but only when the task carries no stamp —
    //    re-completing a reopened task must not rewrite its history.
    // 2. Leaving kDone clears it, so anything that reads the column directly
    //    cannot report a reopened task as finished.
    const std::int64_t now = nowEpochSeconds();
    if (TaskStatus::kDone == after.status) {
        if (!after.completed_at.has_value()) {
            after.completed_at = now;
        }
    } else {
        after.completed_at = std::nullopt;
    }

    const Status valid = validateTaskFields(after.title, after.importance, after.blocks, after.tags);
    if (!valid.ok()) {
        return valid.error();
    }

    // Build the SET clause from the fields the patch carries. Only constant
    // fragments are concatenated; the values are bound below, which is what
    // keeps a title containing a quote harmless here too. The list is never
    // empty because updated_at is unconditional.
    std::vector<std::string> assignments;
    std::vector<BoundValue> values;
    if (patch.title.has_value()) {
        assignments.emplace_back("title = ?");
        values.push_back(textValue(after.title));
    }
    if (patch.notes.has_value()) {
        assignments.emplace_back("notes = ?");
        values.push_back(textValue(after.notes));
    }
    if (patch.status.has_value()) {
        assignments.emplace_back("status = ?");
        values.push_back(textValue(toString(after.status)));
    }
    if (patch.importance.has_value()) {
        assignments.emplace_back("importance = ?");
        values.push_back(intValue(after.importance));
    }
    if (patch.due_at.has_value() || patch.clear_due_at) {
        assignments.emplace_back("due_at = ?");
        values.push_back(after.due_at.has_value() ? int64Value(*after.due_at) : nullValue());
    }
    if (patch.blocks.has_value()) {
        assignments.emplace_back("blocks = ?");
        values.push_back(intValue(after.blocks));
    }
    if (patch.tags.has_value()) {
        assignments.emplace_back("tags = ?");
        values.push_back(textValue(serializeTags(after.tags)));
    }
    if (after.completed_at != before.completed_at) {
        assignments.emplace_back("completed_at = ?");
        values.push_back(after.completed_at.has_value() ? int64Value(*after.completed_at) : nullValue());
    }
    // updated_at is refreshed even by a patch that changes nothing: the column
    // means "last touched", and a no-op write is still a touch the client
    // asked for.
    assignments.emplace_back("updated_at = ?");
    values.push_back(int64Value(now));

    std::string sql = "UPDATE tasks SET ";
    for (std::size_t index = 0; index < assignments.size(); ++index) {
        if (0 != index) {
            sql += ", ";
        }
        sql += assignments[index];
    }
    sql += " WHERE id = ?";
    values.push_back(int64Value(id));

    Statement stmt;
    const Status prepared = stmt.prepare(m_db, sql);
    if (!prepared.ok()) {
        return prepared.error();
    }
    const Status bound = bindValues(stmt, values);
    if (!bound.ok()) {
        return bound.error();
    }
    const Status updated = stepDone(stmt, "update task");
    if (!updated.ok()) {
        return updated.error();
    }

    return selectTaskById(m_db, id);
}

Status TaskStore::deleteTask(std::int64_t id)
{
    const std::lock_guard<std::mutex> lock(m_mutex);

    // Read the row first, because the tombstone has to name its uid and after
    // the DELETE there is no row left to ask. The same lock covers both
    // statements, so nothing can change the row in between.
    //
    // kNotFound keeps the meaning it had before uids existed: deleting an
    // already-deleted task reports kNotFound even though a tombstone for its
    // uid is sitting in the table, because from the caller's side "it is gone"
    // is the truth and the tombstone is bookkeeping, not a task. A silent
    // success would tell the second caller it removed something it did not.
    const Result<Task> doomed = selectTaskById(m_db, id);
    if (!doomed.ok()) {
        return doomed.error();
    }

    // The row removal and its tombstone are one fact, so they are one
    // transaction. Half-applying them — row gone, no tombstone — would let the
    // other machine's next export insert the task straight back, which is the
    // resurrection tombstones exist to prevent.
    Transaction transaction{ m_db };
    const Status began = transaction.begin();
    if (!began.ok()) {
        return began.error();
    }

    // nowEpochSeconds() is sampled after the row was read, so on this machine's
    // clock the tombstone is at least as new as anything the row carried.
    const Status removed = removeTaskByUid(m_db, doomed.value().uid, nowEpochSeconds());
    if (!removed.ok()) {
        // Any early return here rolls the transaction back in ~Transaction, so
        // the row and the tombstone are both still there (or both still gone).
        return removed.error();
    }

    return transaction.commit();
}

// ---------------------------------------------------------------------------
// Synchronisation
// ---------------------------------------------------------------------------
//
// The format and the merge rule live in TaskSync.hpp; these methods are the
// database side of them. The split matters: the part that can lose data is a
// pure function over (local state, incoming record) that is tested without
// SQLite, and what is left here is mechanical — read the state, ask decide(),
// carry out the answer.

Result<std::string> TaskStore::exportJsonl() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);

    // Collected as (uid, line) pairs and sorted afterwards rather than by the
    // SQL: the sort key is the uid of the RECORD, and the two record shapes
    // come from two tables, so no single ORDER BY covers both.
    std::vector<std::pair<std::string, std::string>> lines;

    // 1. Live tasks. The full row is read (not just the export fields) so a
    //    row that cannot be exported can be named by its id.
    {
        Statement stmt;
        const Status prepared =
            stmt.prepare(m_db, std::string("SELECT ") + kTaskColumns + " FROM tasks");
        if (!prepared.ok()) {
            return prepared.error();
        }
        int result = sqlite3_step(stmt.handle());
        while (SQLITE_ROW == result) {
            const Task task = readTaskRow(stmt.handle());
            // A row with no uid cannot be exported: its line would carry an
            // empty uid, which no reader accepts, and dropping the line
            // silently would produce a file that claims to be this machine's
            // complete snapshot while leaving a task out of it. Failing names
            // the row instead. Unreachable through this class — the migration
            // backfills every row and every write path supplies a uid — so
            // reaching it means the database was edited by hand.
            if (task.uid.empty()) {
                return Error::storageFailure("task " + std::to_string(task.id) +
                                             " has no uid, so it cannot be exported; the "
                                             "database needs migrating again");
            }
            lines.emplace_back(task.uid, toJsonLine(task));
            result = sqlite3_step(stmt.handle());
        }
        if (SQLITE_DONE != result) {
            return Error::storageFailure(std::string("failed to read tasks for export: ") +
                                         sqlite3_errmsg(m_db));
        }
    }

    // 2. Tombstones. Same one-line-per-uid shape, so a reader does not have to
    //    know which table a record came from.
    {
        Statement stmt;
        const Status prepared =
            stmt.prepare(m_db, "SELECT uid, deleted_at FROM tombstones");
        if (!prepared.ok()) {
            return prepared.error();
        }
        int result = sqlite3_step(stmt.handle());
        while (SQLITE_ROW == result) {
            const std::string uid = textColumn(stmt.handle(), 0);
            if (uid.empty()) {
                // Same reasoning as above; deleted_at is NOT NULL so the stamp
                // needs no check.
                return Error::storageFailure(
                    "a tombstone has no uid, so it cannot be exported; the database was "
                    "edited by hand");
            }
            lines.emplace_back(uid, toJsonLine(uid, sqlite3_column_int64(stmt.handle(), 1)));
            result = sqlite3_step(stmt.handle());
        }
        if (SQLITE_DONE != result) {
            return Error::storageFailure(std::string("failed to read tombstones for export: ") +
                                         sqlite3_errmsg(m_db));
        }
    }

    // Sorted by uid, which is the property that makes git's line merge
    // deterministic: a change to one task moves exactly one line, so two
    // machines' exports merge line by line instead of conflicting wholesale
    // (TaskSync.hpp, property 4). Sorting here rather than in SQL also puts
    // the two record kinds in one order, which no SQL query could.
    std::sort(lines.begin(), lines.end(),
              [](const std::pair<std::string, std::string> &lhs,
                 const std::pair<std::string, std::string> &rhs) { return lhs.first < rhs.first; });

    // One record per line, each line newline-terminated — including the last,
    // so appending is a plain concatenation and a file that lost its final byte
    // is visibly truncated rather than silently valid.
    std::string out;
    std::size_t total = 0;
    for (const auto &entry : lines) {
        total += entry.second.size() + 1;
    }
    out.reserve(total);
    for (const auto &entry : lines) {
        out += entry.second;
        out += '\n';
    }
    return out;
}

Result<MergeReport> TaskStore::mergeJsonl(const std::string &jsonl, bool dry_run)
{
    // Parse first, before the lock and before any transaction: a malformed file
    // must not even open one. parseJsonl fails the whole call and names the
    // offending line — the only handle a caller has on a corrupt export
    // (TaskStore.hpp), because "some records could not be read" and "the file
    // was short" are indistinguishable once the line numbers are gone.
    const Result<std::vector<SyncRecord>> parsed = parseJsonl(jsonl);
    if (!parsed.ok()) {
        return parsed.error();
    }
    const std::vector<SyncRecord> &records = parsed.value();

    // Validate every record BEFORE the transaction opens, in a pass of its own.
    // Three reasons, in order of weight:
    //
    // 1. The header promises no partial merge, and a rejected record aborts the
    //    whole call. Doing the check before the first write makes half-applying
    //    impossible by construction rather than by remembering to roll back.
    // 2. The check must not depend on this machine's state. If a record were
    //    validated only when the merge would have applied it, the same file
    //    could be accepted here and rejected on the other machine — a sync
    //    failure that appears only on one side, which is the hardest kind to
    //    explain. So every live record is validated, including ones this store
    //    would have skipped.
    // 3. A corrupt export then fails loudly, once, naming the record. The
    //    alternative — skipping the bad record — would leave the operator with
    //    a merge that reported success and a backlog that quietly diverged.
    for (std::size_t index = 0; index < records.size(); ++index) {
        const SyncRecord &record = records[index];
        if (record.deleted) {
            // A tombstone carries no task fields, so there is nothing to
            // validate beyond the uid the parser has already shape-checked.
            continue;
        }

        // The uid is re-checked here even though parseJsonl validates it: this
        // is the boundary where foreign text becomes rows, and a malformed uid
        // that got through would be stored and then re-exported as a line no
        // other machine could merge.
        if (!looksLikeUuidV4(record.uid)) {
            return Error::invalidArgument("record " + std::to_string(index + 1) +
                                          " of the export has a malformed uid (\"" + record.uid +
                                          "\")");
        }

        // The same rules addTask applies, through the same function, so an
        // import cannot smuggle in a task a local write would have rejected.
        const Status valid = validateTaskFields(record.task.title, record.task.importance,
                                                record.task.blocks, record.task.tags);
        if (!valid.ok()) {
            return Error::invalidArgument("record " + std::to_string(index + 1) +
                                          " of the export is not a valid task: " +
                                          valid.error().message);
        }
    }

    const std::lock_guard<std::mutex> lock(m_mutex);

    // The whole merge is ONE transaction (TaskStore.hpp). Stopping halfway
    // would leave the backlog in a state that is neither the old one nor the
    // new one, and the next sync would treat that mixture as truth.
    Transaction transaction{ m_db };
    const Status began = transaction.begin();
    if (!began.ok()) {
        return began.error();
    }

    MergeReport report;
    const Status applied = applyMergeRecords(m_db, records, report);
    if (!applied.ok()) {
        // The destructor rolls back, so the store is exactly as it was; the
        // half-built report is discarded with the error, because a merge that
        // failed is not a merge that inserted a few records.
        return applied.error();
    }

    if (dry_run) {
        // Every decision has been taken and every count is final; not calling
        // commit() is what keeps the database exactly as it was (the destructor
        // rolls back). The report is byte-for-byte what the same call without
        // dry_run would produce, because it IS the same code — the flag only
        // chooses how the transaction ends. That equality is the only thing
        // that makes a dry run worth trusting, and test_task_store.cpp asserts
        // it.
        return report;
    }

    const Status committed = transaction.commit();
    if (!committed.ok()) {
        return committed.error();
    }
    return report;
}

Result<Task> TaskStore::getTaskByUid(const std::string &uid) const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return selectTaskByUid(m_db, uid);
}

Result<std::size_t> TaskStore::tombstoneCount() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);

    Statement stmt;
    const Status prepared = stmt.prepare(m_db, "SELECT COUNT(*) FROM tombstones");
    if (!prepared.ok()) {
        return prepared.error();
    }
    if (SQLITE_ROW != sqlite3_step(stmt.handle())) {
        return Error::storageFailure(std::string("failed to count tombstones: ") +
                                     sqlite3_errmsg(m_db));
    }

    // COUNT(*) is always one row holding a non-negative integer.
    return static_cast<std::size_t>(sqlite3_column_int64(stmt.handle(), 0));
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

Result<Weights> TaskStore::weights() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);

    const Result<std::string> stored = readSetting(m_db, kWeightsKey);
    if (!stored.ok()) {
        // open() seeds this row, so its absence is corruption rather than a
        // normal empty state: fail loudly instead of ranking with defaults the
        // operator never chose and cannot see.
        return Error::storageFailure("ranking weights are unavailable: " + stored.error().message);
    }

    // nlohmann reports malformed JSON by throwing. That is an unexpected
    // database state, not an expected failure, so it is converted at this
    // boundary into the same storage error a missing row produces — nothing
    // above this layer should have to know nlohmann's exception hierarchy.
    try {
        return weightsFromJson(nlohmann::json::parse(stored.value()));
    } catch (const nlohmann::json::exception &parse_error) {
        return Error::storageFailure(std::string("ranking weights are corrupt: ") + parse_error.what());
    }
}

Status TaskStore::setWeights(const Weights &weights)
{
    // Validate before locking, and before writing: a rejected set must leave
    // the stored value exactly as it was. A weights change that is silently
    // dropped is indistinguishable from one that worked, and the operator
    // would spend the next hour wondering why the queue never moved.
    const Status valid = weights.validate();
    if (!valid.ok()) {
        return valid.error();
    }

    const std::lock_guard<std::mutex> lock(m_mutex);
    return writeSetting(m_db, kWeightsKey, toJson(weights).dump());
}

// --- partial weights update ------------------------------------------------
//
// Kept as its own region because the whole point of the method below is that
// the read, the merge, the validation and the write are ONE step: splitting
// them across several m_mutex acquisitions is the lost-update bug this method
// exists to remove.

Result<Weights> TaskStore::patchWeights(const nlohmann::json &patch)
{
    // A patch has to be an object for the same reason a whole weights document
    // does: an array or a bare number carries no field names, so it cannot
    // express "change these keys". Accepting one would mean reporting success
    // for a call that changed nothing.
    if (!patch.is_object()) {
        return Error::invalidArgument(std::string("weights patch must be a JSON object (got ") +
                                      patch.type_name() + ")");
    }

    // Read-merge-write under ONE acquisition of m_mutex, the shape updateTask
    // uses. There is no window between the read and the write for another
    // thread to install its own snapshot, so two concurrent partial updates
    // compose instead of overwriting one another.
    const std::lock_guard<std::mutex> lock(m_mutex);

    const Result<std::string> stored = readSetting(m_db, kWeightsKey);
    if (!stored.ok()) {
        // open() seeds this row, so its absence is corruption rather than a
        // normal empty state — the same judgement weights() makes.
        return Error::storageFailure("ranking weights are unavailable: " + stored.error().message);
    }

    nlohmann::json merged;
    try {
        merged = nlohmann::json::parse(stored.value());
    } catch (const nlohmann::json::exception &parse_error) {
        // Unexpected database state, not expected caller input, so it becomes
        // the same storage error a missing row produces rather than an
        // nlohmann exception escaping this layer.
        return Error::storageFailure(std::string("ranking weights are corrupt: ") + parse_error.what());
    }
    if (!merged.is_object()) {
        return Error::storageFailure(std::string("ranking weights are corrupt: the stored value "
                                                 "is a ") +
                                     merged.type_name() + ", not an object");
    }

    // Supplied keys overwrite stored ones; omitted keys keep what is there.
    // Keys the weight parser does not know are ignored by it and never reach
    // the settings table — the write below serializes the parsed struct, not
    // this merged document — so a caller cannot smuggle extra fields in.
    for (auto entry = patch.begin(); entry != patch.end(); ++entry) {
        merged[entry.key()] = entry.value();
    }

    // Parsing is what validates, and it runs BEFORE the write: an invalid
    // value returns here and leaves the stored weights untouched, so a
    // rejected change is never half-applied.
    const Result<Weights> parsed = weightsFromJson(merged);
    if (!parsed.ok()) {
        return parsed.error();
    }

    const Status written = writeSetting(m_db, kWeightsKey, toJson(parsed.value()).dump());
    if (!written.ok()) {
        return written.error();
    }
    return parsed.value();
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

Result<Stats> TaskStore::stats(std::int64_t now) const
{
    const std::lock_guard<std::mutex> lock(m_mutex);

    // One pass, one row: six tallies computed in SQL instead of pulling every
    // task over the boundary to count it here. The status names are bound from
    // toString() so the query cannot drift from the enum, and the day length
    // comes from kSecondsPerDay so "the next 24 hours" means the same thing
    // here as it does in the scoring code.
    //
    // Question marks are numbered because the actionable pair (open,
    // in_progress) appears in three separate CASE expressions and numbering is
    // what lets them share one bind.
    const std::string sql =
        "SELECT"
        " COALESCE(SUM(CASE WHEN status = ?1 THEN 1 ELSE 0 END), 0),"
        " COALESCE(SUM(CASE WHEN status = ?2 THEN 1 ELSE 0 END), 0),"
        " COALESCE(SUM(CASE WHEN status = ?3 THEN 1 ELSE 0 END), 0),"
        " COALESCE(SUM(CASE WHEN status = ?4 THEN 1 ELSE 0 END), 0),"
        " COALESCE(SUM(CASE WHEN (status = ?1 OR status = ?2)"
        "                        AND due_at IS NOT NULL AND due_at < ?5"
        "                   THEN 1 ELSE 0 END), 0),"
        " COALESCE(SUM(CASE WHEN (status = ?1 OR status = ?2)"
        "                        AND due_at IS NOT NULL AND due_at >= ?5 AND due_at < ?6"
        "                   THEN 1 ELSE 0 END), 0)"
        " FROM tasks";

    Statement stmt;
    const Status prepared = stmt.prepare(m_db, sql);
    if (!prepared.ok()) {
        return prepared.error();
    }

    const std::vector<BoundValue> values = {
        textValue(toString(TaskStatus::kOpen)),
        textValue(toString(TaskStatus::kInProgress)),
        textValue(toString(TaskStatus::kDone)),
        textValue(toString(TaskStatus::kArchived)),
        int64Value(now),
        int64Value(now + kSecondsPerDay),
    };
    const Status bound = bindValues(stmt, values);
    if (!bound.ok()) {
        return bound.error();
    }

    if (SQLITE_ROW != sqlite3_step(stmt.handle())) {
        return Error::storageFailure(std::string("failed to tally task stats: ") + sqlite3_errmsg(m_db));
    }

    // The six SUMs always produce one row, and sqlite3_column_int64 maps NULL
    // to 0 (the COALESCEs above make that explicit for readers). Counts are
    // non-negative by construction, so the casts to size_t are safe.
    Stats stats;
    stats.open = static_cast<std::size_t>(sqlite3_column_int64(stmt.handle(), 0));
    stats.in_progress = static_cast<std::size_t>(sqlite3_column_int64(stmt.handle(), 1));
    stats.done = static_cast<std::size_t>(sqlite3_column_int64(stmt.handle(), 2));
    stats.archived = static_cast<std::size_t>(sqlite3_column_int64(stmt.handle(), 3));
    stats.overdue = static_cast<std::size_t>(sqlite3_column_int64(stmt.handle(), 4));
    stats.due_within_24h = static_cast<std::size_t>(sqlite3_column_int64(stmt.handle(), 5));
    return stats;
}

nlohmann::json toJson(const Stats &stats)
{
    // Keys match the status wire names from toString(): the counts and the
    // tasks they count have to be spelled the same way on the wire.
    return nlohmann::json{
        { "open", stats.open },
        { "in_progress", stats.in_progress },
        { "done", stats.done },
        { "archived", stats.archived },
        { "overdue", stats.overdue },
        { "due_within_24h", stats.due_within_24h },
    };
}

nlohmann::json toJson(const MergeReport &report)
{
    // Keys mirror the fields MergeReport declares, in that order, because the
    // report is read by whoever decides whether to accept a sync — a wire shape
    // that needs a lookup table to read is not a report.
    return nlohmann::json{
        { "inserted", report.inserted },
        { "updated", report.updated },
        { "deleted", report.deleted },
        { "resurrected", report.resurrected },
        { "skipped", report.skipped },
        { "inserted_titles", report.inserted_titles },
        { "updated_titles", report.updated_titles },
        { "deleted_titles", report.deleted_titles },
        { "resurrected_titles", report.resurrected_titles },
        // Derived from the counts rather than stored, but included because
        // "did this change anything" is the first question asked of a report
        // and clean() is its definition (MergeReport, and a no-op sync is the
        // common case).
        { "clean", report.clean() },
    };
}

} // namespace taskpilot
