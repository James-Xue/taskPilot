// Task.cpp — serialization for the Task record
//
// Two directions, two different failure philosophies, and the difference is
// deliberate:
//
//   - Text/JSON -> Task (taskStatusFromString, parseTags) parses data that
//     came from outside: a CLI argument, an LLM tool call, a row in a file
//     somebody edited. Bad input is an EXPECTED outcome here, so it is
//     reported (nullopt, empty vector) instead of throwing. The caller is the
//     one that knows whether a bad status is worth an error message.
//
//   - Task -> JSON/text (toJson, serializeTags) encodes data the program
//     itself constructed. There is no failure to report, so nothing here
//     throws either.

#include "core/Task.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace taskpilot
{

// Wire and disk names for a status.
//
// These strings cross every boundary taskPilot has: the SQLite `status`
// column, the JSON-RPC payload, and the MCP tool schema. They are frozen in
// the same sense toString(ErrorCode) is — a rename is a storage migration
// plus a client break, so the mapping is asserted in tests.
//
// No `default:` label on the switch, so adding a status without handling it
// here fails the build under -Werror.
std::string toString(TaskStatus status)
{
    switch (status)
    {
        case TaskStatus::kOpen:
            return "open";
        case TaskStatus::kInProgress:
            return "in_progress";
        case TaskStatus::kDone:
            return "done";
        case TaskStatus::kArchived:
            return "archived";
    }

    // Only reachable via a static_cast of a value outside the enum — i.e. a
    // corrupt row or a coding error, never a status a caller could produce.
    // "unknown" rather than "open" on purpose: defaulting here would let a
    // corrupt task masquerade as active work in the ranked queue, and a wrong
    // answer is worse than an obviously unparseable one. The mirror of this
    // decision is taskStatusFromString declining to guess.
    return "unknown";
}

// Parse a wire name back into the enum.
//
// Anything unrecognised yields nullopt, including case variants ("OPEN") and
// surrounding whitespace ("open "). Being strict is the point: the caller
// turns nullopt into a kInvalidArgument error naming the offending string,
// whereas a silent default to kOpen would mark an unknown row as actionable
// work and quietly flood the queue.
std::optional<TaskStatus> taskStatusFromString(const std::string &text)
{
    if ("open" == text)
    {
        return TaskStatus::kOpen;
    }
    if ("in_progress" == text)
    {
        return TaskStatus::kInProgress;
    }
    if ("done" == text)
    {
        return TaskStatus::kDone;
    }
    if ("archived" == text)
    {
        return TaskStatus::kArchived;
    }

    return std::nullopt;
}

// Serialize a task for the wire.
//
// Every field is emitted, and the two nullable ones are emitted as JSON null
// rather than dropped when unset. A client can therefore read task.due_at
// unconditionally, and a client-side schema check stays meaningful; an
// optional key would make "no deadline" indistinguishable from "this server
// does not report deadlines".
//
// `uid` is a plain string rather than an optional, so a task the store has
// not saved yet is emitted as "" — present and readable like every other key,
// and distinguishable from a real uuid rather than absent.
//
// Key order is not significant: nlohmann::json's default object type is
// std::map, so the emitted document is key-sorted regardless of the order the
// assignments below appear in.
nlohmann::json toJson(const Task &task)
{
    nlohmann::json out;

    // The two identities, assigned side by side here even though the
    // key-sorted output will not show them that way. `uid` is which task this
    // is on every machine; `id` is only this database's row number (see the
    // field comments in Task.hpp). Anything matching two machines' tasks must
    // use uid — matching on id silently pairs two unrelated "row 9"s, which
    // is the data loss the uid field exists to prevent.
    out["uid"] = task.uid;
    out["id"] = task.id;

    // Text fields, copied verbatim. Notes is emitted even when empty for the
    // same stable-schema reason as the nullable fields below.
    out["title"] = task.title;
    out["notes"] = task.notes;

    // The enum crosses the wire as its name, never as its ordinal: ordinals
    // shift the moment someone reorders the enum declaration, and a client
    // that stored the number would then read a kDone task as kArchived.
    out["status"] = toString(task.status);
    out["importance"] = task.importance;

    if (task.due_at.has_value())
    {
        out["due_at"] = *task.due_at;
    }
    else
    {
        out["due_at"] = nullptr;
    }

    out["blocks"] = task.blocks;

    // std::vector<std::string> converts to a JSON array via nlohmann's
    // serializer; tags never become a delimiter-joined string (see
    // serializeTags for why).
    out["tags"] = task.tags;

    out["created_at"] = task.created_at;
    out["updated_at"] = task.updated_at;

    // completed_at mirrors due_at: present as null until the task reaches
    // kDone, so the two "when" fields behave identically for a client.
    if (task.completed_at.has_value())
    {
        out["completed_at"] = *task.completed_at;
    }
    else
    {
        out["completed_at"] = nullptr;
    }

    return out;
}

// Tags column <- vector<string>.
//
// JSON rather than a comma-joined string, for the reason the header gives: a
// tag may legitimately contain a comma ("blocked, waiting on legal"), and
// every delimiter choice just moves the escaping problem somewhere else.
// dump() handles quotes, backslashes and control characters for free, and
// parseTags() is its exact inverse.
//
// The error handler is `replace` (not the default `strict`) so this function
// cannot throw: tags are free-form strings supplied by an LLM client and may
// contain a lone invalid UTF-8 byte. Strict mode would raise a type_error
// there, and the frozen signature has no way to report failure and no error
// channel the caller could act on — so the choice is between degrading one
// tag to U+FFFD and failing an entire write. Degrading is right: the tag is
// cosmetic metadata, the task is the data.
std::string serializeTags(const std::vector<std::string> &tags)
{
    const nlohmann::json array = tags;
    return array.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// Tags column -> vector<string>.
//
// This function must never throw. TaskStore calls it while materializing rows
// for a list query, so one corrupt row would otherwise abort the whole query
// and hide every healthy task behind it — a far worse outcome than rendering
// that one task as untagged. Three degradation steps:
//
//   1. parse with allow_exceptions=false, which returns a discarded value on
//      malformed input instead of raising parse_error;
//   2. anything that is not an array (an object, a bare scalar, a discarded
//      value, the empty string) yields no tags at all;
//   3. non-string elements inside an array are skipped rather than coerced,
//      so a partially mangled column still yields the tags that survived.
std::vector<std::string> parseTags(const std::string &text)
{
    const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array())
    {
        return {};
    }

    std::vector<std::string> tags;
    tags.reserve(parsed.size());

    for (const nlohmann::json &entry : parsed)
    {
        if (entry.is_string())
        {
            tags.push_back(entry.get<std::string>());
        }
    }

    return tags;
}

} // namespace taskpilot
