// tests/unit/test_task_sync.cpp — the export format and the merge decision
//
// Three things are being protected here, and each fails silently in production:
//
//   1. The decision table in TaskSync.hpp, every row of it. Its THREE deliberate
//      asymmetries (a delete beats a same-second edit; a strictly newer edit
//      resurrects a deletion; an equal-stamp disagreement is broken by content
//      digest) are exactly what someone tidying this code would "simplify" into
//      one comparison — and the damage is a task that reappears after being
//      deleted, an edit that vanishes, or two machines that never converge, on a
//      machine nobody is looking at days later. The same table's tombstone rows
//      are where the evidence that a deletion happened is either carried onward
//      or quietly dropped: a machine that declines to record a tombstone for a
//      uid it never held reports "clean" while it makes the deletion undoable.
//   2. The framing invariants of the file: one record per line, no header, one
//      line per uid, and no key this writer emits that a reader would refuse.
//      git merges the file line by line, so a violation of any of them turns a
//      routine merge into either a conflict on every sync or a lost record.
//   3. The stamp window. A stamp in the wrong UNIT — milliseconds instead of
//      seconds, the mistake a JSON client actually makes — passes every other
//      check, is stored verbatim, and then outranks every future local write on
//      every machine: the task freezes and nothing can ever raise its stamp
//      again. Nothing about that failure is visible in the record.
//
// The parser tests damage exactly one field of an otherwise valid line, so a
// failure names the property that broke rather than the whole line.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Clock.hpp"
#include "core/Result.hpp"
#include "core/Task.hpp"
#include "core/TaskSync.hpp"

namespace
{

// Well-formed v4 uuids: 36 characters, hyphens at 8/13/18/23, a '4' version
// nibble and an 8/9/a/b variant nibble. Written out rather than generated so
// the tests do not depend on the entropy source.
const std::string kUidA{ "9f3c1a2b-4d5e-4f60-8a9b-0c1d2e3f4a5b" };
const std::string kUidB{ "0b1c2d3e-4f50-4161-8273-8495a6b7c8d9" };
const std::string kUidC{ "7a1b2c3d-9e8f-4a5b-b6c7-d8e9f0a1b2c3" };

// A non-default task, so the export is exercised on something other than the
// zero-value struct. `id` is set on purpose: it must NOT survive the trip.
taskpilot::Task makeTask()
{
    taskpilot::Task task;
    task.uid = kUidA;
    task.id = 7;
    task.title = "Sync the backlog across machines";
    task.notes = "Primary key is machine-local; identity moves to the uid.";
    task.status = taskpilot::TaskStatus::kInProgress;
    task.importance = 5;
    task.due_at = 1700007200;
    task.blocks = 2;
    task.tags = { "sync", "p1" };
    task.created_at = 1700000000;
    task.updated_at = 1700000500;
    return task; // completed_at deliberately left unset
}

// The smallest line the parser accepts, as a JSON document rather than as text:
// each rejection test below then damages precisely one field of a line that is
// known good, which is what makes the failure attributable.
nlohmann::json minimalLiveDocument()
{
    nlohmann::json document;
    document["v"] = taskpilot::kExportFormatVersion;
    document["uid"] = kUidA;
    document["title"] = "a task";
    document["notes"] = "";
    document["status"] = "open";
    document["importance"] = 3;
    document["due_at"] = nullptr;
    document["blocks"] = 0;
    document["tags"] = nlohmann::json::array();
    document["created_at"] = 1700000000;
    document["updated_at"] = 1700000500;
    document["completed_at"] = nullptr;
    document["deleted"] = false;
    return document;
}

nlohmann::json minimalTombstoneDocument()
{
    nlohmann::json document;
    document["v"] = taskpilot::kExportFormatVersion;
    document["uid"] = kUidB;
    document["updated_at"] = 1700009999;
    document["deleted"] = true;
    return document;
}

// Parse a line that is expected to succeed. Failure is reported with the
// parser's own message rather than by calling value() on a failed Result, which
// would abort the test process instead of failing it.
taskpilot::SyncRecord parsedOrFail(const std::string &line)
{
    const taskpilot::Result<taskpilot::SyncRecord> parsed = taskpilot::parseJsonLine(line);
    if (!parsed.ok())
    {
        ADD_FAILURE() << "expected a valid line, got: " << parsed.error().message;
        return {};
    }
    return parsed.value();
}

std::vector<taskpilot::SyncRecord> fileOrFail(const std::string &jsonl)
{
    const taskpilot::Result<std::vector<taskpilot::SyncRecord>> parsed =
        taskpilot::parseJsonl(jsonl);
    if (!parsed.ok())
    {
        ADD_FAILURE() << "expected a valid file, got: " << parsed.error().message;
        return {};
    }
    return parsed.value();
}

// Assert that a line is refused as a kInvalidArgument whose message mentions
// `expectedFragment`. The message is checked as well as the code, because an
// error that says only "invalid argument" sends the person who hand-edited the
// file looking through thirteen fields for the one that broke.
void expectRejectedWith(const std::string &line, const std::string &expectedFragment)
{
    const taskpilot::Result<taskpilot::SyncRecord> parsed = taskpilot::parseJsonLine(line);
    if (parsed.ok())
    {
        ADD_FAILURE() << "expected a rejection, got a record for uid "
                      << parsed.value().uid << " from: " << line;
        return;
    }

    EXPECT_EQ(taskpilot::ErrorCode::kInvalidArgument, parsed.error().code)
        << "line: " << line;
    EXPECT_NE(std::string::npos, parsed.error().message.find(expectedFragment))
        << "message: " << parsed.error().message;
}

// Every field of a Task, including the two optionals — the fields a partial
// comparison would quietly let drift.
void expectTasksEqual(const taskpilot::Task &expected, const taskpilot::Task &actual)
{
    EXPECT_EQ(expected.uid, actual.uid);
    EXPECT_EQ(expected.title, actual.title);
    EXPECT_EQ(expected.notes, actual.notes);
    EXPECT_EQ(expected.status, actual.status);
    EXPECT_EQ(expected.importance, actual.importance);
    EXPECT_EQ(expected.due_at, actual.due_at);
    EXPECT_EQ(expected.blocks, actual.blocks);
    EXPECT_EQ(expected.tags, actual.tags);
    EXPECT_EQ(expected.created_at, actual.created_at);
    EXPECT_EQ(expected.updated_at, actual.updated_at);
    EXPECT_EQ(expected.completed_at, actual.completed_at);
}

// Write a parsed record back out as a line: a live record from its task, a
// tombstone from its uid and stamp. This is the exact inverse of parseJsonLine,
// which is what lets a round-trip test compare bytes rather than fields.
std::string lineFor(const taskpilot::SyncRecord &record)
{
    return record.deleted ? taskpilot::toJsonLine(record.uid, record.stamp)
                          : taskpilot::toJsonLine(record.task);
}

// The local side of a merge for a task this store holds: the stamp it compares
// with, and the row itself, which is what lets the equal-stamp tie-break look
// at the content. Both are what TaskStore::lookupLocal hands over.
taskpilot::LocalState stateFor(const taskpilot::Task &task)
{
    taskpilot::LocalState local;
    local.present = true;
    local.stamp = task.updated_at;
    local.task = task;
    return local;
}

// The same local state as it arrives from the other machine: a record, with its
// stamp lifted out of the task exactly as parseJsonLine does it.
taskpilot::SyncRecord recordFor(const taskpilot::Task &task)
{
    taskpilot::SyncRecord record;
    record.uid = task.uid;
    record.stamp = task.updated_at;
    record.task = task;
    return record;
}

// One row of the header's merge table.
struct DecideCase
{
    const char *name;
    bool local_present;
    bool local_tombstoned;
    std::int64_t local_stamp;
    bool incoming_deleted;
    std::int64_t incoming_stamp;
    taskpilot::SyncAction expected;
};

} // namespace

