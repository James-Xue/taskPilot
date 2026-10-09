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
    kRecordTombstone, ///< Incoming tombstone for a uid this store has never
                ///< held, as either a task or a tombstone.
                ///
                /// ADOPT IT — do not skip it. This is the single most important
                /// rule in the file, because skipping it destroys the record
                /// and the loss is invisible from this machine:
                ///
                ///   1. A machine that skips a tombstone records nothing.
                ///   2. exportJsonl writes only what the store holds, so that
                ///      machine's export OMITS the uid's line entirely.
                ///   3. A file with no line for a uid is indistinguishable from
                ///      a file where the task was never deleted — and git
                ///      propagates the omission as a deletion of the tombstone.
                ///   4. Any machine still holding the task live (it had not
                ///      synced since the delete) pulls that file, learns nothing
                ///      about the uid, keeps the task, and republishes it live.
                ///
                /// The deleted task comes back, on every machine, and the
                /// machine that caused it reported "skipped: 1, clean". The
                /// whole point of tombstones — that absence is never evidence
                /// of deletion — is defeated by a receiving machine declining to
                /// store the evidence it was handed.
    kReviseTombstone, ///< Both sides hold a tombstone for this uid and the
                ///< incoming one is strictly newer; adopt its stamp.
                ///
                /// Two machines can each delete the same task locally, at their
                /// own second. Without this branch neither stamp can ever be
                /// revised — a tombstone's stamp is only ever written by
                /// deleting a LIVE row, and a second delete is kNotFound — so
                /// the two machines keep different stamps forever: they never
                /// converge, their exports differ on that line permanently, and
                /// git conflicts on it at every sync.
    kSkip,      ///< Nothing to do: our copy is newer, or already equivalent.
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

    /// True when the stamp on the wire was outside the plausible window and was
    /// clamped to the nearest bound (see clampPlausibleStamp). The record is
    /// still accepted — refusing it took the whole sync down — but the repair is
    /// counted in MergeReport so a caller can see that a peer or a client is
    /// sending timestamps in the wrong unit, which is a bug worth fixing at its
    /// source rather than absorbing forever.
    bool stamp_adjusted{ false };
};

/// What the local store already holds for a uid. Absent means neither a task
/// nor a tombstone.
struct LocalState
{
    bool present{ false };    ///< A live task row exists.
    bool tombstoned{ false }; ///< A tombstone row exists.
    std::int64_t stamp{ 0 };  ///< updated_at when present, deleted_at when tombstoned.

    /// The live row itself, so the equal-stamp tie-break can compare CONTENT
    /// (see asymmetry 3 below): the row when `present`, default-constructed
    /// otherwise.
    ///
    /// It is here because decide() is a pure function of its two arguments and
    /// has no database to read, while the content comparison needs BOTH records
    /// — the incoming one and ours. The caller already holds the row (it read it
    /// to learn `stamp`), so filling this in costs a copy rather than a query.
    ///
    /// Both digests of one comparison are computed inside decide() from the two
    /// records it was given, deliberately: a caller-side digest would be a
    /// second implementation of the same rule, and two machines computing it
    /// differently is exactly the fork the tie-break exists to prevent.
    ///
    /// An empty `uid` marks "no row supplied" — a stored task always has one —
    /// and decide() then keeps the local copy on an equal stamp rather than
    /// guessing at the content.
    Task task;
};

/// The merge rule.
///
/// | local      | incoming                       | action           |
/// |------------|--------------------------------|------------------|
/// | absent     | live                           | kInsert          |
/// | absent     | tombstone                      | kRecordTombstone |
/// | live       | live, newer stamp              | kUpdate          |
/// | live       | live, equal stamp, same content| kSkip            |
/// | live       | live, equal stamp, DIFFERS     | kUpdate          |
/// | live       | live, older stamp              | kSkip            |
/// | live       | tombstone, stamp >=            | kDelete          |
/// | live       | tombstone, older stamp         | kSkip            |
/// | tombstoned | live, strictly newer           | kResurrect       |
/// | tombstoned | live, not newer                | kSkip            |
/// | tombstoned | tombstone, strictly newer      | kReviseTombstone |
/// | tombstoned | tombstone, not newer           | kSkip            |
///
/// THREE deliberate asymmetries. Each one picks the outcome that is less
/// surprising, and each is justified by what the alternative does to a real
/// sequence of machine actions rather than by taste:
///
///   1. A DELETE WINS A TIE — `>=` here where an update needs a strict `>`.
///      Resurrecting something the user deliberately deleted is far more
///      startling than losing an edit made in the same second, and the second
///      is recoverable by hand while the first looks like the tool ignoring an
///      instruction.
///
///   2. A POST-DELETE EDIT RESURRECTS. Delete is not permanent; it is just the
///      latest write. Making delete absolute would need a second, separately
///      synchronised "this uid is banned forever" channel, and its failure mode
///      (an edit silently vanishing) is worse than the one it prevents.
///
///   3. AN EQUAL-STAMP DISAGREEMENT IS BROKEN BY CONTENT, NOT BY SIDE.
///      "Equal stamp means agreement" is false for two different edits made in
///      the same second: the records carry the same updated_at and different
///      content. Skipping on equality leaves each machine holding its own copy
///      forever — a permanent fork that the merge reports as "skipped", i.e.
///      clean. Because stamps are wall clock at ONE-SECOND resolution and every
///      write path can land in the same second, this is reachable for any pair
///      of machines, not a contrived race.
///
///      So when the stamps are equal and the content differs, compare
///      contentDigest() of the two records and take the LARGER digest. Both
///      machines compute the same pair of digests from the same two records and
///      therefore pick the SAME winner — no third machine, no tie-break server,
///      no extra round trip. The loser's edit is discarded, which is
///      unavoidable: with no ordering information left, something must lose,
///      and losing deterministically on both sides is the only outcome that
///      converges.
[[nodiscard]] SyncAction decide(const LocalState &local, const SyncRecord &incoming);

