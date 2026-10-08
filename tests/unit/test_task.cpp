// tests/unit/test_task.cpp — Task serialization and the tags column
//
// The cases that matter are the boundaries Task.cpp deliberately refuses to
// cross: an unrecognised status must not quietly become kOpen (that would
// promote an unknown row into the ranked queue), and a corrupt tags column
// must not throw while a list query is being assembled (that would hide every
// healthy task behind one bad row).
//
// The comma case is here to keep the JSON encoding honest — it is precisely
// the input a delimiter-joined column would have mangled.
//
// The uid/id cases pin the one distinction the wire format must never
// collapse: `id` is this database's row number and `uid` is the task's
// identity everywhere. Serializing them as separate keys of different JSON
// types is what stops a client from matching on the wrong one.

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Task.hpp"

namespace
{

// A fully populated task in a non-default state, so toJson is exercised on
// something other than the zero-value struct. The uid is a well-formed v4
// uuid, because a stored task always carries one: the store assigns it at
// insert time, so this is what a real serialized task looks like.
taskpilot::Task makeTask()
{
    taskpilot::Task task;
    task.uid = "9f3c1a2b-4d5e-4f60-8a9b-0c1d2e3f4a5b";
    task.id = 7;
    task.title = "Fix the reconnect bug";
    task.notes = "Two sockets drift after a short outage.";
    task.status = taskpilot::TaskStatus::kInProgress;
    task.importance = 5;
    task.due_at = 1700007200;
    task.blocks = 2;
    task.tags = { "bug", "ws" };
    task.created_at = 1700000000;
    task.updated_at = 1700000500;
    return task; // completed_at intentionally left unset
}

} // namespace

TEST(TaskStatusTest, WireNamesAreStable)
{
    // These strings live in the SQLite status column and in the JSON-RPC
    // payload at the same time, so a rename is a storage migration plus a
    // client break. Pin the exact spelling.
    EXPECT_EQ(std::string("open"), taskpilot::toString(taskpilot::TaskStatus::kOpen));
    EXPECT_EQ(std::string("in_progress"),
              taskpilot::toString(taskpilot::TaskStatus::kInProgress));
    EXPECT_EQ(std::string("done"), taskpilot::toString(taskpilot::TaskStatus::kDone));
    EXPECT_EQ(std::string("archived"),
              taskpilot::toString(taskpilot::TaskStatus::kArchived));
}

TEST(TaskStatusTest, RoundTripsThroughItsWireName)
{
    const std::vector<taskpilot::TaskStatus> statuses{
        taskpilot::TaskStatus::kOpen,
        taskpilot::TaskStatus::kInProgress,
        taskpilot::TaskStatus::kDone,
        taskpilot::TaskStatus::kArchived,
    };

    for (const taskpilot::TaskStatus status : statuses)
    {
        const std::optional<taskpilot::TaskStatus> parsed =
            taskpilot::taskStatusFromString(taskpilot::toString(status));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(status, *parsed);
    }
}

TEST(TaskStatusTest, UnknownNamesAreRejected)
{
    // Not "default to open": a caller that passes junk has to be told which
    // string failed. Case, padding and ordinals are all rejected, because
    // accepting any of them would make the wire format ambiguous.
    const std::vector<std::string> rejected{
        "",
        "OPEN",
        "Open",
        "in progress",
        "in-progress",
        "open ",
        " open",
        "done\n",
        "null",
        "0",
        "3",
    };

    for (const std::string &text : rejected)
    {
        EXPECT_FALSE(taskpilot::taskStatusFromString(text).has_value())
            << "unexpectedly accepted: [" << text << "]";
    }
}

TEST(TaskJsonTest, EmitsEveryFieldWithNullForAbsentTimestamps)
{
    const taskpilot::Task task = makeTask();

    const nlohmann::json expected = nlohmann::json::parse(R"({
        "uid": "9f3c1a2b-4d5e-4f60-8a9b-0c1d2e3f4a5b",
        "id": 7,
        "title": "Fix the reconnect bug",
        "notes": "Two sockets drift after a short outage.",
        "status": "in_progress",
        "importance": 5,
        "due_at": 1700007200,
        "blocks": 2,
        "tags": ["bug", "ws"],
        "created_at": 1700000000,
        "updated_at": 1700000500,
        "completed_at": null
    })");

    // Comparing whole documents pins the key set as well as the values: a
    // dropped key or an extra one fails here, which is the entire point of
    // promising clients a stable schema. The cost is that adding a field to
    // Task.cpp (as uid was added) breaks this test until the literal is
    // extended deliberately — and that is the feature, not the friction: a
    // schema change that no reviewer sees is exactly what must not happen.
    EXPECT_EQ(expected, taskpilot::toJson(task));
}