// ---------------------------------------------------------------------------
// Serialization: the line the writer produces
// ---------------------------------------------------------------------------

TEST(TaskSyncLineTest, LiveTaskEmitsTheDocumentedDocument)
{
    const std::string line = taskpilot::toJsonLine(makeTask());

    const nlohmann::json expected = nlohmann::json::parse(R"({
        "v": 1,
        "uid": "9f3c1a2b-4d5e-4f60-8a9b-0c1d2e3f4a5b",
        "title": "Sync the backlog across machines",
        "notes": "Primary key is machine-local; identity moves to the uid.",
        "status": "in_progress",
        "importance": 5,
        "due_at": 1700007200,
        "blocks": 2,
        "tags": ["sync", "p1"],
        "created_at": 1700000000,
        "updated_at": 1700000500,
        "completed_at": null,
        "deleted": false
    })");

    // Comparing the whole document pins the key SET as well as the values. That
    // is the point: two machines must emit the same keys or every sync conflicts
    // on lines that describe identical tasks.
    EXPECT_EQ(expected, nlohmann::json::parse(line));
}

TEST(TaskSyncLineTest, TheIntegerRowIdIsNeverTransported)
{
    taskpilot::Task task = makeTask();
    ASSERT_EQ(std::int64_t{ 7 }, task.id);

    const nlohmann::json document = nlohmann::json::parse(taskpilot::toJsonLine(task));

    // `id` means "the ninth row THIS database created" (Uuid.hpp). Emitting it
    // would invite a caller to match on it, and matching on it across machines
    // is the silent data loss the uid exists to prevent.
    EXPECT_FALSE(document.contains("id"));
}

TEST(TaskSyncLineTest, UnsetTimestampsAreNullNotMissing)
{
    taskpilot::Task task = makeTask();
    task.due_at.reset();

    const nlohmann::json document = nlohmann::json::parse(taskpilot::toJsonLine(task));

    // Keys must exist and be null, so "no deadline" stays distinguishable from
    // "this writer does not report deadlines" (Task.hpp's wire rule).
    ASSERT_TRUE(document.contains("due_at"));
    ASSERT_TRUE(document.contains("completed_at"));
    EXPECT_TRUE(document.at("due_at").is_null());
    EXPECT_TRUE(document.at("completed_at").is_null());
}

TEST(TaskSyncLineTest, TombstoneCarriesOnlyTheFourKeys)
{
    const std::string line = taskpilot::toJsonLine(kUidB, 1700009999);

    const nlohmann::json expected = nlohmann::json::parse(R"({
        "v": 1,
        "uid": "0b1c2d3e-4f50-4161-8273-8495a6b7c8d9",
        "updated_at": 1700009999,
        "deleted": true
    })");

    EXPECT_EQ(expected, nlohmann::json::parse(line));

    // "Nothing else" is the requirement, not a convenience: a reader that
    // ignored `deleted` must not be able to read stale content out of a
    // deletion, so no task field may appear even when the caller has one.
    EXPECT_EQ(std::size_t{ 4 }, nlohmann::json::parse(line).size());
}

TEST(TaskSyncLineTest, NoProducedLineContainsAnEmbeddedNewline)
{
    // The framing invariant the whole format rests on: one record per line is
    // what lets git merge two machines' exports line by line (header property
    // 1). A raw newline inside a title would split one record across two lines
    // and break every merge that touched it.
    taskpilot::Task task = makeTask();
    task.title = "line one\nline two";
    task.notes = "first\r\nsecond\n\nthird and a tab\there";

    const std::string line = taskpilot::toJsonLine(task);

    EXPECT_EQ(std::string::npos, line.find('\n'));
    EXPECT_EQ(std::string::npos, line.find('\r'));

    // Escaping must also be reversible: the text survives the trip unchanged,
    // which is what makes a task whose title spans lines representable at all.
    const taskpilot::SyncRecord record = parsedOrFail(line);
    EXPECT_EQ(task.title, record.task.title);
    EXPECT_EQ(task.notes, record.task.notes);

    // And the line is compact and canonical: re-serializing what it decodes to
    // reproduces it byte for byte, so a sync that rewrites an unchanged file
    // produces no diff for git to merge.
    EXPECT_EQ(line, nlohmann::json::parse(line).dump());
}

TEST(TaskSyncLineTest, AFileOfRecordsHoldsExactlyOneLinePerRecord)
{
    const std::string file = taskpilot::toJsonLine(makeTask()) + "\n"
                             + taskpilot::toJsonLine(kUidB, 1700000600) + "\n";

    const std::vector<taskpilot::SyncRecord> records = fileOrFail(file);

    ASSERT_EQ(std::size_t{ 2 }, records.size());
    EXPECT_FALSE(records[0].deleted);
    EXPECT_TRUE(records[1].deleted);

    // Two records, two newlines: the newline count IS the record count, which
    // is the property git merges on. The trailing newline is the file's, not a
    // record's, and parseJsonl skips the empty segment it leaves behind.
    const std::size_t newlines =
        static_cast<std::size_t>(std::count(file.begin(), file.end(), '\n'));
    EXPECT_EQ(records.size(), newlines);
}

// ---------------------------------------------------------------------------
// Round trips
// ---------------------------------------------------------------------------

TEST(TaskSyncRoundTripTest, LiveTaskSurvivesEveryField)
{
    const taskpilot::Task task = makeTask();
    const std::string line = taskpilot::toJsonLine(task);

    const taskpilot::SyncRecord record = parsedOrFail(line);

    expectTasksEqual(task, record.task);
    EXPECT_EQ(task.uid, record.uid);
    EXPECT_FALSE(record.deleted);
    EXPECT_EQ(task.updated_at, record.stamp);

    // Writing the parsed record back out reproduces the line exactly.
    EXPECT_EQ(line, lineFor(record));
}

TEST(TaskSyncRoundTripTest, DoneTaskKeepsItsCompletionTime)
{
    taskpilot::Task task = makeTask();
    task.status = taskpilot::TaskStatus::kDone;
    task.completed_at = 1700000900;

    const std::string line = taskpilot::toJsonLine(task);
    const taskpilot::SyncRecord record = parsedOrFail(line);

    ASSERT_TRUE(record.task.completed_at.has_value());
    EXPECT_EQ(std::int64_t{ 1700000900 }, record.task.completed_at.value());
    expectTasksEqual(task, record.task);
}

TEST(TaskSyncRoundTripTest, MinimalTaskKeepsBothTimestampsNull)
{
    // due_at and completed_at null at once: the two keys a client reads
    // unconditionally, and the two a "drop the key when unset" writer would
    // silently make unreadable.
    taskpilot::Task task;
    task.uid = kUidC;
    task.title = "no deadline, not finished";
    task.created_at = 1700000000;
    task.updated_at = 1700000001;

    const taskpilot::SyncRecord record = parsedOrFail(taskpilot::toJsonLine(task));

    EXPECT_FALSE(record.task.due_at.has_value());
    EXPECT_FALSE(record.task.completed_at.has_value());
    expectTasksEqual(task, record.task);
}

TEST(TaskSyncRoundTripTest, TagsSurviveCharactersADelimiterWouldBreak)
{
    taskpilot::Task task = makeTask();
    task.tags = { "blocked, waiting on legal", "quote \" inside", "back\\slash", "line\nbreak" };

    const taskpilot::SyncRecord record = parsedOrFail(taskpilot::toJsonLine(task));

    EXPECT_EQ(task.tags, record.task.tags);
}

