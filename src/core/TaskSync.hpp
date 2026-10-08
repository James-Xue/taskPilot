#pragma once
// TaskSync.hpp — the cross-machine export format and the merge rule
//
// Pure: no SQLite, no sockets, no clock. The part of synchronisation that can
// silently destroy data is the merge DECISION, so that decision lives here as
// a total function over (what the local store has, what arrived) and is
// exercised exhaustively without a database. TaskStore only carries the
// decision out.
//
// ---------------------------------------------------------------------------
// The export format: JSONL, one record per line
// ---------------------------------------------------------------------------
// A record is a task or its tombstone:
//
//   {"v":1,"uid":"9f3c…","title":"…","notes":"","status":"open",
//    "importance":3,"due_at":null,"blocks":0,"tags":[],"created_at":N,
//    "updated_at":N,"completed_at":null,"deleted":false}
//
//   {"v":1,"uid":"9f3c…","updated_at":N,"deleted":true}
//
// Four properties are load-bearing, and each exists because its absence breaks
// git's line-based merge in a way that loses work:
//
//   1. ONE RECORD PER LINE. Two machines each appending tasks produce lines at
//      different positions, and git merges them. A single JSON object holding
//      an array would always conflict, because both sides would have rewritten
//      the same region.
//   2. NO HEADER LINE. A header appears at line 1 of every export, so both
//      sides would rewrite line 1 and conflict on every single sync. The format
//      version therefore travels on EVERY line as `v`.
//   3. EXACTLY ONE LINE PER UID. A uid appears once, as either the live task or
//      its tombstone — never both, never twice. So the file is a complete
//      snapshot keyed by uid, and merging is a per-uid comparison rather than a
//      replay.
//   4. SORTED BY UID. Same uid on the same side of every export, so a change to
//      one task moves exactly one line and a merge resolves per line rather
//      than per hunk.
//
// ---------------------------------------------------------------------------
// Why tombstones
// ---------------------------------------------------------------------------
// Without them a delete cannot travel: the other machine exports the task it
// still has, the merge sees a task it does not know, inserts it, and the thing
// you deliberately deleted comes back. So a delete leaves a tombstone behind,
// and absence from one side is never evidence of deletion.

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Result.hpp"
#include "core/Task.hpp"

namespace taskpilot
{

/// Current export format version, carried on every line as `v`.
/// Bump when a change would make an older reader misread a newer file.
inline constexpr int kExportFormatVersion{ 1 };

/// What the merge should do with one record.
enum class SyncAction
{
    kInsert,    ///< The local store has never seen this uid.
    kUpdate,    ///< Both sides have it; the incoming record is newer.
    kDelete,    ///< Incoming tombstone is at least as new as the local task.
    kResurrect, ///< Local tombstone, but the incoming record is strictly newer —
                ///< the other machine edited the task after we deleted it, and
                ///< last-write-wins means the edit stands. The store must also
                ///< drop the tombstone, which is why this is not kInsert.
    kSkip,      ///< Nothing to do: our copy is newer, or it is already gone.
};

/// One parsed export line.
struct SyncRecord
{
    int format{ kExportFormatVersion };
    std::string uid;
    bool deleted{ false };

    /// The task, when `deleted` is false. Its `uid` member is set from the
    /// record, and its `id` is deliberately left at 0: the integer id is a
    /// local row number, never transported, and a record that carried one
    /// would invite a caller to trust it.
    Task task;

    /// The last-write-wins stamp. `task.updated_at` for a live record,
    /// `deleted_at` for a tombstone. Kept as its own field so `decide()` never
    /// has to know which of the two it is looking at.
    std::int64_t stamp{ 0 };
};

/// What the local store already holds for a uid. Absent means neither a task
/// nor a tombstone.
struct LocalState
{
    bool present{ false };    ///< A live task row exists.
    bool tombstoned{ false }; ///< A tombstone row exists.
    std::int64_t stamp{ 0 };  ///< updated_at when present, deleted_at when tombstoned.
};

/// The merge rule.
///
/// | local      | incoming            | action     |
/// |------------|---------------------|------------|
/// | absent     | live                | kInsert    |
/// | absent     | tombstone           | kSkip      — a delete for a task we never had |
/// | live       | live, newer         | kUpdate    |
/// | live       | live, not newer     | kSkip      — ours is newer or identical |
/// | live       | tombstone, >=       | kDelete    — a delete beats a same-second edit |
/// | tombstoned | live, strictly newer| kResurrect |
/// | tombstoned | live, not newer     | kSkip      — our delete is newer |
/// | tombstoned | tombstone           | kSkip      — already gone |
///
/// Two deliberate asymmetries, both chosen so the surprising outcome is the
/// rarer one:
///
///   - A delete WINS A TIE (`>=`, where an update needs a strict `>`).
///     Resurrecting something the user deliberately deleted is far more
///     startling than losing an edit made in the same second, and the second
///     is recoverable by hand while the first looks like the tool ignoring an
///     instruction.
///   - A POST-DELETE EDIT RESURRECTS. Delete is not permanent; it is just the
///     latest write. Making delete absolute would need a second, separately
///     synchronised "this uid is banned forever" channel, and its failure mode
///     (an edit silently vanishing) is worse than the one it prevents.
[[nodiscard]] SyncAction decide(const LocalState &local, const SyncRecord &incoming);

/// Serialize a live task as one JSONL line, without a trailing newline.
/// `task.uid` must be a well-formed v4 uuid; the caller owns that invariant.
[[nodiscard]] std::string toJsonLine(const Task &task);

/// Serialize a tombstone as one JSONL line, without a trailing newline.
[[nodiscard]] std::string toJsonLine(const std::string &uid, std::int64_t deleted_at);

/// Parse one JSONL line.
///
/// Strict, because a lenient parser is how a corrupt line becomes data loss:
/// an unparseable line, an unknown `v`, a malformed or missing uid, or a live
/// record missing its required fields is a kInvalidArgument naming the problem.
/// A blank line is also an error — the caller decides to skip blanks, so that
/// a file which is unexpectedly empty is not silently reported as a clean
/// merge of zero records.
[[nodiscard]] Result<SyncRecord> parseJsonLine(const std::string &line);

/// Split a whole export into lines and parse each, skipping blank lines.
/// On the first malformed line the whole parse fails, naming the line number —
/// a partial parse must never be handed to a merge, because "some records
/// could not be read" and "the file was genuinely short" are indistinguishable
/// once the line numbers are gone.
[[nodiscard]] Result<std::vector<SyncRecord>> parseJsonl(const std::string &jsonl);

} // namespace taskpilot
