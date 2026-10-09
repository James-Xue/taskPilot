#pragma once
// Uuid.hpp — version-4 UUID generation
//
// Why a UUID exists in this project at all:
//
// The primary key is an INTEGER AUTOINCREMENT, which is a MACHINE-LOCAL notion:
// it means "the ninth row this database ever created". Two machines that have
// each created nine tasks both have a row 9, and no amount of merging can tell
// them apart. That is fine while there is one database; the moment the backlog
// synchronises across machines it becomes silent data loss — the export of one
// machine overwrites the export of the other.
//
// So identity moves to a `uid`: assigned once at creation, never changed, and
// unique across machines because it is random rather than sequenced. The
// integer id stays, demoted to what it always really was — a local row number,
// useful for typing `done 7` at this machine's REPL and meaningless anywhere
// else.
//
// Randomness rather than time + hostname: a clock that steps backwards or two
// machines with the same hostname would both break the uniqueness that is the
// entire point, and detecting either after the fact is impossible.

#include <string>

#include "core/Result.hpp"

namespace taskpilot
{

/// Generate a version-4 (random) UUID in lowercase hyphenated form:
///   "9f3c1a2b-4d5e-4f60-8a9b-0c1d2e3f4a5b"
///
/// Returns kStorageFailure when no entropy source can be read. A weak or
/// partial-entropy fallback is deliberately NOT provided: a uuid that silently
/// repeats is worse than a loud failure, because the damage (two tasks sharing
/// an identity) only surfaces later, as data loss during a sync.
[[nodiscard]] Result<std::string> generateUuidV4();

/// Shape check for a v4 uuid: 36 characters, hyphens at 8/13/18/23, version
/// nibble '4', variant nibble one of 8/9/a/b, and every other character a
/// LOWERCASE hex digit.
///
/// Lowercase only, deliberately, and this is a correctness rule rather than a
/// style one. Every merge comparison keys on the uid STRING — the tasks.uid
/// column, its unique index, and the lookup in mergeJsonl. A checker that
/// accepted "9F3C…" alongside "9f3c…" would let one task carry two spellings,
/// which are two different strings and therefore two different identities: the
/// task would duplicate on every machine and the shared export file would hold
/// both spellings forever. The generator emits lowercase and nothing anywhere
/// normalises case, so accepting a second spelling buys nothing and costs a
/// silent fork.
///
/// This validates FORM only, never existence — it is how the export parser
/// rejects a truncated or hand-edited line before that line is allowed to
/// influence a merge. Existence is the store's question, answered by
/// TaskStore::getTaskByUid().
[[nodiscard]] bool looksLikeUuidV4(const std::string &text);

/// Derive a STABLE uuid-shaped identifier from `canonical`, which the caller
/// must have built in a canonical form (fixed field order, fixed separators,
/// empty values included).
///
/// Deterministic, unlike generateUuidV4(), and that is the entire point: it
/// exists for one situation only — backfilling uids onto rows that predate the
/// uid column. Two machines that entered the sync era holding the SAME rows
/// (one database copied to the other, a restored backup, an rsync) must derive
/// the SAME uid for the same row, or the first sync duplicates every task:
/// each machine would assert its own random uid for what is one task, the merge
/// would see two unknown uids, and insert both.
///
/// The output is formatted as a v4 uuid (version and variant nibbles stamped)
/// so that looksLikeUuidV4 accepts it and no reader needs a second code path.
/// It is NOT random, and must never be used where unpredictability matters —
/// it is an identity, not a secret.
///
/// Collision risk: 128 bits over a digest of the whole row. Two rows collide
/// only if their canonical forms are identical, which for genuinely different
/// tasks means identical titles, timestamps and local ids. When that happens
/// the two rows are indistinguishable to a merge anyway, so they merge rather
/// than corrupt. Callers should still include the local id and created_at in
/// the canonical form, since a copied database preserves both while two
/// independently created tasks do not.
[[nodiscard]] std::string deriveUuidFromText(const std::string &canonical);

} // namespace taskpilot