TEST(TaskSyncRoundTripTest, TombstoneSurvivesWithNoTaskFieldsAtAll)
{
    const std::string line = taskpilot::toJsonLine(kUidB, 1700009999);

    const taskpilot::SyncRecord record = parsedOrFail(line);

    EXPECT_TRUE(record.deleted);
    EXPECT_EQ(kUidB, record.uid);
    EXPECT_EQ(std::int64_t{ 1700009999 }, record.stamp);
    EXPECT_EQ(taskpilot::kExportFormatVersion, record.format);

    // The task is deliberately empty on a tombstone: fields on a deletion are
    // not merely absent, they are meaningless (see SyncRecord).
    EXPECT_TRUE(record.task.title.empty());
    EXPECT_TRUE(record.task.tags.empty());
    EXPECT_FALSE(record.task.due_at.has_value());

    EXPECT_EQ(line, lineFor(record));
}

TEST(TaskSyncRoundTripTest, ParsedRecordsNeverCarryALocalRowId)
{
    // The source task had id 7. The record must not: a line arriving from
    // another machine must never be able to name a row in this database, and
    // a caller that trusted the field would write into the wrong task.
    const taskpilot::SyncRecord record = parsedOrFail(taskpilot::toJsonLine(makeTask()));

    EXPECT_EQ(std::int64_t{ 0 }, record.task.id);

    // The uid, in contrast, is carried — it is the only field that survives
    // synchronization.
    EXPECT_EQ(kUidA, record.task.uid);
}

// ---------------------------------------------------------------------------
// parseJsonLine: what it refuses, and why the reason is named
// ---------------------------------------------------------------------------

TEST(ParseJsonLineTest, RejectsBlankAndWhitespaceOnlyLines)
{
    // Blank is an ERROR here, unlike in parseJsonl, which is the layer that
    // decides blanks are skippable. If this function waved one through, a file
    // that had been truncated to nothing would parse as a clean merge of zero
    // records instead of reporting anything at all.
    expectRejectedWith("", "blank");
    expectRejectedWith("   ", "blank");
    expectRejectedWith("\t", "blank");
}

TEST(ParseJsonLineTest, RejectsTextThatIsNotJson)
{
    // A half-written line, the classic result of a copy or a write that was
    // interrupted. It is refused, never repaired.
    expectRejectedWith("not json", "not valid JSON");
    expectRejectedWith("{", "not valid JSON");
    expectRejectedWith(R"({"v":1,})", "not valid JSON");
    expectRejectedWith(R"({"v":1,"uid":"9f3c)", "not valid JSON");
}

TEST(ParseJsonLineTest, RejectsJsonThatIsNotAnObject)
{
    // A whole array — someone exported the backlog as one JSON document — is
    // not one record, so it gets the same answer as a missing key.
    expectRejectedWith("[]", "not a JSON object");
    expectRejectedWith("12", "not a JSON object");
    expectRejectedWith("null", "not a JSON object");
    expectRejectedWith("true", "not a JSON object");
    expectRejectedWith(R"("a bare string")", "not a JSON object");
}

TEST(ParseJsonLineTest, RejectsAMissingOrUnknownFormatVersion)
{
    // No header line carries the version (header property 2): it rides on every
    // record. A record without it is not ours to interpret.
    nlohmann::json without_version = minimalLiveDocument();
    without_version.erase("v");
    expectRejectedWith(without_version.dump(), "'v'");

    // A NEWER version is refused, and this is the interesting direction: a
    // later writer may have changed what a field means while keeping its name,
    // so reading it under today's rules would silently misinterpret the record.
    nlohmann::json too_new = minimalLiveDocument();
    too_new["v"] = taskpilot::kExportFormatVersion + 1;
    expectRejectedWith(too_new.dump(), "newer than this reader understands");

    // A version this format never had is refused too — there is no rule book
    // for it.
    nlohmann::json too_old = minimalLiveDocument();
    too_old["v"] = 0;
    expectRejectedWith(too_old.dump(), "not a known export version");

    nlohmann::json wrong_type = minimalLiveDocument();
    wrong_type["v"] = "1";
    expectRejectedWith(wrong_type.dump(), "'v'");
}

TEST(ParseJsonLineTest, RejectsAMissingOrMalformedUid)
{
    nlohmann::json missing = minimalLiveDocument();
    missing.erase("uid");
    expectRejectedWith(missing.dump(), "'uid'");

    nlohmann::json not_a_string = minimalLiveDocument();
    not_a_string["uid"] = 42;
    expectRejectedWith(not_a_string.dump(), "non-string 'uid'");

    // Shape failures a hand-edit produces: a missing character, a word where a
    // uuid belongs, and a v1 uuid — which is a real uuid, just not one this
    // format ever generates, so accepting it would mean accepting an identity
    // this program cannot have created.
    nlohmann::json truncated = minimalLiveDocument();
    truncated["uid"] = kUidA.substr(0, kUidA.size() - 1);
    expectRejectedWith(truncated.dump(), "not a v4 uuid");

    nlohmann::json word = minimalLiveDocument();
    word["uid"] = "not-a-uuid";
    expectRejectedWith(word.dump(), "not-a-uuid");

    nlohmann::json wrong_version = minimalLiveDocument();
    wrong_version["uid"] = "9f3c1a2b-4d5e-1f60-8a9b-0c1d2e3f4a5b";
    expectRejectedWith(wrong_version.dump(), "not a v4 uuid");
}

TEST(ParseJsonLineTest, RejectsALiveRecordWithAnUnknownStatus)
{
    // Never defaulted to kOpen: Task.hpp — a default here is what lets a
    // corrupt record masquerade as active work in the ranked queue.
    nlohmann::json blocked = minimalLiveDocument();
    blocked["status"] = "blocked";
    expectRejectedWith(blocked.dump(), "blocked");

    nlohmann::json empty = minimalLiveDocument();
    empty["status"] = "";
    expectRejectedWith(empty.dump(), "unknown 'status'");

    nlohmann::json missing = minimalLiveDocument();
    missing.erase("status");
    expectRejectedWith(missing.dump(), "'status'");
}

TEST(ParseJsonLineTest, RejectsALiveRecordWithNoTitle)
{
    nlohmann::json missing = minimalLiveDocument();
    missing.erase("title");
    expectRejectedWith(missing.dump(), "'title'");

    // Non-empty is required: a task with no text is not rankable work, so an
    // empty title is a corrupt record rather than a task with a blank name.
    nlohmann::json empty = minimalLiveDocument();
    empty["title"] = "";
    expectRejectedWith(empty.dump(), "empty 'title'");
}

TEST(ParseJsonLineTest, RejectsTagsThatAreNotAnArrayOfStrings)
{
    nlohmann::json not_an_array = minimalLiveDocument();
    not_an_array["tags"] = "bug";
    expectRejectedWith(not_an_array.dump(), "non-array 'tags'");

    nlohmann::json wrong_element = minimalLiveDocument();
    wrong_element["tags"] = nlohmann::json::array({ "ok", 7 });
    expectRejectedWith(wrong_element.dump(), "non-string tag");

    nlohmann::json missing = minimalLiveDocument();
    missing.erase("tags");
    expectRejectedWith(missing.dump(), "'tags'");
}