TEST(TaskJsonTest, UnsetOptionalFieldsAreNullNotMissing)
{
    taskpilot::Task task;
    task.id = 1;
    task.title = "no deadline yet";

    const nlohmann::json document = taskpilot::toJson(task);

    // The keys must EXIST and be null: a client reading task.due_at without a
    // null check should get "no deadline", not a missing-key error.
    ASSERT_TRUE(document.contains("due_at"));
    ASSERT_TRUE(document.contains("completed_at"));
    EXPECT_TRUE(document.at("due_at").is_null());
    EXPECT_TRUE(document.at("completed_at").is_null());
}

TEST(TaskJsonTest, DefaultsSurviveTheTrip)
{
    taskpilot::Task task;
    task.id = 1;
    task.title = "fresh";

    const nlohmann::json document = taskpilot::toJson(task);

    EXPECT_EQ(std::string("open"), document.at("status").get<std::string>());
    EXPECT_EQ(3, document.at("importance").get<int>());
    EXPECT_EQ(0, document.at("blocks").get<int>());
    EXPECT_TRUE(document.at("tags").is_array());
    EXPECT_TRUE(document.at("tags").empty());

    // Fields the store has not stamped yet: an un-saved task reports zero
    // timestamps and empty text rather than omitting them, so a client can
    // read every key unconditionally.
    EXPECT_EQ(std::int64_t{ 0 }, document.at("created_at").get<std::int64_t>());
    EXPECT_EQ(std::int64_t{ 0 }, document.at("updated_at").get<std::int64_t>());
    EXPECT_EQ(std::string(""), document.at("notes").get<std::string>());
}

TEST(TaskJsonTest, UnstoredTaskReportsAnEmptyUid)
{
    // uid is assigned by the store at insert time, so a task that has never
    // been saved legitimately has none. The key still exists and holds "" —
    // not null, not an absent key — because uid is a plain string everywhere
    // else and a client must be able to read it unconditionally. An empty
    // value cannot pass for an identity: the export parser's uuid shape check
    // rejects it, so an unstored task can never be matched or merged.
    const taskpilot::Task task;
    const nlohmann::json document = taskpilot::toJson(task);

    ASSERT_TRUE(document.contains("uid"));
    ASSERT_TRUE(document.at("uid").is_string());
    EXPECT_EQ(std::string(""), document.at("uid").get<std::string>());
}

TEST(TaskJsonTest, UidAndIdAreSeparateIdentities)
{
    // This pins why uid exists at all. `id` is a local row number, so two
    // machines that have each saved nine tasks both hold a row 9 and a merge
    // cannot tell them apart. Serialization therefore has to keep the two
    // identities under their own keys, each carrying its own field, and with
    // values of different JSON types (uuid string vs. integer row number) so
    // that reaching for the wrong one is a visible type error rather than a
    // plausible-looking match that silently pairs unrelated tasks.
    taskpilot::Task first = makeTask();
    first.uid = "9f3c1a2b-4d5e-4f60-8a9b-0c1d2e3f4a5b";
    first.id = 9;

    const nlohmann::json document = taskpilot::toJson(first);

    ASSERT_TRUE(document.contains("uid"));
    ASSERT_TRUE(document.contains("id"));
    EXPECT_TRUE(document.at("uid").is_string());
    EXPECT_TRUE(document.at("id").is_number_integer());

    // Neither key is an alias for the other: each reports its own field.
    EXPECT_EQ(first.uid, document.at("uid").get<std::string>());
    EXPECT_EQ(first.id, document.at("id").get<std::int64_t>());

    // The case the split exists for, made concrete: two tasks sharing a local
    // row number are still distinguishable documents, because only uid
    // travels between machines.
    taskpilot::Task second = first;
    second.uid = "0a1b2c3d-4e5f-4a6b-8c7d-8e9f0a1b2c3d";
    const nlohmann::json other = taskpilot::toJson(second);

    EXPECT_EQ(document.at("id"), other.at("id"));
    EXPECT_NE(document.at("uid"), other.at("uid"));
}

TEST(TaskJsonTest, SetTimestampsAppearAsNumbers)
{
    taskpilot::Task task;
    task.id = 2;
    task.title = "finished thing";
    task.status = taskpilot::TaskStatus::kDone;
    task.completed_at = 1700009999;

    const nlohmann::json document = taskpilot::toJson(task);
    ASSERT_TRUE(document.at("completed_at").is_number_integer());
    EXPECT_EQ(std::int64_t{ 1700009999 },
              document.at("completed_at").get<std::int64_t>());
}

