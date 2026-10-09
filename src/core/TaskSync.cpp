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
//     meant to send that way. It is also the only place in this file that reads
//     the clock, and only to check a transported stamp's UNIT
//     (isPlausibleStamp): a stamp in the wrong unit passes every other check
//     here, and then outranks every future local write forever.
//   - decide() orders two records by STAMP and breaks a stamp tie by CONTENT
//     (asymmetry 3 in the header). It never reads a clock: the same two records
//     produce the same answer on both machines, because the tie-break digest is
//     computed from the records alone. That is what makes the merge idempotent
//     and convergent rather than something that has to be run in a particular
//     order.
//
// Neither function touches SQLite or the filesystem; the store owns both
// (TaskStore::exportJsonl / mergeJsonl).

#include "core/TaskSync.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Clock.hpp"
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

// ---------------------------------------------------------------------------
// Content digest (see contentDigest in the header)
// ---------------------------------------------------------------------------

// The 64-bit FNV-1a parameters. Any deterministic byte mix would do; FNV-1a is
// short enough to read in full and needs no table.
constexpr std::uint64_t kFnvOffsetBasis{ 0xcbf29ce484222325ULL };
constexpr std::uint64_t kFnvPrime{ 0x100000001b3ULL };

// Fold one byte into a running FNV-1a state.
//
// The byte is read as UNSIGNED, not as a plain `char`: `char` is signed on
// x86 and a byte >= 0x80 would sign-extend, so the same UTF-8 title would
// digest differently on two platforms. The digest has to be identical on both
// machines or the tie-break picks its winner differently on each of them.
void mixByte(std::uint64_t &hash, char byte)
{
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
    hash *= kFnvPrime;
}

// Fold a string as its length, a ':' and then its bytes.
//
// Length-prefixed rather than delimiter-joined: title, notes and tags are user
// text and may contain any byte at all, so a delimiter would let one value
// mimic the end of another — the pair title "a:b" with empty notes against
// title "a" with notes "b:" is the same run of bytes under a delimiter, and two
// byte-identical digest inputs are two records the tie-break believes are
// equal. With the length in front, the encoding is unambiguous for every input,
// which is what makes the digest a function of the content rather than of where
// a colon happened to land.
void mixText(std::uint64_t &hash, const std::string &text)
{
    for (const char digit : std::to_string(text.size()))
    {
        mixByte(hash, digit);
    }

    mixByte(hash, ':');

    for (const char byte : text)
    {
        mixByte(hash, byte);
    }
}

// Fold a nullable timestamp.
//
// Absence gets its own marker rather than being folded as nothing at all: an
// unset deadline and a deadline of 0 are different records (0 is a real, if
// unwise, epoch stamp), and the merge rule promises that records which differ
// only in whether an optional is set do not digest alike.
void mixOptional(std::uint64_t &hash, const std::optional<std::int64_t> &value)
{
    if (!value.has_value())
    {
        mixText(hash, "unset");
        return;
    }

    mixText(hash, "set");
    mixText(hash, std::to_string(*value));
}

// Fold a tag list: the count, then each tag.
//
// The count is redundant with the per-tag lengths — the stream stays decodable
// without it — but it keeps a dump of the mixed bytes readable, and it makes
// the empty list's contribution explicit.
void mixTags(std::uint64_t &hash, const std::vector<std::string> &tags)
{
    mixText(hash, std::to_string(tags.size()));

    for (const std::string &tag : tags)
    {
        mixText(hash, tag);
    }
}