TEST(ParseJsonLineTest, RejectsALiveRecordMissingAnyRequiredField)
{
    // One case per field the header lists as required, so a field that stops
    // being checked is a failing test rather than a gap.
    const std::vector<std::string> required{
        "title", "status", "importance", "blocks", "created_at", "updated_at", "tags",
    };

    for (const std::string &key : required)
    {
        nlohmann::json damaged = minimalLiveDocument();
        damaged.erase(key);
        expectRejectedWith(damaged.dump(), "'" + key + "'");
    }
}

TEST(ParseJsonLineTest, RejectsNonIntegerNumbersWhereIntegersAreRequired)
{
    nlohmann::json string_importance = minimalLiveDocument();
    string_importance["importance"] = "high";
    expectRejectedWith(string_importance.dump(), "non-integer 'importance'");

    nlohmann::json float_stamp = minimalLiveDocument();
    float_stamp["updated_at"] = 1700000500.5;
    expectRejectedWith(float_stamp.dump(), "non-integer 'updated_at'");

    // A date spelled as text is the near-miss this strictness exists for: it
    // parses fine as JSON and would otherwise be coerced into a nonsense stamp.
    nlohmann::json string_due = minimalLiveDocument();
    string_due["due_at"] = "2023-11-14";
    expectRejectedWith(string_due.dump(), "non-integer 'due_at'");

    nlohmann::json string_completed = minimalLiveDocument();
    string_completed["completed_at"] = "2023-11-14";
    expectRejectedWith(string_completed.dump(), "non-integer 'completed_at'");

    nlohmann::json string_notes = minimalLiveDocument();
    string_notes["notes"] = 5;
    expectRejectedWith(string_notes.dump(), "non-string 'notes'");
}

TEST(ParseJsonLineTest, RejectsAnIntegerWiderThanTheFieldCanHold)
{
    // Refused rather than narrowed: a truncated importance would be a number the
    // file never contained, and nothing downstream could tell which side was
    // wrong.
    nlohmann::json huge = minimalLiveDocument();
    huge["importance"] = 5000000000;
    expectRejectedWith(huge.dump(), "out-of-range 'importance'");
}

TEST(ParseJsonLineTest, RejectsATombstoneWithoutAnIntegerStamp)
{
    // The stamp is the whole content of a tombstone: without it the delete
    // cannot compete with an edit, and a merge would have to guess.
    nlohmann::json missing = minimalTombstoneDocument();
    missing.erase("updated_at");
    expectRejectedWith(missing.dump(), "'updated_at'");

    nlohmann::json wrong_type = minimalTombstoneDocument();
    wrong_type["updated_at"] = "yesterday";
    expectRejectedWith(wrong_type.dump(), "non-integer 'updated_at'");
}

TEST(ParseJsonLineTest, RejectsANonBooleanDeletedFlag)
{
    nlohmann::json document = minimalLiveDocument();
    document["deleted"] = 0;
    expectRejectedWith(document.dump(), "'deleted'");
}

TEST(ParseJsonLineTest, RepairsALiveStampThatCannotBeEpochSeconds)
{
    // An implausible stamp is REPAIRED, not refused. Refusing it failed the
    // whole file (parseJsonl's contract), which took the entire shared backlog
    // down — and because export_tasks re-parses its own output, it left the one
    // machine holding the bad row unable to export at all, with no API able to
    // clear it. The defence survives the repair: the value is pulled inside the
    // window before it is ever stored, so a millisecond stamp still cannot
    // outrank real edits.
    //
    // Zero is a field that was never filled in, and a negative is a subtraction
    // that went the wrong way. Neither is a time this program can mean.
    nlohmann::json zero = minimalLiveDocument();
    zero["updated_at"] = 0;
    const taskpilot::SyncRecord clamped_low = parsedOrFail(zero.dump());
    EXPECT_EQ(taskpilot::kMinPlausibleStamp, clamped_low.stamp);
    EXPECT_TRUE(clamped_low.stamp_adjusted);

    nlohmann::json negative = minimalLiveDocument();
    negative["updated_at"] = -1700000500;
    EXPECT_EQ(taskpilot::kMinPlausibleStamp, parsedOrFail(negative.dump()).stamp);

    // A MILLISECOND value — the mistake that actually happens (Date.now(), a
    // browser console, a hand-written script). It is pulled back to the upper
    // bound rather than refused, which is what stops a single such row from
    // making the whole shared file un-mergeable, and stops it outranking every
    // future real edit.
    nlohmann::json milliseconds = minimalLiveDocument();
    milliseconds["updated_at"] = 1791445376000;
    const taskpilot::SyncRecord clamped_high = parsedOrFail(milliseconds.dump());
    // Divided back to the instant the sender meant (1791445376), NOT pinned to
    // the ceiling: pinning would satisfy the parser while leaving the task
    // frozen for another 74 years, still outranking every real edit.
    EXPECT_EQ(std::int64_t{ 1791445376 }, clamped_high.stamp);
    EXPECT_TRUE(clamped_high.stamp_adjusted);

    // A value in the right unit is NOT touched and NOT flagged: the window must
    // not quietly rewrite real time.
    nlohmann::json normal = minimalLiveDocument();
    normal["updated_at"] = 1700000500;
    const taskpilot::SyncRecord untouched = parsedOrFail(normal.dump());
    EXPECT_EQ(std::int64_t{ 1700000500 }, untouched.stamp);
    EXPECT_FALSE(untouched.stamp_adjusted);
}

TEST(ParseJsonLineTest, RepairsATombstoneStampThatCannotBeEpochSeconds)
{
    // Same repair as the live branch. A frozen tombstone stamp would make the
    // two machines' exports disagree on that line permanently, which is a git
    // conflict at every sync — so the window still has to catch it, it just
    // catches it by clamping instead of by making the file unreadable.
    nlohmann::json milliseconds = minimalTombstoneDocument();
    milliseconds["updated_at"] = 1791445376000;
    const taskpilot::SyncRecord clamped = parsedOrFail(milliseconds.dump());
    EXPECT_EQ(std::int64_t{ 1791445376 }, clamped.stamp);
    EXPECT_TRUE(clamped.stamp_adjusted);
    EXPECT_TRUE(clamped.deleted);

    nlohmann::json zero = minimalTombstoneDocument();
    zero["updated_at"] = 0;
    EXPECT_EQ(taskpilot::kMinPlausibleStamp, parsedOrFail(zero.dump()).stamp);

    nlohmann::json negative = minimalTombstoneDocument();
    negative["updated_at"] = -1;
    EXPECT_EQ(taskpilot::kMinPlausibleStamp, parsedOrFail(negative.dump()).stamp);
}

TEST(ParseJsonLineTest, IgnoresUnknownExtraKeys)
{
    // Forward tolerance, and only of this kind: a newer writer adding a field
    // can only add information, so it is ignored. A newer `v` cannot be, which
    // is why it is refused — see the version test above.
    nlohmann::json extended = minimalLiveDocument();
    extended["future_field"] = { { "nested", true } };
    extended["another"] = 3;

    // An `id` from another machine's export is ignored rather than trusted: the
    // row number means nothing here (Uuid.hpp).
    extended["id"] = 4242;

    const taskpilot::SyncRecord record = parsedOrFail(extended.dump());

    EXPECT_EQ(kUidA, record.uid);
    EXPECT_EQ(std::int64_t{ 0 }, record.task.id);
    EXPECT_EQ(taskpilot::kExportFormatVersion, record.format);
}

TEST(ParseJsonLineTest, TheStampComesFromUpdatedAtForALiveRecord)
{
    // decide() compares one field, so the parser is what guarantees the stamp
    // and the task's own updated_at agree.
    nlohmann::json document = minimalLiveDocument();
    document["updated_at"] = 1700007777;

    const taskpilot::SyncRecord record = parsedOrFail(document.dump());

    EXPECT_EQ(std::int64_t{ 1700007777 }, record.stamp);
    EXPECT_EQ(std::int64_t{ 1700007777 }, record.task.updated_at);
}