TEST(TagsTest, RoundTripsThroughTheJsonColumn)
{
    const std::vector<std::string> tags{ "bug", "ws", "p1" };
    const std::string encoded = taskpilot::serializeTags(tags);

    // The column holds a JSON array rather than a joined string, so "no tags"
    // and "one empty tag" stay distinguishable and nothing is escaped by hand.
    EXPECT_EQ(nlohmann::json::parse(R"(["bug","ws","p1"])"), nlohmann::json::parse(encoded));
    EXPECT_EQ(tags, taskpilot::parseTags(encoded));
}

TEST(TagsTest, SurvivesCharactersThatADelimiterWouldBreak)
{
    // This is why the column is JSON. A comma-joined scheme splits the first
    // tag into three on the way back; quotes and backslashes break the
    // CSV-family writers in worse ways; a newline breaks line-oriented ones.
    const std::vector<std::string> tags{
        "blocked, waiting on legal",
        "quote \" inside",
        "back\\slash",
        "line\nbreak",
        "tab\there",
    };

    const std::vector<std::string> parsed = taskpilot::parseTags(taskpilot::serializeTags(tags));
    EXPECT_EQ(tags, parsed);
}

TEST(TagsTest, MultiByteUtf8SurvivesBothDirections)
{
    // Spelled as byte escapes so this source file stays pure ASCII, which
    // keeps the exact bytes under test independent of the encoding the
    // compiler assumed when it read the file.
    //
    // What is being tested: serializeTags calls dump() with ensure_ascii
    // disabled, so multi-byte sequences must pass through as UTF-8 rather
    // than being rewritten as \uXXXX escapes (which parseTags would then
    // return as the literal six-character text). Two- and three-byte
    // sequences are both covered.
    const std::vector<std::string> tags{
        "caf\xC3\xA9",                 // e-acute, two bytes
        "\xE6\x97\xA5\xE6\x9C\xAC",    // three-byte sequence
        "\xF0\x9F\x93\x8C",            // emoji, four bytes
    };

    EXPECT_EQ(tags, taskpilot::parseTags(taskpilot::serializeTags(tags)));
}

TEST(TagsTest, InvalidUtf8DoesNotThrow)
{
    // Tags are free-form text from an LLM client, so a lone invalid byte is
    // reachable. serializeTags runs on the write path and has no error
    // channel in its frozen signature, so it must degrade the byte (the
    // replacement character) rather than raise: the tag is cosmetic metadata,
    // the task is the data. Strict JSON encoding would throw type_error here.
    const std::vector<std::string> tags{ "ok", std::string("\xFF\xFE", 2) };

    std::string encoded;
    ASSERT_NO_THROW({ encoded = taskpilot::serializeTags(tags); });

    // Both entries are still there; only the un-encodable bytes changed.
    EXPECT_EQ(std::size_t{ 2 }, taskpilot::parseTags(encoded).size());
}

TEST(TagsTest, EmptyVectorSerializesToAnEmptyArray)
{
    EXPECT_EQ(nlohmann::json::parse("[]"), nlohmann::json::parse(taskpilot::serializeTags({})));
    EXPECT_TRUE(taskpilot::parseTags("[]").empty());
}

TEST(TagsTest, MalformedInputDegradesToNoTags)
{
    // None of these may throw. A corrupt column is read while a list query is
    // being assembled, so throwing would hide every healthy task behind one
    // bad row — the exact failure the header calls out.
    const std::vector<std::string> malformed{
        "",         // NULL or an empty column
        "not json", // hand-edited or truncated write
        "{}",       // valid JSON, wrong type
        "[1,2]",    // array of the wrong element type
        "[",        // truncated write
        "null",     // JSON null
        "\"tag\"",  // bare string instead of an array
    };

    for (const std::string &text : malformed)
    {
        EXPECT_TRUE(taskpilot::parseTags(text).empty()) << "text: [" << text << "]";
    }
}

TEST(TagsTest, NonStringElementsAreSkippedNotCoerced)
{
    // A partially mangled array keeps the tags that survived. Numbers are not
    // stringified: that would let a client filter on a tag nobody typed.
    const std::vector<std::string> parsed =
        taskpilot::parseTags(R"(["bug", 1, null, "ws", {"a":1}])");
    const std::vector<std::string> expected{ "bug", "ws" };
    EXPECT_EQ(expected, parsed);
}

TEST(TagsTest, WhitespaceInTheStoredJsonIsIrrelevant)
{
    const std::vector<std::string> expected{ "a", "b" };
    EXPECT_EQ(expected, taskpilot::parseTags("[\n  \"a\",\n  \"b\"\n]"));
}