/// A canonical digest of everything a merge can carry, used ONLY to break an
/// equal-stamp disagreement deterministically (see asymmetry 3 above).
///
/// "Canonical" means the digest depends on the record's CONTENT and not on how
/// it was serialized: fixed field order, fixed separators, an explicit marker
/// for an absent optional. Two machines holding byte-different JSON for the
/// same logical record must compute the same digest, or the tie-break would
/// pick different winners and create exactly the fork it exists to prevent.
///
/// It is deliberately NOT a cryptographic hash and makes no collision
/// resistance claim — a collision would only mean two records that already
/// disagree at the same second are treated as agreeing, which is the current
/// behaviour rather than a new failure.
[[nodiscard]] std::string contentDigest(const Task &task);

/// Plausibility bounds for a transported stamp.
///
/// A stamp is epoch SECONDS — Task.hpp and docs/sync.md both say so — but
/// nothing in the wire format enforces the unit, and the single most likely
/// mistake (an MCP client or a hand-edited file supplying MILLISECONDS) would
/// otherwise be accepted by a bare integer check. The value is stored verbatim
/// and compared against every future local write, so a millisecond stamp
/// freezes the task permanently: it wins every merge and every edit made on any
/// machine is skipped as "older" forever.
///
/// BOTH BOUNDS ARE ABSOLUTE CONSTANTS, and that is the whole point. An earlier
/// revision made the upper bound "no more than a year ahead of the LOCAL clock",
/// which made acceptance depend on WHICH MACHINE read the file: a machine whose
/// clock was more than a year out had every one of its records refused by every
/// peer (and refused every peer's records), so one wrong clock stopped the
/// backlog moving in one direction — while the header of this file promises the
/// merge is a function of the records alone. A bound that moves with a clock is
/// a bound that polices clocks, and a clock is not what is being checked.
///
/// The window is deliberately enormous (2001 to 2100) because its job is to
/// catch a WRONG UNIT, which is off by ~1000x, not to police a skew.
inline constexpr std::int64_t kMinPlausibleStamp{ 1000000000 };  ///< 2001-09-09.
inline constexpr std::int64_t kMaxPlausibleStamp{ 4102444800 };  ///< 2100-01-01.

/// True when `stamp` could be epoch seconds. See the constants above.
[[nodiscard]] bool isPlausibleStamp(std::int64_t stamp);

/// Clamp `stamp` into the plausible window, undoing a unit error where one is
/// recognisable.
///
/// Used by the parser INSTEAD of rejecting an implausible stamp, because the
/// rejection was fatal in a way that took the whole workflow down: a single
/// poisoned row (one a PREVIOUS build accepted and stored, or one a client
/// sends today) made the file unparseable, and `parseJsonl` fails whole-file by
/// contract — so the entire shared backlog became un-mergeable, and because
/// export_tasks re-parses its own output to check it, the poisoned machine
/// could no longer even EXPORT. Nothing in the API could repair it either:
/// update_task floors the stamp upward, so the poison survived; a corrected
/// record lost to the poison on stamp order; and a delete merely moved the
/// poison into a tombstone that could never be resurrected.
///
/// Clamping keeps the defence (a millisecond stamp cannot outrank real edits,
/// because it is corrected before it is stored) without the failure mode
/// (nothing is ever un-importable). A value above the window that divides down
/// into it is treated as MILLISECONDS and divided by 1000 — the value the
/// sender meant, rather than a ceiling that would leave the task frozen for
/// decades. It is a REPAIR, so it is not silent: the record carries
/// SyncRecord::stamp_adjusted and the merge counts it.
[[nodiscard]] std::int64_t clampPlausibleStamp(std::int64_t stamp);

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