// ---------------------------------------------------------------------------
// parseJsonl: line numbers, blank lines, and the all-or-nothing rule
// ---------------------------------------------------------------------------

TEST(ParseJsonlTest, ParsesARecordPerLineInFileOrder)
{
    const std::string file = taskpilot::toJsonLine(makeTask()) + "\n"
                             + taskpilot::toJsonLine(kUidB, 1700000600) + "\n"
                             + taskpilot::toJsonLine(kUidC, 1700000700) + "\n";

    const std::vector<taskpilot::SyncRecord> records = fileOrFail(file);

    ASSERT_EQ(std::size_t{ 3 }, records.size());
    EXPECT_EQ(kUidA, records[0].uid);
    EXPECT_EQ(kUidB, records[1].uid);
    EXPECT_EQ(kUidC, records[2].uid);
}

TEST(ParseJsonlTest, ReportsTheOneBasedLineNumberAndReturnsNoRecords)
{
    const std::string file = taskpilot::toJsonLine(makeTask()) + "\n"
                             + taskpilot::toJsonLine(kUidB, 1700000600) + "\n"
                             + taskpilot::toJsonLine(kUidC, 1700000700) + "\n"
                             + "{ this line is not json }\n"
                             + taskpilot::toJsonLine(kUidB, 1700000800) + "\n";

    const taskpilot::Result<std::vector<taskpilot::SyncRecord>> parsed =
        taskpilot::parseJsonl(file);

    // All or nothing: a partial result must never reach a merge (see the
    // header) — "these records could not be read" and "the file was genuinely
    // short" are indistinguishable once the line numbers are gone. A Result
    // cannot carry both records and an error, so this one assertion is the
    // "no records were returned" check.
    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(taskpilot::ErrorCode::kInvalidArgument, parsed.error().code);

    // 1-based, and the number a person sees in an editor: the message OPENS
    // with "line 4:", so a number that referred to the parse of a batch of
    // non-blank lines — or to the column inside the line — could not satisfy it.
    EXPECT_EQ(std::size_t{ 0 }, parsed.error().message.find("line 4:"))
        << "message: " << parsed.error().message;
}

TEST(ParseJsonlTest, BlankAndWhitespaceLinesDoNotShiftTheLineNumbers)
{
    // A trailing newline, a blank separator and a whitespace-only line are all
    // formatting. The count must walk the PHYSICAL lines anyway, or the reported
    // number points the reader at a different record than the broken one.
    const std::string file = taskpilot::toJsonLine(makeTask()) + "\n"
                             + "\n"
                             + "   \t \n"
                             + "{ broken }\n"
                             + taskpilot::toJsonLine(kUidB, 1700000600) + "\n";

    const taskpilot::Result<std::vector<taskpilot::SyncRecord>> parsed =
        taskpilot::parseJsonl(file);

    ASSERT_FALSE(parsed.ok());
    EXPECT_EQ(std::size_t{ 0 }, parsed.error().message.find("line 4:"))
        << "message: " << parsed.error().message;
}

TEST(ParseJsonlTest, SkipsBlankLinesAndKeepsTheRecordsAroundThem)
{
    const std::string file = "\n"
                             + taskpilot::toJsonLine(makeTask()) + "\n"
                             + "   \n"
                             + taskpilot::toJsonLine(kUidB, 1700000600) + "\n"
                             + "\n";

    const std::vector<taskpilot::SyncRecord> records = fileOrFail(file);

    ASSERT_EQ(std::size_t{ 2 }, records.size());
    EXPECT_EQ(kUidA, records[0].uid);
    EXPECT_EQ(kUidB, records[1].uid);
}

TEST(ParseJsonlTest, AnEmptyFileYieldsNoRecords)
{
    // Every line here is blank, so every line is skipped: the empty file is not
    // an error at THIS layer (parseJsonLine is where a blank line is refused),
    // but the caller can see it read nothing and decide what that means.
    EXPECT_TRUE(fileOrFail("").empty());
    EXPECT_TRUE(fileOrFail("\n\n").empty());
}

TEST(ParseJsonlTest, AFinalLineWithoutATrailingNewlineIsStillARecord)
{
    const std::string file = taskpilot::toJsonLine(makeTask()) + "\n"
                             + taskpilot::toJsonLine(kUidB, 1700000600);

    const std::vector<taskpilot::SyncRecord> records = fileOrFail(file);

    ASSERT_EQ(std::size_t{ 2 }, records.size());
    EXPECT_EQ(kUidB, records[1].uid);
}

TEST(ParseJsonlTest, RewritingAParsedFileReproducesItByteForByte)
{
    // A sync rewrites the export in place. If a parse/write cycle perturbed the
    // bytes, every sync would produce a diff for git to merge — and the merge
    // would be resolving changes nobody made.
    taskpilot::Task edited = makeTask();
    edited.title = "a title with \"quotes\" and a \\ backslash";
    edited.notes = "and a newline\nin the middle";
    edited.status = taskpilot::TaskStatus::kDone;
    edited.completed_at = 1700001000;

    taskpilot::Task plain;
    plain.uid = kUidC;
    plain.title = "second";
    plain.created_at = 1700000000;
    plain.updated_at = 1700000001;

    const std::string file = taskpilot::toJsonLine(edited) + "\n"
                             + taskpilot::toJsonLine(plain) + "\n"
                             + taskpilot::toJsonLine(kUidB, 1700009999) + "\n";

    const std::vector<taskpilot::SyncRecord> records = fileOrFail(file);

    ASSERT_EQ(std::size_t{ 3 }, records.size());

    std::string rewritten;
    for (const taskpilot::SyncRecord &record : records)
    {
        rewritten += lineFor(record);
        rewritten += '\n';
    }

    EXPECT_EQ(file, rewritten);
}

TEST(ParseJsonlTest, EveryRecordInAFileHasOneLinePerUid)
{
    // Header property 3: one line per uid, as either the live task or its
    // tombstone. Two lines for one uid would leave a merge comparing a record
    // against itself, with the later line winning arbitrarily.
    const std::string file = taskpilot::toJsonLine(makeTask()) + "\n"
                             + taskpilot::toJsonLine(kUidB, 1700000600) + "\n"
                             + taskpilot::toJsonLine(kUidC, 1700000700) + "\n";

    const std::vector<taskpilot::SyncRecord> records = fileOrFail(file);

    for (std::size_t i = 0; i < records.size(); ++i)
    {
        for (std::size_t j = i + 1; j < records.size(); ++j)
        {
            EXPECT_NE(records[i].uid, records[j].uid);
        }
    }
}

// ---------------------------------------------------------------------------
// decide(): the table in the header, row by row
// ---------------------------------------------------------------------------

