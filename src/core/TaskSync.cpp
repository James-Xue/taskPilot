// TaskSync.cpp — the export format and the merge decision
//
// TaskSync.hpp carries the reasoning; this file carries the mechanics. Two
// properties of the implementation are worth stating up front, because they are
// what makes a sync safe to run unattended:
//
//   - parseJsonLine is STRICT and total: every input either yields a record or
//     a named kInvalidArgument. There is no best-effort mode, because a lenient
//     parser is how a corrupt line becomes silent data loss — a record whose
//     fields were quietly dropped looks exactly like a record the other machine
//     meant to send that way.
//   - decide() compares two stamps and nothing else. It never reads a clock, so
//     the same pair of files merged in either direction reaches the same answer
//     on both machines; that is what makes the merge idempotent rather than
//     something that has to be run in a particular order.
//
// Neither function touches SQLite, the filesystem or the clock; the store owns
// all three (TaskStore::exportJsonl / mergeJsonl).

#include "core/TaskSync.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Result.hpp"
#include "core/Task.hpp"
#include "core/Uuid.hpp"

namespace taskpilot
{
namespace
{

// True when a line holds nothing a person would call content: empty, or only
// spaces, tabs, or the carriage return a Windows editor leaves behind.
//
// std::isspace is deliberately not used: it takes an int in the unsigned-char
// domain and would need a cast on every byte, while the bytes that matter here
// are known exactly. A JSON value never begins or ends with any of them.
[[nodiscard]] bool isBlankLine(const std::string &line)
{
    return std::string::npos == line.find_first_not_of(" \t\r\v\f");
}

// Echo of an offending value for an error message.
//
// Truncated on purpose: a hand-edited line is short, but a single line of a
// machine-written file can be large, and these messages travel back over
// JSON-RPC to whichever client asked for the merge. The true size is appended
// so a truncated echo is never mistaken for the whole value.
[[nodiscard]] std::string abbreviated(const std::string &text)
{
    constexpr std::size_t kMaxEcho{ 200 };

    if (kMaxEcho >= text.size())
    {
        return text;
    }

    return text.substr(0, kMaxEcho) + "... (" + std::to_string(text.size()) + " bytes)";
}

// Fetch a required string field.
//
// The two failures are reported separately — "the key is absent" and "the key
// is there but is not a string" are different defects in a hand-edited file —
// and the second one echoes the offending value, so the person can see what
// they actually typed rather than only what was expected.
[[nodiscard]] Result<std::string> requireString(const nlohmann::json &document,
                                                const std::string &what,
                                                const char *key)
{
    if (!document.contains(key))
    {
        return Error::invalidArgument(what + " has no '" + key + "'");
    }

    const nlohmann::json &value = document.at(key);
    if (!value.is_string())
    {
        return Error::invalidArgument(what + " has a non-string '" + key + "': ["
                                      + abbreviated(value.dump()) + "]");
    }

    return value.get<std::string>();
}

// Fetch a required 64-bit integer field: every timestamp in this format, plus
// the format version. Floats are refused rather than rounded, because a
// timestamp that arrived as 1.7e9 was written by something that is not this
// program, and guessing what it meant is how a merge silently mis-dates a task.
[[nodiscard]] Result<std::int64_t> requireInt64(const nlohmann::json &document,
                                                const std::string &what,
                                                const char *key)
{
    if (!document.contains(key))
    {
        return Error::invalidArgument(what + " has no '" + key + "'");
    }

    const nlohmann::json &value = document.at(key);
    if (!value.is_number_integer())
    {
        return Error::invalidArgument(what + " has a non-integer '" + key + "': ["
                                      + abbreviated(value.dump()) + "]");
    }

    return value.get<std::int64_t>();
}

// Fetch a required integer field that lands in one of Task's `int` members.
//
// The width is checked rather than cast. A JSON integer wider than `int` would
// otherwise narrow silently — with -Wconversion that costs a cast, and with a
// cast it costs the truth: the local task would then hold a number the file
// never contained, and nothing downstream could tell which side is wrong.
[[nodiscard]] Result<int> requireInt(const nlohmann::json &document,
                                     const std::string &what,
                                     const char *key)
{
    const Result<std::int64_t> wide = requireInt64(document, what, key);
    if (!wide.ok())
    {
        return wide.error();
    }

    if (wide.value() < static_cast<std::int64_t>(std::numeric_limits<int>::min())
        || wide.value() > static_cast<std::int64_t>(std::numeric_limits<int>::max()))
    {
        return Error::invalidArgument(what + " has an out-of-range '" + key + "': "
                                      + std::to_string(wide.value()));
    }

    return static_cast<int>(wide.value());
}

} // namespace

// Live task -> one JSONL line.
//
// The key set is fixed and parseJsonLine mirrors it exactly. Two things about
// it are load-bearing:
//
//   1. `id` is NOT emitted. The integer row number means "the ninth row THIS
//      database created" (see Uuid.hpp); transporting it would invite a caller
//      to match on it, and matching on it is precisely the data loss this whole
//      format exists to prevent.
//   2. `due_at` and `completed_at` are emitted as null rather than omitted when
//      unset, following Task.hpp's rule for the wire format: a client can then
//      read them unconditionally, and "no deadline" stays distinguishable from
//      "this writer does not report deadlines".
//
// Key order is nlohmann's (its object type is std::map, so the document comes
// out key-sorted); what the format promises is the key SET, which is identical
// on every line of every export.
std::string toJsonLine(const Task &task)
{
    nlohmann::json document;

    document["v"] = kExportFormatVersion;
    document["uid"] = task.uid;
    document["title"] = task.title;
    document["notes"] = task.notes;

    // The status crosses as its wire name, never its ordinal — reordering the
    // enum would renumber every export already on disk (see Task.cpp).
    document["status"] = toString(task.status);
    document["importance"] = task.importance;

    if (task.due_at.has_value())
    {
        document["due_at"] = *task.due_at;
    }
    else
    {
        document["due_at"] = nullptr;
    }

    document["blocks"] = task.blocks;
    document["tags"] = task.tags;
    document["created_at"] = task.created_at;
    document["updated_at"] = task.updated_at;

    if (task.completed_at.has_value())
    {
        document["completed_at"] = *task.completed_at;
    }
    else
    {
        document["completed_at"] = nullptr;
    }

    document["deleted"] = false;

    // One compact line, no indent, and no trailing newline: the caller joins
    // lines, so a newline here would become an empty line in the file.
    //
    // The error handler is `replace` for the reason serializeTags gives: this
    // signature has no error channel, and a task can carry text read back from a
    // corrupt row or supplied by an LLM client with a lone invalid UTF-8 byte.
    // Degrading that one byte beats the alternative — strict encoding would
    // throw on the export path, which the store, the CLI and the MCP server all
    // call without a catch.
    return document.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// Tombstone -> one JSONL line.
//
// Exactly three keys plus the version, and deliberately no task fields: a
// tombstone is a deletion, and a reader that ignored `deleted` must not be able
// to read stale content out of it. A title carried here would be a title that
// was deleted, waiting to be re-inserted by a merge that never noticed.
//
// Nothing is validated here. The caller owns the well-formed-uid invariant (see
// the header): the store is the only producer, and a uid that reached the file
// by hand is refused by parseJsonLine — before it can influence a merge.
std::string toJsonLine(const std::string &uid, std::int64_t deleted_at)
{
    nlohmann::json document;

    document["v"] = kExportFormatVersion;
    document["uid"] = uid;
    document["updated_at"] = deleted_at;
    document["deleted"] = true;

    return document.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// One JSONL line -> one record, or a named failure.
//
// The order of the checks below is the order the header states them in, and it
// matters: a line is first established to be JSON, then an object, then of a
// version this reader understands, then identified, and only then read field by
// field. Anything checked earlier would report the wrong problem for a line
// that is broken in two ways at once.
Result<SyncRecord> parseJsonLine(const std::string &line)
{
    // 1. Blank. Refused here rather than skipped: the header makes skipping
    //    blanks parseJsonl's decision, so that a file which is unexpectedly
    //    empty is never reported as a clean merge of zero records.
    if (line.empty() || isBlankLine(line))
    {
        return Error::invalidArgument("export line is blank; only parseJsonl skips blank lines");
    }

    // 2. Syntax. The exception is caught rather than avoided with
    //    allow_exceptions=false (the choice parseTags makes) because here the
    //    parser's own diagnostic — the column, and the bytes it choked on — is
    //    the most useful thing we can hand back. A malformed line is an
    //    EXPECTED outcome here, so it becomes a Result, not a throw.
    nlohmann::json document;
    try
    {
        document = nlohmann::json::parse(line);
    }
    catch (const nlohmann::json::exception &error)
    {
        return Error::invalidArgument("line is not valid JSON (" + std::string(error.what())
                                      + "): [" + abbreviated(line) + "]");
    }

    // 3. Shape. An array of records, or a bare scalar, is "not one record" —
    //    the same answer a missing key gets, because the merge has no way to
    //    treat either as one line it can act on.
    if (!document.is_object())
    {
        return Error::invalidArgument("line is not a JSON object: [" + abbreviated(line) + "]");
    }

    // 4. Format version. It rides on every line (the header's property 2) —
    //    there is no header line to read it from, because a header would be
    //    rewritten by both machines on every sync.
    const Result<std::int64_t> version = requireInt64(document, "record", "v");
    if (!version.ok())
    {
        return version.error();
    }

    // Two directions, and only one of them is tolerant — see step 7 for why the
    // tolerance is asymmetric:
    //   - newer: a later writer may have changed what a field MEANS while
    //     keeping its name, so reading it under today's rules is how a merge
    //     silently misinterprets a record. Refused, with an instruction.
    //   - unknown/older: no version of this format ever wrote it, so the line
    //     is not ours to interpret at all.
    if (static_cast<std::int64_t>(kExportFormatVersion) < version.value())
    {
        return Error::invalidArgument("record format version " + std::to_string(version.value())
                                      + " is newer than this reader understands ("
                                      + std::to_string(kExportFormatVersion)
                                      + "); upgrade taskPilot before merging this file");
    }

    if (static_cast<std::int64_t>(kExportFormatVersion) != version.value())
    {
        return Error::invalidArgument("record format version " + std::to_string(version.value())
                                      + " is not a known export version");
    }

    // 5. Identity. The uid is the only field a merge matches on (property 3),
    //    so a line without a usable one has no meaning at all — and a
    //    truncated uuid is the classic result of a copy-paste that half worked.
    const Result<std::string> uid = requireString(document, "record", "uid");
    if (!uid.ok())
    {
        return uid.error();
    }

    if (!looksLikeUuidV4(uid.value()))
    {
        return Error::invalidArgument("record uid is not a v4 uuid: ["
                                      + abbreviated(uid.value()) + "]");
    }

    SyncRecord record;
    // Anything other than the current version was refused above, so the record
    // can carry it as a constant rather than as the parsed number.
    record.format = kExportFormatVersion;
    record.uid = uid.value();

    // 6. Discriminator. An absent `deleted` reads as live: the key is written on
    //    every line, and a record that reaches here without it must still pass
    //    the live-record checks below — so a tombstone that lost its flag is
    //    refused there instead of being silently treated as a live task.
    bool deleted = false;
    if (document.contains("deleted"))
    {
        if (!document.at("deleted").is_boolean())
        {
            return Error::invalidArgument("record 'deleted' is not a boolean: ["
                                          + abbreviated(document.at("deleted").dump()) + "]");
        }
        deleted = document.at("deleted").get<bool>();
    }

    if (deleted)
    {
        // Only the stamp is checked, because only the stamp is read. The task
        // fields a tombstone does not carry are not merely absent, they are
        // meaningless (see SyncRecord), so record.task is left default.
        const Result<std::int64_t> stamp = requireInt64(document, "tombstone " + record.uid,
                                                        "updated_at");
        if (!stamp.ok())
        {
            return stamp.error();
        }

        record.deleted = true;
        record.stamp = stamp.value();
        return record;
    }

    // 7. A live record, field by field.
    //
    //    An unknown EXTRA key is ignored (nlohmann skips what we do not ask
    //    for), because a newer writer adding a field can only add information —
    //    including an `id`, which is ignored on purpose: it is a row number on
    //    some other machine and means nothing here.
    //
    //    An unknown VERSION is not tolerated (step 4), because it can change
    //    the meaning of a key we already understand — the one form of forward
    //    compatibility that cannot be absorbed silently.
    const std::string what = "live record " + record.uid;

    const Result<std::string> title = requireString(document, what, "title");
    if (!title.ok())
    {
        return title.error();
    }

    // Empty, but not blank-after-trimming: trimming is the store's validation
    // rule (TaskStore::addTask), not the format's. This layer transports text
    // verbatim rather than editing it.
    if (title.value().empty())
    {
        return Error::invalidArgument(what + " has an empty 'title'");
    }

    const Result<std::string> status_name = requireString(document, what, "status");
    if (!status_name.ok())
    {
        return status_name.error();
    }

    const std::optional<TaskStatus> status = taskStatusFromString(status_name.value());
    if (!status.has_value())
    {
        // Refused, never defaulted to kOpen. Task.hpp: a silent default is what
        // lets a corrupt record masquerade as active work in the ranked queue.
        return Error::invalidArgument(what + " has an unknown 'status': ["
                                      + abbreviated(status_name.value()) + "]");
    }

    const Result<int> importance = requireInt(document, what, "importance");
    if (!importance.ok())
    {
        return importance.error();
    }

    const Result<int> blocks = requireInt(document, what, "blocks");
    if (!blocks.ok())
    {
        return blocks.error();
    }

    const Result<std::int64_t> created_at = requireInt64(document, what, "created_at");
    if (!created_at.ok())
    {
        return created_at.error();
    }

    const Result<std::int64_t> updated_at = requireInt64(document, what, "updated_at");
    if (!updated_at.ok())
    {
        return updated_at.error();
    }

    if (!document.contains("tags"))
    {
        return Error::invalidArgument(what + " has no 'tags'");
    }

    if (!document.at("tags").is_array())
    {
        return Error::invalidArgument(what + " has a non-array 'tags': ["
                                      + abbreviated(document.at("tags").dump()) + "]");
    }

    std::vector<std::string> tags;
    tags.reserve(document.at("tags").size());
    for (const nlohmann::json &tag : document.at("tags"))
    {
        // A non-string element is refused rather than stringified: a tag nobody
        // typed must not become filterable on this machine.
        if (!tag.is_string())
        {
            return Error::invalidArgument(what + " has a non-string tag: ["
                                          + abbreviated(tag.dump()) + "]");
        }
        tags.push_back(tag.get<std::string>());
    }

    // `notes` is optional: our own writer always emits it, but a line that
    // leaves it out plainly means "no notes" rather than being corrupt.
    if (document.contains("notes"))
    {
        const Result<std::string> notes = requireString(document, what, "notes");
        if (!notes.ok())
        {
            return notes.error();
        }
        record.task.notes = notes.value();
    }

    // The two nullable timestamps. null (or absent) means unset; anything else
    // must be an integer — a date spelled as a string is the kind of near-miss
    // this parser exists to refuse rather than guess at.
    if (document.contains("due_at") && !document.at("due_at").is_null())
    {
        const Result<std::int64_t> due_at = requireInt64(document, what, "due_at");
        if (!due_at.ok())
        {
            return due_at.error();
        }
        record.task.due_at = due_at.value();
    }

    if (document.contains("completed_at") && !document.at("completed_at").is_null())
    {
        const Result<std::int64_t> completed_at = requireInt64(document, what, "completed_at");
        if (!completed_at.ok())
        {
            return completed_at.error();
        }
        record.task.completed_at = completed_at.value();
    }

    // Every field has been checked; only now is the task assembled, field by
    // field rather than from the line wholesale, so what reaches the store is
    // exactly the set of fields read above. `id` is not among them, and is left
    // at 0 explicitly: a record arriving from another machine must never be able
    // to name a row number in this database.
    record.task.uid = record.uid;
    record.task.id = 0;
    record.task.title = title.value();
    record.task.status = *status;
    record.task.importance = importance.value();
    record.task.blocks = blocks.value();
    record.task.created_at = created_at.value();
    record.task.updated_at = updated_at.value();
    record.task.tags = std::move(tags);

    // The stamp is updated_at's value, kept as its own field so decide() never
    // has to know whether it is looking at a task or at a tombstone.
    record.stamp = updated_at.value();

    return record;
}

// Whole export -> every record, or the first failure.
Result<std::vector<SyncRecord>> parseJsonl(const std::string &jsonl)
{
    std::vector<SyncRecord> records;

    // Physical lines are walked in order and EVERY one is counted, including
    // the skipped ones. The count is what a failure reports, and a line number
    // that does not match what the reader sees in an editor is worse than no
    // line number at all — it sends them to the wrong record.
    std::size_t position = 0;
    std::int64_t line_number = 0;

    while (true)
    {
        const std::size_t newline = jsonl.find('\n', position);
        const std::size_t end = (std::string::npos == newline) ? jsonl.size() : newline;
        const std::string line = jsonl.substr(position, end - position);
        ++line_number;

        // Blank lines are this layer's to skip: a trailing newline, a blank
        // separator, or a stray whitespace line is formatting, not a record.
        if (!isBlankLine(line))
        {
            const Result<SyncRecord> parsed = parseJsonLine(line);
            if (!parsed.ok())
            {
                // The first bad line fails the whole parse. A partial result
                // must never reach a merge (see the header): "these records
                // could not be read" and "the file was genuinely short" become
                // indistinguishable once the line numbers are gone.
                return Error::invalidArgument("line " + std::to_string(line_number) + ": "
                                              + parsed.error().message);
            }

            records.push_back(parsed.value());
        }

        // The final segment after the last newline (empty for a file that ends
        // with one) is the end of the walk, not a record.
        if (std::string::npos == newline)
        {
            break;
        }

        position = newline + 1;
    }

    return records;
}

// The merge rule — the header's table, compared in the order it is written.
//
// `absent` is the only state with neither flag set. A store holding BOTH a row
// and a tombstone for one uid is out of contract (deleteTask removes the row,
// a resurrect drops the tombstone); if one ever occurred, the branches below
// resolve it as "the live row wins", because a row is present evidence and a
// tombstone is bookkeeping.
SyncAction decide(const LocalState &local, const SyncRecord &incoming)
{
    if (!local.present && !local.tombstoned)
    {
        // A tombstone for a task we never had is a no-op, not an insert: there
        // is nothing to remove, and recording one would grow the tombstone
        // count on machines that never held the task.
        return incoming.deleted ? SyncAction::kSkip : SyncAction::kInsert;
    }

    if (!local.present)
    {
        // Local state: tombstoned, nothing else.
        if (incoming.deleted)
        {
            // Already gone; a second tombstone has nothing left to do.
            return SyncAction::kSkip;
        }

        // ASYMMETRY 2 of 2 — a post-delete edit resurrects (see the header), and
        // only a STRICTLY newer one. A tie leaves our deletion standing, which
        // mirrors the other asymmetry below: between "an edit made in the same
        // second is lost" and "something the user deleted comes back", the
        // version that loses the edit is the one that looks intentional.
        return (incoming.stamp > local.stamp) ? SyncAction::kResurrect : SyncAction::kSkip;
    }

    if (incoming.deleted)
    {
        // ASYMMETRY 1 of 2 — `>=`, where every other comparison here is a
        // strict `>`. A delete beats an edit made in the same second. See the
        // header: an edit lost this way can be retyped, whereas a task that
        // reappears after the user deleted it reads as the tool ignoring an
        // instruction.
        return (incoming.stamp >= local.stamp) ? SyncAction::kDelete : SyncAction::kSkip;
    }

    // Both sides live: the incoming record wins only if it is strictly newer.
    // Equal stamps mean the two machines agree on the last write, so re-applying
    // our own copy would be churn — and every merge would report churn as a
    // change, which is what MergeReport::clean() exists to avoid.
    return (incoming.stamp > local.stamp) ? SyncAction::kUpdate : SyncAction::kSkip;
}

} // namespace taskpilot