// Render a 64-bit digest as exactly 16 lowercase hex characters.
//
// The fixed WIDTH matters, and not only for looks: decide() compares the two
// digests as STRINGS, and a fixed-width lowercase rendering is the one spelling
// where the string order is also the numeric order. A variable-width rendering
// ("f0…" against "0f…") would order by first character rather than by leading
// zero, and "the larger digest wins" would then mean something a reader of the
// rule would not recognise.
[[nodiscard]] std::string hexDigest(std::uint64_t value)
{
    constexpr std::size_t kHexDigits{ 16 };
    constexpr char kDigits[]{ "0123456789abcdef" };

    std::string text(kHexDigits, '0');
    for (std::size_t position = 0; position < kHexDigits; ++position)
    {
        const std::size_t nibble_index = kHexDigits - 1 - position;
        const std::uint64_t nibble = (value >> (nibble_index * 4)) & 0xFULL;
        text[position] = kDigits[static_cast<std::size_t>(nibble)];
    }

    return text;
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
        const std::string what = "tombstone " + record.uid;
        const Result<std::int64_t> stamp = requireInt64(document, what, "updated_at");
        if (!stamp.ok())
        {
            return stamp.error();
        }

        // The stamp's UNIT is checked, not only its type. A stamp in the wrong
        // unit is worse than a corrupt one: every other check here accepts it,
        // the store writes it verbatim, and it then outranks every future local
        // write — the task freezes, because each edit on every machine loses the
        // comparison and is skipped as "older" forever. MILLISECONDS are the
        // mistake that actually happens (Date.now(), a browser console, a
        // hand-written script), and a millisecond tombstone would in addition
        // make the two machines' exports disagree on that line permanently.
        // REPAIRED, not refused. A refusal here is whole-file (parseJsonl's
        // contract), so one bad stamp made the entire shared backlog
        // un-mergeable — and, because export_tasks re-parses its own output,
        // un-EXPORTABLE on the machine that held it, with no API able to clear
        // it. See clampPlausibleStamp in the header for the full chain. The
        // defence survives the repair: the value is pulled inside the window
        // before it is ever stored, so a millisecond stamp still cannot outrank
        // real edits.
        record.deleted = true;
        record.stamp_adjusted = !isPlausibleStamp(stamp.value());
        record.stamp = clampPlausibleStamp(stamp.value());
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

    // The unit as well as the type — see the tombstone branch above for why a
    // wrong unit is the failure this parser cannot afford to let through. The
    // stamp is what decide() compares, so a frozen or absurd one silently
    // outranks real edits on every machine for as long as the record exists.
    // Repaired rather than refused — see the tombstone branch above and
    // clampPlausibleStamp in the header. The record is flagged so the merge can
    // count the repair: a peer or a client sending timestamps in the wrong unit
    // is a bug worth surfacing, just not one worth losing the backlog over.
    record.stamp_adjusted = !isPlausibleStamp(updated_at.value());
    const std::int64_t stamp_value = clampPlausibleStamp(updated_at.value());

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
    record.task.updated_at = stamp_value;
    record.task.tags = std::move(tags);

    // The stamp is updated_at's value (repaired if it arrived out of window),
    // kept as its own field so decide() never has to know whether it is looking
    // at a task or at a tombstone.
    record.stamp = stamp_value;

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

// ---------------------------------------------------------------------------
// The content digest: the tie-break's only input
// ---------------------------------------------------------------------------

// A canonical digest of everything a merge carries (see the header for what
// "canonical" has to mean, and why a non-cryptographic mix is enough).
std::string contentDigest(const Task &task)
{
    // FNV-1a, deliberately not a cryptographic hash: this orders two records
    // that already disagree at one stamp, and a collision would only make them
    // look equal — which is the behaviour that exists today, not a new failure.
    // Nothing here is a security boundary, so unpredictability would buy
    // nothing.
    //
    // The field ORDER below is part of the contract between machines and must
    // never be rearranged: each side computes its own record's digest and then
    // compares, so a revision that encoded the fields in a different order
    // would make the two machines choose different winners and fork exactly
    // where the tie-break promised to converge.
    std::uint64_t hash = kFnvOffsetBasis;

    // 1. Identity. The merge matches on the uid, so it is covered like every
    //    other field a record carries.
    mixText(hash, task.uid);

    // 2. The text a person edited. Empty values are folded with their length
    //    prefix like any other, so "empty" still contributes and cannot be
    //    confused with a field that was never written.
    mixText(hash, task.title);
    mixText(hash, task.notes);

    // 3. Status by its WIRE NAME, never its ordinal — renumbering the enum
    //    would otherwise change the digest of every unchanged record on the
    //    machine that was rebuilt (Task.cpp keeps the wire names stable for the
    //    same reason).
    mixText(hash, toString(task.status));

    // 4. The ranking fields, including the nullable deadline, where absence has
    //    a marker of its own (see mixOptional).
    mixText(hash, std::to_string(task.importance));
    mixOptional(hash, task.due_at);
    mixText(hash, std::to_string(task.blocks));
    mixTags(hash, task.tags);

    // 5. The timestamps a merge carries.
    mixText(hash, std::to_string(task.created_at));
    mixText(hash, std::to_string(task.updated_at));
    mixOptional(hash, task.completed_at);

    // `id` is DELIBERATELY absent, and this is the one omission worth stating:
    // it is the only Task field a merge never transports (Task.hpp: it means
    // "the ninth row THIS database created"). Folding it would make the same
    // logical record digest differently on the two machines — a real row number
    // in one store's local copy, 0 in the record that arrived from the other —
    // so the tie-break, which both sides compute independently, would turn on a
    // field that means nothing across machines. A digest that changes when a
    // row is renumbered is not a digest of content.

    return hexDigest(hash);
}

// ---------------------------------------------------------------------------
// The stamp window
// ---------------------------------------------------------------------------

// Could `stamp` be epoch SECONDS? The header carries the reasoning; these are
// the two bounds it names, and BOTH ARE CONSTANTS.
bool isPlausibleStamp(std::int64_t stamp)
{
    // 1. Not before 2001-09-09. Nothing this program or any client of it can
    //    mean lands below the bound; what does land there is 0 (a field that
    //    was never filled in), a negative (a subtraction that went the wrong
    //    way), a two-digit year, or seconds-since-boot — every one of them a
    //    value that would mis-order against real stamps forever.
    if (kMinPlausibleStamp > stamp)
    {
        return false;
    }

    // 2. Not past 2100-01-01. This is the bound that earns its keep, because the
    //    mistake that actually happens is a WRONG UNIT rather than a typo:
    //    MILLISECONDS instead of seconds are about 1000x too large — 1.79e12
    //    where the true count today is 1.79e9 — and arrive from any client that
    //    calls Date.now(), from a line edited in a browser console, or from a
    //    hand-written script. A millisecond stamp passes every other check in
    //    this file, is stored verbatim, and then outranks every future local
    //    write: each edit on every machine is skipped as "older", so the task
    //    freezes and nothing ever raises its stamp again.
    //
    //    It used to be "not more than a year ahead of the LOCAL clock", which
    //    made the verdict depend on the READER. That turned the check into a
    //    clock-quality test with a whole-file blast radius: a machine whose
    //    clock was more than a year out had its entire export refused by every
    //    peer, and refused every peer's file, so one wrong clock stopped the
    //    backlog moving in one direction — while this file's own header promises
    //    the merge answers from the records alone. A constant cannot do that.
    return kMaxPlausibleStamp >= stamp;
}

// Pull `stamp` inside the window. The header explains why the parser repairs
// instead of refusing; this is the repair.
std::int64_t clampPlausibleStamp(std::int64_t stamp)
{
    if (kMinPlausibleStamp > stamp)
    {
        return kMinPlausibleStamp;
    }
    if (kMaxPlausibleStamp >= stamp)
    {
        return stamp;
    }

    // Above the window. The overwhelmingly likely cause is MILLISECONDS, and the
    // honest repair is to undo the UNIT ERROR rather than to pin the value to the
    // ceiling: pinning would satisfy the parser while leaving the task frozen for
    // another 74 years, still outranking every real edit made anywhere. Dividing
    // by 1000 recovers the instant the sender actually meant, so a client that
    // called Date.now() is corrected rather than merely contained.
    const std::int64_t as_seconds = stamp / 1000;
    if (kMinPlausibleStamp <= as_seconds && kMaxPlausibleStamp >= as_seconds)
    {
        return as_seconds;
    }

    // Neither seconds nor a recognisable millisecond value: nothing can be
    // recovered, so sit on the ceiling. The record is still imported (dropping
    // it would be the data loss this whole path exists to avoid) and the merge
    // reports the adjustment.
    return kMaxPlausibleStamp;
}

// ---------------------------------------------------------------------------
// The merge rule
// ---------------------------------------------------------------------------

// The header's table, compared in the order it is written.
//
// `absent` is the only state with neither flag set. A store holding BOTH a row
// and a tombstone for one uid is out of contract (deleteTask removes the row,
// a resurrect drops the tombstone); if one ever occurred, the branches below
// resolve it as "the live row wins", because a row is present evidence and a
// tombstone is bookkeeping.
SyncAction decide(const LocalState &local, const SyncRecord &incoming)
{
    // 1. Absent: this store has never held the uid, as a task or as a tombstone.
    if (!local.present && !local.tombstoned)
    {
        // A tombstone for a uid we have never held is ADOPTED, not skipped.
        // The chain from "skip" to "the deleted task is back everywhere" is
        // short, and every step of it is invisible from this machine:
        //
        //   1. A machine that skips the tombstone records nothing.
        //   2. exportJsonl writes only what the store holds, so this machine's
        //      export carries NO line for the uid.
        //   3. A file with no line for a uid cannot be told from a file where
        //      the task was never deleted — and git propagates the omission as a
        //      deletion of the tombstone line.
        //   4. Every machine still holding the task live (it has not synced
        //      since the delete) learns nothing about the uid, keeps the task,
        //      and republishes it live.
        //
        // The deletion is undone on every machine, and the machine that caused
        // it reports "skipped: 1, clean". Absence from one side is never
        // evidence of deletion (the header's tombstone rule), so the one machine
        // that WAS told about it must carry the evidence onward.
        return incoming.deleted ? SyncAction::kRecordTombstone : SyncAction::kInsert;
    }

    // 2. Locally tombstoned, nothing live.
    if (!local.present)
    {
        if (incoming.deleted)
        {
            // Tombstone against tombstone: last write wins on the stamp, and
            // only a STRICTLY newer one has anything to write.
            //
            // Adopting the stamp is what lets two machines converge here. A
            // tombstone's stamp is otherwise written once and never revised —
            // deleting a LIVE row is the only thing that writes one, and a
            // second delete of the same uid is kNotFound — so two machines that
            // each deleted the task locally, in their own second, would keep
            // different stamps forever: their exports would differ on that line
            // permanently and git would conflict on it at every sync.
            return (incoming.stamp > local.stamp) ? SyncAction::kReviseTombstone
                                                  : SyncAction::kSkip;
        }

        // ASYMMETRY 2 — a post-delete edit resurrects (see the header), and only
        // a STRICTLY newer one. A tie leaves our deletion standing, which
        // mirrors asymmetry 1 below: between "an edit made in the same second is
        // lost" and "something the user deleted comes back", the version that
        // loses the edit is the one that looks intentional.
        return (incoming.stamp > local.stamp) ? SyncAction::kResurrect : SyncAction::kSkip;
    }

    // 3. A live row locally, and a tombstone arriving. ASYMMETRY 1 — `>=`, where
    //    every other comparison here is a strict `>`. A delete beats an edit
    //    made in the same second: an edit lost this way can be retyped, whereas
    //    a task that reappears after the user deleted it reads as the tool
    //    ignoring an instruction.
    if (incoming.deleted)
    {
        return (incoming.stamp >= local.stamp) ? SyncAction::kDelete : SyncAction::kSkip;
    }

    // 4. Both sides live: the incoming record wins only if it is STRICTLY newer.
    if (incoming.stamp > local.stamp)
    {
        return SyncAction::kUpdate;
    }

    if (incoming.stamp < local.stamp)
    {
        return SyncAction::kSkip;
    }

    // ASYMMETRY 3. Equal stamps, so the timestamps carry no ordering — but
    // "equal stamp" is NOT "equal content": two machines can each edit the same
    // task inside the same second, and nothing ever raises a stamp on its own.
    // Skipping here would leave each machine holding its own copy forever while
    // the merge reported "skipped: 1, clean".
    //
    // So the CONTENT decides, by the LARGER digest. Both machines see the same
    // two records and compute the same two digests with the same function, so
    // the record with the larger digest wins on both of them — no clock, no
    // tie-break server, no third round trip. The loser's edit is discarded,
    // which is unavoidable: with no ordering information left something must
    // lose, and losing deterministically on both sides is the only outcome that
    // converges instead of oscillating.
    //
    // A caller that handed over no local row cannot answer the content question
    // (an empty uid in LocalState::task; see its comment). Skipping is the
    // conservative answer: it preserves today's behaviour, whereas treating
    // "unknown content" as "different" would make each machine adopt the
    // other's equal-stamp record on every merge — the two would swap copies
    // forever and no merge would ever come out clean.
    if (local.task.uid.empty())
    {
        return SyncAction::kSkip;
    }

    const std::string ours = contentDigest(local.task);
    const std::string theirs = contentDigest(incoming.task);

    // Digests are fixed-width lowercase hex, so this string comparison is the
    // same order as comparing the underlying 64-bit values: the tie-break does
    // not depend on how a reader chooses to think about the digest.
    return (theirs > ours) ? SyncAction::kUpdate : SyncAction::kSkip;
}

} // namespace taskpilot