TEST(SyncDecideTest, WalksTheWholeDecisionTable)
{
    const std::vector<DecideCase> cases{
        // local absent
        { "absent + live", false, false, 0, false, 100, taskpilot::SyncAction::kInsert },
        { "absent + tombstone", false, false, 0, true, 100,
          taskpilot::SyncAction::kRecordTombstone },
        // local live
        { "live + live, newer", true, false, 100, false, 101, taskpilot::SyncAction::kUpdate },
        { "live + live, equal", true, false, 100, false, 100, taskpilot::SyncAction::kSkip },
        { "live + live, older", true, false, 100, false, 99, taskpilot::SyncAction::kSkip },
        { "live + tombstone, newer", true, false, 100, true, 101, taskpilot::SyncAction::kDelete },
        { "live + tombstone, equal", true, false, 100, true, 100, taskpilot::SyncAction::kDelete },
        { "live + tombstone, older", true, false, 100, true, 99, taskpilot::SyncAction::kSkip },
        // local tombstoned
        { "tombstoned + live, newer", false, true, 100, false, 101,
          taskpilot::SyncAction::kResurrect },
        { "tombstoned + live, equal", false, true, 100, false, 100, taskpilot::SyncAction::kSkip },
        { "tombstoned + live, older", false, true, 100, false, 99, taskpilot::SyncAction::kSkip },
        { "tombstoned + tombstone, newer", false, true, 100, true, 101,
          taskpilot::SyncAction::kReviseTombstone },
        { "tombstoned + tombstone, equal", false, true, 100, true, 100,
          taskpilot::SyncAction::kSkip },
        { "tombstoned + tombstone, older", false, true, 100, true, 99,
          taskpilot::SyncAction::kSkip },
    };

    for (const DecideCase &row : cases)
    {
        SCOPED_TRACE(row.name);

        taskpilot::LocalState local;
        local.present = row.local_present;
        local.tombstoned = row.local_tombstoned;
        local.stamp = row.local_stamp;

        // Only `deleted` and `stamp` are populated: this walk is the STAMP
        // table. The one row that needs more than the two stamps — an equal
        // stamp on two live records, which the content decides — is exercised
        // by the dedicated tests below, and the row here pins the documented
        // answer for a caller that supplied no local row at all.
        taskpilot::SyncRecord incoming;
        incoming.deleted = row.incoming_deleted;
        incoming.stamp = row.incoming_stamp;

        EXPECT_EQ(row.expected, taskpilot::decide(local, incoming));
    }
}

TEST(SyncDecideTest, ATombstoneForAnUnknownUidIsRecordedNotSkipped)
{
    // The most damaging row in the table, and the one whose damage is invisible
    // from the machine that causes it. Skipping records nothing; exportJsonl
    // then writes no line for the uid; git propagates the missing line as a
    // deletion of the tombstone; and every machine still holding the task (it
    // has not synced since the delete) learns nothing, keeps it, and
    // republishes it live. The deletion is undone everywhere while the machine
    // that dropped the evidence reports "skipped: 1, clean".
    taskpilot::LocalState local; // absent: neither a row nor a tombstone

    taskpilot::SyncRecord incoming;
    incoming.deleted = true;
    incoming.stamp = 1700000600;

    const taskpilot::SyncAction action = taskpilot::decide(local, incoming);

    // Asserted on its own line for the reason this case exists: a regression
    // here reads as "the evidence was dropped", not as "some other action came
    // back".
    EXPECT_NE(taskpilot::SyncAction::kSkip, action);
    EXPECT_EQ(taskpilot::SyncAction::kRecordTombstone, action);
}

TEST(SyncDecideTest, TombstoneAgainstTombstoneTakesTheStrictlyNewerStamp)
{
    // Two machines each deleted the same task locally, in their own second. A
    // tombstone's stamp is otherwise written once and never revised — only
    // deleting a LIVE row writes one, and a second delete of the same uid is
    // kNotFound — so without this branch the two stamps could never be
    // reconciled: the exports would differ on that line permanently, and git
    // would conflict on it at every sync.
    taskpilot::LocalState local;
    local.tombstoned = true;
    local.stamp = 1700000600;

    taskpilot::SyncRecord incoming;
    incoming.deleted = true;

    incoming.stamp = 1700000601;
    EXPECT_EQ(taskpilot::SyncAction::kReviseTombstone, taskpilot::decide(local, incoming));

    // Equal and older have nothing to write: the stamp we hold already IS the
    // later write, and re-writing it would make every merge report a change.
    incoming.stamp = 1700000600;
    EXPECT_EQ(taskpilot::SyncAction::kSkip, taskpilot::decide(local, incoming));

    incoming.stamp = 1700000599;
    EXPECT_EQ(taskpilot::SyncAction::kSkip, taskpilot::decide(local, incoming));
}

TEST(SyncDecideTest, AnEqualStampIsBrokenByContentInBothDirections)
{
    // Two edits of one task made inside the same epoch second: the stamps are
    // equal, so the timestamps carry no ordering at all, and neither record's
    // stamp will ever rise on its own. Skipping would leave each machine
    // holding its own copy forever while the merge reported "clean".
    taskpilot::Task alpha = makeTask();
    alpha.title = "edited on one machine";
    taskpilot::Task beta = makeTask();
    beta.title = "edited on the other machine";
    beta.updated_at = alpha.updated_at; // the premise: one second, two edits

    const std::string alpha_digest = taskpilot::contentDigest(alpha);
    const std::string beta_digest = taskpilot::contentDigest(beta);
    ASSERT_NE(alpha_digest, beta_digest) << "the pair must disagree";

    // Named by digest rather than by title, so the expectation describes the
    // RULE (the larger digest wins) instead of which title happens to hash
    // above the other.
    const taskpilot::Task &larger = (beta_digest > alpha_digest) ? beta : alpha;
    const taskpilot::Task &smaller = (beta_digest > alpha_digest) ? alpha : beta;

    // The machine holding the smaller record adopts the larger one...
    EXPECT_EQ(taskpilot::SyncAction::kUpdate,
              taskpilot::decide(stateFor(smaller), recordFor(larger)));

    // ...and the machine holding the larger record keeps it. Both directions
    // are asserted because both machines run the same comparison on the same
    // pair: exactly one copy wins, and it is the SAME copy on both of them.
    // Two winners, or none, would mean the two machines swap or deadlock
    // instead of converging.
    EXPECT_EQ(taskpilot::SyncAction::kSkip,
              taskpilot::decide(stateFor(larger), recordFor(smaller)));
}

TEST(SyncDecideTest, AnEqualStampWithEqualContentSkips)
{
    // Equal stamps and equal content is the ordinary case — the two machines
    // agree — and it must stay a skip, or every merge would rewrite records
    // nobody changed and MergeReport::clean() would never be true.
    taskpilot::Task task = makeTask();
    taskpilot::Task same = task;
    same.id = task.id + 1; // the local row number is not content (see the digest tests)

    EXPECT_EQ(taskpilot::contentDigest(task), taskpilot::contentDigest(same));
    EXPECT_EQ(taskpilot::SyncAction::kSkip, taskpilot::decide(stateFor(task), recordFor(same)));
}

TEST(SyncDecideTest, AnEqualStampWithNoLocalRowSuppliedSkips)
{
    // LocalState::task is the only way decide() can see our copy's content, and
    // a caller that supplied none cannot answer the content question. Keeping
    // our copy is the conservative answer: treating "unknown" as "different"
    // would make each machine adopt the other's equal-stamp record on every
    // merge, so the two would swap copies forever and no merge would ever come
    // out clean.
    taskpilot::LocalState local;
    local.present = true;
    local.stamp = 1700000500;
    // local.task deliberately left default: an empty uid means "no row handed
    // over", which a stored task can never be.

    taskpilot::SyncRecord incoming = recordFor(makeTask());
    ASSERT_EQ(local.stamp, incoming.stamp);

    EXPECT_EQ(taskpilot::SyncAction::kSkip, taskpilot::decide(local, incoming));
}

TEST(SyncDecideTest, ADeleteBeatsALiveRecordAtTheSameStamp)
{
    // Asymmetry 1 of 3, explicitly. `>=`, not `>`: a delete beats an edit made
    // in the same second, because a task reappearing after the user deleted it
    // reads as the tool ignoring an instruction, while an edit lost to a
    // same-second delete can be retyped.
    taskpilot::LocalState local;
    local.present = true;
    local.stamp = 1700000500;

    taskpilot::SyncRecord incoming;
    incoming.deleted = true;
    incoming.stamp = 1700000500;

    EXPECT_EQ(taskpilot::SyncAction::kDelete, taskpilot::decide(local, incoming));
}

