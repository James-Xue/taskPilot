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
/// nibble '4', variant nibble one of 8/9/a/b.
///
/// This validates FORM only, never existence — it is how the export parser
/// rejects a truncated or hand-edited line before that line is allowed to
/// influence a merge. Existence is the store's question, answered by
/// TaskStore::getTaskByUid().
[[nodiscard]] bool looksLikeUuidV4(const std::string &text);

} // namespace taskpilot