TEST(SyncDecideTest, EqualStampsDoNotResurrect)
{
    // Asymmetry 2 of 3, and the mirror of the test above: a tie here means our
    // delete stands, so the two equal-stamp cases resolve the same way — toward
    // the deletion. If this returned kResurrect, a sync between two machines
    // would oscillate.
    taskpilot::LocalState local;
    local.tombstoned = true;
    local.stamp = 1700000500;

    taskpilot::SyncRecord incoming;
    incoming.stamp = 1700000500;

    EXPECT_EQ(taskpilot::SyncAction::kSkip, taskpilot::decide(local, incoming));
}

TEST(SyncDecideTest, AOneSecondNewerEditResurrects)
{
    // The boundary of asymmetry 2: a post-delete edit is not permanent — delete
    // is just the latest write — so one second of difference is enough to bring
    // the task back, tombstone and all.
    taskpilot::LocalState local;
    local.tombstoned = true;
    local.stamp = 1700000500;

    taskpilot::SyncRecord incoming;
    incoming.stamp = 1700000501;

    EXPECT_EQ(taskpilot::SyncAction::kResurrect, taskpilot::decide(local, incoming));
}

// ---------------------------------------------------------------------------
// The two halves agreeing: what is written is what is read
// ---------------------------------------------------------------------------

TEST(TaskSyncIntegrationTest, ALiveLineAndItsTombstoneDecideInFavourOfTheDelete)
{
    // End to end through the format: take a real task, serialize it, then
    // serialize its deletion, and check that the merge prefers the deletion at
    // an equal stamp. This is the sequence a sync performs, and it is the case
    // where the wrong comparison loses a delete.
    const taskpilot::Task task = makeTask();
    const taskpilot::SyncRecord live = parsedOrFail(taskpilot::toJsonLine(task));
    const taskpilot::SyncRecord tombstone =
        parsedOrFail(taskpilot::toJsonLine(task.uid, task.updated_at));

    // The local row is handed over, so the second expectation below is the
    // equal-content answer and not the no-local-row fallback.
    taskpilot::LocalState local = stateFor(task);
    ASSERT_EQ(live.stamp, local.stamp);

    EXPECT_EQ(taskpilot::SyncAction::kDelete, taskpilot::decide(local, tombstone));
    EXPECT_EQ(taskpilot::SyncAction::kSkip, taskpilot::decide(local, live));
}

TEST(TaskSyncIntegrationTest, ATombstoneForAnUnknownUidIsCarriedOnward)
{
    // The critical row, end to end: the tombstone really is what the wire
    // carries, and a machine that never held the task must record it, because
    // its own export is the only thing that can go on telling the machines
    // which still hold the task that it was deleted.
    const taskpilot::SyncRecord tombstone =
        parsedOrFail(taskpilot::toJsonLine(kUidB, 1700009999));

    taskpilot::LocalState never_held_it; // neither a row nor a tombstone

    const taskpilot::SyncAction action = taskpilot::decide(never_held_it, tombstone);
    EXPECT_NE(taskpilot::SyncAction::kSkip, action);
    EXPECT_EQ(taskpilot::SyncAction::kRecordTombstone, action);
}

TEST(TaskSyncIntegrationTest, ASameSecondEditOnTwoMachinesConvergesOnOneCopy)
{
    // Both edits round-tripped through the wire format, as a merge does them.
    // The two machines must decide in OPPOSITE directions on the same pair:
    // exactly one copy wins, the same one on both sides, and which one is
    // decided by the digest rather than by which machine is asking. Anything
    // else — two winners, or none — is the permanent fork this rule exists to
    // close, and it is invisible in a report that calls both merges clean.
    taskpilot::Task on_this_machine = makeTask();
    on_this_machine.title = "edited on this machine";
    taskpilot::Task on_the_other_machine = makeTask();
    on_the_other_machine.title = "edited on the other machine";
    ASSERT_EQ(on_this_machine.updated_at, on_the_other_machine.updated_at);

    const taskpilot::SyncRecord here = parsedOrFail(taskpilot::toJsonLine(on_this_machine));
    const taskpilot::SyncRecord there = parsedOrFail(taskpilot::toJsonLine(on_the_other_machine));

    taskpilot::LocalState local_here;
    local_here.present = true;
    local_here.stamp = here.stamp;
    local_here.task = here.task;

    taskpilot::LocalState local_there;
    local_there.present = true;
    local_there.stamp = there.stamp;
    local_there.task = there.task;

    const taskpilot::SyncAction this_machine = taskpilot::decide(local_here, there);
    const taskpilot::SyncAction other_machine = taskpilot::decide(local_there, here);

    const bool this_machine_adopts = (taskpilot::SyncAction::kUpdate == this_machine);
    EXPECT_TRUE(this_machine_adopts || (taskpilot::SyncAction::kUpdate == other_machine))
        << "neither machine adopted the other's copy: they stay forked";
    EXPECT_FALSE(this_machine_adopts && (taskpilot::SyncAction::kUpdate == other_machine))
        << "both machines adopted the other's copy: they would swap forever";

    // Whichever direction this machine chose, the other one chose the mirror of
    // it — the assertion that makes the outcome convergence rather than an
    // agreement to differ.
    EXPECT_EQ(this_machine_adopts ? taskpilot::SyncAction::kSkip : taskpilot::SyncAction::kUpdate,
              other_machine);
}

TEST(TaskSyncIntegrationTest, ALaterEditInTheSameFileWinsOverAnEarlierOne)
{
    // Two records with one uid never appear in a file we write, but a merge
    // still has to resolve them deterministically: the file is applied in
    // order, so the later stamp must be the one that survives.
    taskpilot::Task task = makeTask();

    const taskpilot::SyncRecord older = parsedOrFail(taskpilot::toJsonLine(task));
    task.updated_at += 60;
    task.title = "edited on the other machine";
    const taskpilot::SyncRecord newer = parsedOrFail(taskpilot::toJsonLine(task));

    taskpilot::LocalState local;
    local.present = true;
    local.stamp = older.stamp;
    local.task = older.task;

    EXPECT_EQ(taskpilot::SyncAction::kUpdate, taskpilot::decide(local, newer));

    // The second line repeats the first one's uid at the same stamp, and here
    // the content is genuinely identical — so this is a skip for the right
    // reason (equal digests), not only because a local row was missing.
    EXPECT_EQ(taskpilot::SyncAction::kSkip, taskpilot::decide(local, older));
}

// ---------------------------------------------------------------------------
// contentDigest: what the tie-break is allowed to depend on
// ---------------------------------------------------------------------------

TEST(ContentDigestTest, IsDeterministicAndFixedWidthLowercaseHex)
{
    const taskpilot::Task task = makeTask();

    const std::string first = taskpilot::contentDigest(task);
    EXPECT_EQ(first, taskpilot::contentDigest(task));

    // decide() compares two digests as STRINGS, and a fixed-width lowercase
    // rendering is what makes that comparison the same order as comparing the
    // 64-bit values: without it, "the larger digest wins" would order by first
    // character instead of by leading zero.
    EXPECT_EQ(std::size_t{ 16 }, first.size());
    for (const char character : first)
    {
        const bool is_digit = character >= '0' && character <= '9';
        const bool is_lowercase_hex_letter = character >= 'a' && character <= 'f';
        EXPECT_TRUE(is_digit || is_lowercase_hex_letter) << "not lowercase hex: " << first;
    }
}

TEST(ContentDigestTest, ChangesForEveryFieldAMergeCarries)
{
    const taskpilot::Task base = makeTask();

    // One field at a time, each from the pristine base, so a field left out of
    // the digest is named by the case that catches it instead of hiding behind a
    // comparison of two whole records.
    const auto expect_change = [&base](const taskpilot::Task &mutated, const std::string &what)
    {
        EXPECT_NE(taskpilot::contentDigest(base), taskpilot::contentDigest(mutated)) << what;
    };

    taskpilot::Task mutated = base;
    mutated.uid = kUidC;
    expect_change(mutated, "uid");

    mutated = base;
    mutated.title += " (edited)";
    expect_change(mutated, "title");

    mutated = base;
    mutated.notes += " (edited)";
    expect_change(mutated, "notes");

    mutated = base;
    mutated.notes.clear();
    expect_change(mutated, "notes emptied");

    mutated = base;
    mutated.status = taskpilot::TaskStatus::kDone;
    expect_change(mutated, "status");

    mutated = base;
    mutated.importance = base.importance - 1;
    expect_change(mutated, "importance");

    mutated = base;
    mutated.due_at.reset();
    expect_change(mutated, "due_at unset");

    mutated = base;
    mutated.due_at = *base.due_at + 1;
    expect_change(mutated, "due_at moved");

    mutated = base;
    mutated.blocks = base.blocks + 1;
    expect_change(mutated, "blocks");

    mutated = base;
    mutated.tags = { "sync" };
    expect_change(mutated, "tags shortened");

    mutated = base;
    mutated.tags = { "sync", "p1", "p2" };
    expect_change(mutated, "tags extended");

    mutated = base;
    mutated.tags = {};
    expect_change(mutated, "tags emptied");

    mutated = base;
    mutated.created_at = base.created_at + 1;
    expect_change(mutated, "created_at");

    mutated = base;
    mutated.updated_at = base.updated_at + 1;
    expect_change(mutated, "updated_at");

    mutated = base;
    mutated.completed_at = base.updated_at;
    expect_change(mutated, "completed_at set");

    // The one field that must NOT matter. `id` is a local row number that a
    // merge never transports, so a digest that moved when a row was renumbered
    // would make the same record digest differently on the two machines — and
    // the tie-break, which both sides compute independently, would turn on
    // nothing.
    mutated = base;
    mutated.id = base.id + 1;
    EXPECT_EQ(taskpilot::contentDigest(base), taskpilot::contentDigest(mutated)) << "id";
}

TEST(ContentDigestTest, AnUnsetOptionalDoesNotDigestLikeZero)
{
    // "No deadline" and "a deadline at the epoch" are different records, and the
    // merge rule's promise that a content difference is detected rests on the
    // digest telling them apart. Same for the completion stamp.
    taskpilot::Task unset = makeTask();
    unset.due_at.reset();
    unset.completed_at.reset();

    taskpilot::Task zero = unset;
    zero.due_at = 0;
    EXPECT_NE(taskpilot::contentDigest(unset), taskpilot::contentDigest(zero)) << "due_at";

    taskpilot::Task zero_completed = unset;
    zero_completed.completed_at = 0;
    EXPECT_NE(taskpilot::contentDigest(unset), taskpilot::contentDigest(zero_completed))
        << "completed_at";
}

TEST(ContentDigestTest, ContentCannotImitateAFieldBoundary)
{
    // The encoding has to be unambiguous for EVERY input, not only for the ones
    // a person types: title, notes and tags are free text, and a tag may contain
    // a comma, a colon or anything else. If a value could stand in for a value
    // plus the start of the next field, two different records would digest
    // alike — and two records that digest alike are, to the tie-break, the same
    // record, so one machine would keep an edit the other believes is already
    // there.
    //
    // Both pairs below are the same bytes under a delimiter-joined encoding and
    // different records under the length-prefixed one the implementation uses.
    taskpilot::Task split_at_the_colon = makeTask();
    split_at_the_colon.title = "a:b";
    split_at_the_colon.notes = "";
    taskpilot::Task split_after_the_colon = makeTask();
    split_after_the_colon.title = "a";
    split_after_the_colon.notes = "b:";
    EXPECT_NE(taskpilot::contentDigest(split_at_the_colon),
              taskpilot::contentDigest(split_after_the_colon))
        << "title/notes boundary";

    taskpilot::Task one_tag = makeTask();
    one_tag.tags = { "a:b", "c" };
    taskpilot::Task two_tags = makeTask();
    two_tags.tags = { "a", "b:c" };
    EXPECT_NE(taskpilot::contentDigest(one_tag), taskpilot::contentDigest(two_tags))
        << "between two tags";
}

// ---------------------------------------------------------------------------
// isPlausibleStamp: the unit check, and the clock it is measured against
// ---------------------------------------------------------------------------

TEST(StampPlausibilityTest, RefusesValuesThatCannotBeEpochSeconds)
{
    // MILLISECONDS, the mistake that will actually happen: this value is about
    // 2026-10-08 expressed in milliseconds, where the same instant in seconds is
    // 1791445376. It is ~1000x too large, so the window refuses it — and a
    // stamp that got through here would outrank every future local write.
    EXPECT_FALSE(taskpilot::isPlausibleStamp(1791445376000));

    // 0 (a field that was never filled in), a negative (a subtraction that went
    // the wrong way), and the second before the lower bound.
    EXPECT_FALSE(taskpilot::isPlausibleStamp(0));
    EXPECT_FALSE(taskpilot::isPlausibleStamp(-1));
    EXPECT_FALSE(taskpilot::isPlausibleStamp(taskpilot::kMinPlausibleStamp - 1));

    // The bounds themselves are inside the window, and an ordinary seconds
    // value from 2023 clearly is too.
    EXPECT_TRUE(taskpilot::isPlausibleStamp(taskpilot::kMinPlausibleStamp));
    EXPECT_TRUE(taskpilot::isPlausibleStamp(1700000500));

    // The upper bound is relative to the local clock, and it has to stay wide
    // enough that a merely skewed clock is not "implausible": a minute of
    // margin on each side of it keeps this from being flaky when a second ticks
    // over between the two reads.
    // BOTH bounds are constants, so the verdict is a property of the RECORD and
    // not of whoever read it. It used to be relative to the local clock, which
    // meant a machine more than a year out refused every peer's file and had
    // every one of its own refused — one wrong clock stopping the backlog in one
    // direction, while this file's header promises the merge answers from the
    // records alone.
    EXPECT_TRUE(taskpilot::isPlausibleStamp(taskpilot::kMinPlausibleStamp));
    EXPECT_TRUE(taskpilot::isPlausibleStamp(taskpilot::kMaxPlausibleStamp));
    EXPECT_FALSE(taskpilot::isPlausibleStamp(taskpilot::kMinPlausibleStamp - 1));
    EXPECT_FALSE(taskpilot::isPlausibleStamp(taskpilot::kMaxPlausibleStamp + 1));

    // A clock that is wildly wrong is NOT what this window polices: only a wrong
    // UNIT (~1000x, i.e. milliseconds) lands outside it.
    const taskpilot::SystemClock clock;
    const std::int64_t now = clock.nowEpochSeconds();
    EXPECT_TRUE(taskpilot::isPlausibleStamp(now));
    EXPECT_TRUE(taskpilot::isPlausibleStamp(now + (366LL * 24 * 3600)));
    EXPECT_FALSE(taskpilot::isPlausibleStamp(now * 1000));
}
