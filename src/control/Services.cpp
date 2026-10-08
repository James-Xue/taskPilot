// Services.cpp — implementation of the taskPilot API surface
//
// The catalog in methodSpecs() is the single source of truth for the API:
// JsonRpcServer's dispatch registry and McpServer's `tools/list` are both
// derived from it, so a method cannot be advertised on one transport and
// missing on the other. Adding a method means adding one spec here plus one
// dispatch row in invoke(); tests/unit/test_services.cpp cross-checks the two
// by invoking every advertised name with empty params and asserting that none
// of them answers "unknown method".
//
// Parameter handling is strict on purpose. A missing required key and a
// present-but-wrong-typed value are both kInvalidArgument, and the message
// names the parameter together with the type that actually arrived. Coercing
// "5" into 5, or silently using a default for a misspelled key, would make a
// failed call indistinguishable from a successful one — the worst possible
// outcome for an LLM client that cannot read the daemon's logs.

#include "control/Services.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/PriorityEngine.hpp"
#include "core/Task.hpp"
#include "core/TaskSync.hpp"
#include "core/Weights.hpp"

namespace taskpilot
{

namespace
{

/// Reserved params key carrying the instant invoke() sampled for this call.
///
/// The frozen handler signature takes only `params`, so the sampled instant
/// travels with the params of the call it belongs to. That is per-call value
/// propagation with no shared mutable state, which is exactly what keeps
/// invoke() safe to call concurrently — a member or a static would be either a
/// data race or a lie about the class's thread-safety. A value supplied by a
/// client under this key is always overwritten, so a caller cannot pin or
/// forge the instant the ranking was computed against.
constexpr const char *kNowParamKey{ "__taskpilot_now" };

/// The instant invoke() sampled for this call.
///
/// Every handler is private and reached only through invoke(), which always
/// stamps the key above; 0 (the epoch) is returned only for a handler reached
/// some other way, which would be a programming error rather than caller
/// input, and is not worth throwing across the RPC boundary for.
[[nodiscard]] std::int64_t sampledNow(const nlohmann::json &params)
{
    const auto it = params.find(kNowParamKey);
    if (it == params.end() || !it->is_number_integer())
    {
        return 0;
    }
    return it->get<std::int64_t>();
}

/// Read an optional string parameter.
///
/// Contract shared by the three optional readers here: `out` is left at the
/// caller's initial value when the key is absent — that initial value IS the
/// fallback — while a key that is present with the wrong type is rejected
/// rather than falling back, because a silently ignored argument and a failed
/// one look identical from the other side of the wire.
[[nodiscard]] Status optionalString(const nlohmann::json &params, const char *key,
                                    std::string &out)
{
    const auto it = params.find(key);
    if (it == params.end())
    {
        return Unit{};
    }
    if (!it->is_string())
    {
        return Error::invalidArgument(std::string{ "parameter '" } + key +
                                      "' must be a string, got " + it->type_name());
    }
    out = it->get<std::string>();
    return Unit{};
}

/// Read an optional boolean parameter. See optionalString for the contract.
[[nodiscard]] Status optionalBool(const nlohmann::json &params, const char *key, bool &out)
{
    const auto it = params.find(key);
    if (it == params.end())
    {
        return Unit{};
    }
    if (!it->is_boolean())
    {
        return Error::invalidArgument(std::string{ "parameter '" } + key +
                                      "' must be a boolean, got " + it->type_name());
    }
    out = it->get<bool>();
    return Unit{};
}

/// Read an optional array of strings. See optionalString for the contract.
///
/// The error for a bad element names its index, because "tags must be an array
/// of strings" is useless when one of nine entries is a number.
[[nodiscard]] Status optionalStringArray(const nlohmann::json &params, const char *key,
                                        std::vector<std::string> &out)
{
    const auto it = params.find(key);
    if (it == params.end())
    {
        return Unit{};
    }
    if (!it->is_array())
    {
        return Error::invalidArgument(std::string{ "parameter '" } + key +
                                      "' must be an array of strings, got " + it->type_name());
    }

    std::vector<std::string> parsed;
    parsed.reserve(it->size());
    for (const nlohmann::json &element : *it)
    {
        if (!element.is_string())
        {
            return Error::invalidArgument(std::string{ "parameter '" } + key + "[" +
                                          std::to_string(parsed.size()) +
                                          "]' must be a string, got " + element.type_name());
        }
        parsed.push_back(element.get<std::string>());
    }

    out = std::move(parsed);
    return Unit{};
}

/// Narrow an int64 parameter onto one of Task's `int` fields.
///
/// A value that does not fit is refused instead of truncated: a truncated
/// importance would change the ranking with no visible error anywhere, which
/// is far harder to notice than a rejected call. The 1..5 / >= 0 domain rules
/// are deliberately NOT duplicated here — TaskStore::addTask documents itself
/// as the shared write-path validation, and two copies of a rule drift.
[[nodiscard]] Result<int> toIntField(std::int64_t value, const char *key)
{
    if (value < static_cast<std::int64_t>(std::numeric_limits<int>::min()) ||
        value > static_cast<std::int64_t>(std::numeric_limits<int>::max()))
    {
        return Error::invalidArgument(std::string{ "parameter '" } + key +
                                      "' is out of range for a 32-bit integer: " +
                                      std::to_string(value));
    }
    return static_cast<int>(value);
}

/// Read a JSON integer that has already passed is_number_integer() as an
/// int64, or fail when the value does not fit.
///
/// The check has to happen here, on the JSON variant, BEFORE any conversion:
/// on the installed nlohmann (3.11.3) the signed-integer conversion is an
/// unchecked static_cast of the stored std::uint64_t, so it does NOT throw for
/// a value above INT64_MAX. 18446744073709551615 (UINT64_MAX) silently comes
/// back as -1 and 9223372036854775808 as INT64_MIN. A try/catch around
/// get<std::int64_t>() therefore advertises an error it can never produce, and
/// worse, the wrapped value is a real one: due_at = -1 is 1969, so the task
/// would not be rejected at all — it would become the most urgent row in the
/// queue.
[[nodiscard]] Result<std::int64_t> readInt64(const nlohmann::json &value, const char *key)
{
    // is_number_integer() covers both the signed and the unsigned variant, so
    // the two are told apart by is_number_unsigned(). Only the unsigned one can
    // fail to fit: a signed value is already stored as an int64, and a float —
    // even an integral one — never reaches here because the call sites reject
    // it as a wrong type first.
    if (value.is_number_unsigned())
    {
        const std::uint64_t raw = value.get<std::uint64_t>();
        if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        {
            // The message carries the digits the caller sent, so the fix needs
            // no lookup of what the largest representable value would have been.
            return Error::invalidArgument(std::string{ "parameter '" } + key +
                                          "' is out of range: " + std::to_string(raw));
        }
        return static_cast<std::int64_t>(raw);
    }

    return value.get<std::int64_t>();
}

/// Translate an optional `limit` onto TaskFilter's size_t sentinel.
///
/// 0 means "no limit" in TaskFilter, so an explicit 0 and an absent key mean
/// the same thing here; a negative limit is a caller bug and is reported
/// rather than quietly turned into "no limit" (which would hand back the
/// entire backlog to a client that asked for nothing).
[[nodiscard]] Status readLimit(const std::optional<std::int64_t> &value, const char *key,
                               std::size_t &out)
{
    if (!value.has_value())
    {
        out = 0;
        return Unit{};
    }
    if (*value < 0)
    {
        return Error::invalidArgument(std::string{ "parameter '" } + key +
                                      "' must not be negative, got " + std::to_string(*value));
    }
    out = static_cast<std::size_t>(*value);
    return Unit{};
}

// --- JSON Schema builders -----------------------------------------------
// Every params_schema is built through these so required-ness is expressed the
// only way JSON Schema defines it — a schema-level "required" array. A
// per-property "required": true is not JSON Schema at all, and API validators
// (including the ones MCP hosts run over tools/list) reject it.

[[nodiscard]] nlohmann::json stringProp(std::string description)
{
    return nlohmann::json{ { "type", "string" }, { "description", std::move(description) } };
}

[[nodiscard]] nlohmann::json integerProp(std::string description,
                                         std::optional<int> minimum = std::nullopt,
                                         std::optional<int> maximum = std::nullopt)
{
    nlohmann::json prop{ { "type", "integer" }, { "description", std::move(description) } };
    if (minimum.has_value())
    {
        prop["minimum"] = *minimum;
    }
    if (maximum.has_value())
    {
        prop["maximum"] = *maximum;
    }
    return prop;
}

[[nodiscard]] nlohmann::json numberProp(std::string description)
{
    // "number", not "integer": weights are doubles (0.5 of a day of aging is a
    // legitimate tuning), and a schema that says integer would make a strict
    // client-side validator reject a fractional weight before it ever arrived.
    return nlohmann::json{ { "type", "number" }, { "description", std::move(description) } };
}

[[nodiscard]] nlohmann::json booleanProp(std::string description)
{
    return nlohmann::json{ { "type", "boolean" }, { "description", std::move(description) } };
}

[[nodiscard]] nlohmann::json stringArrayProp(std::string description)
{
    return nlohmann::json{
        { "type", "array" },
        { "description", std::move(description) },
        { "items", nlohmann::json{ { "type", "string" } } },
    };
}

/// Assemble an object schema. `required` is omitted when empty rather than
/// emitted as [], so a schema with no required parameters stays minimal.
[[nodiscard]] nlohmann::json objectSchema(nlohmann::json properties,
                                          std::vector<std::string> required)
{
    nlohmann::json schema{
        { "type", "object" },
        { "properties", std::move(properties) },
    };
    if (!required.empty())
    {
        schema["required"] = std::move(required);
    }
    return schema;
}

} // namespace

int rpcErrorCode(ErrorCode code)
{
    // -32000..-32099 is the range JSON-RPC 2.0 reserves for application
    // errors, and that is where the domain failures belong: an unknown task id
    // or a name that is already taken is not a transport fault, and a client
    // must be able to tell "your request was malformed" (-32602, standard)
    // apart from "your request was well-formed but the domain said no".
    switch (code)
    {
        case ErrorCode::kInvalidArgument:
            return -32602; // standard "Invalid params"
        case ErrorCode::kNotFound:
            return -32001;
        case ErrorCode::kConflict:
            return -32002;
        case ErrorCode::kStorageFailure:
            return -32003;
        case ErrorCode::kInternal:
            return -32603; // standard "Internal error"
    }
    // Unreachable for a valid enumerator. kInternal is the safe landing spot:
    // it tells the client the fault is ours and carries no false claim that
    // the caller's request was wrong.
    return -32603;
}

Services::Services(TaskStore &store, const Clock &clock, std::string version)
    : m_store{ store },
      m_clock{ clock },
      m_version{ std::move(version) },
      // Sampled once here so uptime is measured from the instant the service
      // was built, not from the first call that happened to need the time.
      m_startedAt{ clock.nowEpochSeconds() }
{
}

const std::vector<MethodSpec> &Services::methodSpecs()
{
    // A function-local static: the catalog is pure data, so it can be built
    // without an instance (the `mcp` subcommand publishes the tool list with
    // no store and no daemon), and C++ guarantees the initialization is
    // thread-safe, so concurrent readers need no lock of their own.
    static const std::vector<MethodSpec> kSpecs = [] {
        std::vector<MethodSpec> specs;
        specs.reserve(15);

        // --- introspection ---
        specs.push_back(MethodSpec{
            "get_status",
            "Report the daemon: version, uptime, database path, method count and the "
            "backlog tallies (open, in_progress, done, archived, overdue, "
            "due_within_24h, and tombstones, the records of deletions that a merge "
            "from another machine must not undo). Call it to check that the daemon is "
            "alive and to see how much work is outstanding before deciding what to do.",
            objectSchema(nlohmann::json::object(), {}) });

        specs.push_back(MethodSpec{
            "describe_methods",
            "List every operation taskPilot exposes, each with a description and the "
            "JSON Schema of its parameters. Use it when you need the exact parameter "
            "names for a call, or to discover what else is available.",
            objectSchema(nlohmann::json::object(), {}) });

        // --- task mutations ---
        specs.push_back(MethodSpec{
            "add_task",
            "Create a task and return it with its assigned id. Use this as soon as a "
            "piece of work is identified: give it a deadline (due_at, in epoch seconds) "
            "and a blocks count if other work is waiting on it, because those two "
            "fields drive the ranking more than importance does.",
            objectSchema(
                nlohmann::json{
                    { "title", stringProp("Short description of the work. Required, and "
                                          "must not be blank.") },
                    { "importance",
                      integerProp("How much this matters, 1 (trivia) to 5 (critical). "
                                  "Defaults to 3.",
                                  1, 5) },
                    { "due_at",
                      integerProp("Deadline as epoch seconds (UTC). Omit it for work with "
                                  "no deadline: an undated task scores zero urgency, it is "
                                  "not treated as urgent.") },
                    { "blocks",
                      integerProp("How many other tasks or people are waiting on this "
                                  "one. Defaults to 0.",
                                  0, std::nullopt) },
                    { "tags", stringArrayProp("Free-form labels used by list_tasks to "
                                              "filter.") },
                    { "notes",
                      stringProp("Longer detail, free-form. May be empty or omitted.") },
                },
                { "title" }) });

        specs.push_back(MethodSpec{
            "update_task",
            "Change any subset of an existing task's fields and return the task as it "
            "now stands; fields you do not send keep their current value. Use "
            "clear_due_at to remove a deadline (due_at sets one). To finish work use "
            "complete_task and to revive it use reopen_task.",
            objectSchema(
                nlohmann::json{
                    { "id", integerProp("Id of the task to update. Required.") },
                    { "title", stringProp("New title.") },
                    { "notes", stringProp("New notes; an empty string clears them.") },
                    { "importance", integerProp("New importance, 1 to 5.", 1, 5) },
                    { "due_at",
                      integerProp("New deadline as epoch seconds (UTC).") },
                    { "clear_due_at",
                      booleanProp("True removes the deadline. Do not combine it with "
                                  "due_at.") },
                    { "blocks", integerProp("New count of downstream dependents.", 0,
                                            std::nullopt) },
                    { "tags", stringArrayProp("Replacement tag list; it replaces the "
                                              "existing tags rather than appending.") },
                    { "status",
                      stringProp("New lifecycle state: open, in_progress, done or "
                                 "archived. Prefer complete_task / reopen_task for the "
                                 "common transitions.") },
                },
                { "id" }) });

        specs.push_back(MethodSpec{
            "complete_task",
            "Mark a task as done and return it. Use it the moment work finishes, so the "
            "task stops competing for attention in the queue (the record is kept for "
            "history and stats).",
            objectSchema(nlohmann::json{
                             { "id", integerProp("Id of the task to complete. Required.") },
                         },
                         { "id" }) });

        specs.push_back(MethodSpec{
            "reopen_task",
            "Put a completed or archived task back into the queue as open and return it. "
            "Use it when work was completed by mistake or has to be redone.",
            objectSchema(nlohmann::json{
                             { "id", integerProp("Id of the task to reopen. Required.") },
                         },
                         { "id" }) });

        specs.push_back(MethodSpec{
            "delete_task",
            "Permanently delete a task and return {\"deleted\": true, \"id\": N}. This "
            "cannot be undone and the history is gone, so prefer complete_task or "
            "archived status unless the task was created by mistake.",
            objectSchema(nlohmann::json{
                             { "id", integerProp("Id of the task to delete. Required.") },
                         },
                         { "id" }) });

        // --- queries ---
        specs.push_back(MethodSpec{
            "get_queue",
            "The ranked queue: what to work on next, best first, each entry carrying its "
            "total score and the per-term breakdown that produced it. Call this when "
            "the user asks what to do next. Done and archived tasks are never included.",
            objectSchema(
                nlohmann::json{
                    { "limit",
                      integerProp("Return at most this many entries. Omit or pass 0 for "
                                  "the whole queue.",
                                  0, std::nullopt) },
                    { "include_in_progress",
                      booleanProp("Whether tasks already in progress appear in the queue. "
                                  "Defaults to true; pass false to see only not-yet-"
                                  "started work.") },
                },
                {}) });

        specs.push_back(MethodSpec{
            "list_tasks",
            "List tasks for browsing or filtering, optionally by status or tag, and in "
            "score, creation or due-date order. Use get_queue instead when the question "
            "is what to do next.",
            objectSchema(
                nlohmann::json{
                    { "status",
                      stringProp("Only tasks in this state: open, in_progress, done or "
                                 "archived.") },
                    { "tag", stringProp("Only tasks carrying this exact tag.") },
                    { "limit",
                      integerProp("Return at most this many tasks. Omit or pass 0 for all "
                                  "of them.",
                                  0, std::nullopt) },
                    { "order",
                      stringProp("Ordering: \"score\" (default, the same order as "
                                 "get_queue), \"created\" (oldest first) or \"due\" "
                                 "(soonest deadline first, undated tasks last).") },
                },
                {}) });

        specs.push_back(MethodSpec{
            "get_task",
            "Fetch a single task by id together with its current score and the breakdown "
            "of that score. Use it to explain why a task sits where it does, or to read "
            "a task's notes in full.",
            objectSchema(nlohmann::json{
                             { "id", integerProp("Id of the task to fetch. Required.") },
                         },
                         { "id" }) });

        specs.push_back(MethodSpec{
            "get_stats",
            "Backlog tallies: counts by state, how many tasks are overdue and how many "
            "fall due in the next 24 hours, plus the title and score at the top of the "
            "queue. Finding that top entry ranks the whole actionable backlog — a "
            "bounded scan could only report the head of a subset — so use get_queue "
            "when you want the ranked list itself.",
            objectSchema(nlohmann::json::object(), {}) });

        // --- synchronisation ---
        //
        // The backlog is shared between machines through a file that travels in
        // git, so these are its two ends: export produces the file this machine
        // would commit, import folds in the file another machine committed.
        // They sit here rather than among the task mutations because one only
        // reads, and because the other is the sole call in the API that can
        // destroy local work — which is why only its destructive form is opt-in.

        specs.push_back(MethodSpec{
            "export_tasks",
            "Return this machine's entire backlog as one JSONL document: every task "
            "and every record of a deletion, one JSON record per line, sorted by uid, "
            "plus its record count and format version. Use it to produce the "
            "synchronisation file, which belongs in the PRIVATE data repository and "
            "never in this public project one, and to see what this machine would "
            "contribute to a merge. The whole backlog fits in the reply, so there is "
            "nothing to page.",
            objectSchema(nlohmann::json::object(), {}) });

        specs.push_back(MethodSpec{
            "import_tasks",
            "Merge a backlog produced by export_tasks on another machine into this "
            "one and report what changed: how many records were inserted, updated, "
            "deleted, resurrected and skipped, with example titles. Records are "
            "matched by uid, not by id, and the newer record wins, so an older copy "
            "elsewhere cannot undo a deletion made here. IT IS A DRY RUN UNLESS YOU "
            "ASK OTHERWISE: dry_run defaults to true and nothing is written unless "
            "you pass dry_run false. Call it once to preview, read the report, then "
            "call it again with dry_run false to apply it.",
            objectSchema(
                nlohmann::json{
                    { "jsonl",
                      stringProp("The whole backlog to merge, one JSON record per line, "
                                 "exactly as export_tasks returned it. Required.") },
                    { "dry_run",
                      booleanProp("Whether to report the merge without performing it. "
                                  "Defaults to true, so omit it to preview and pass "
                                  "false to apply.") },
                },
                { "jsonl" }) });

        // --- settings ---
        specs.push_back(MethodSpec{
            "get_weights",
            "Read the ranking weights and the urgency horizon. Use it to see how the "
            "queue is currently tuned before changing it.",
            objectSchema(nlohmann::json::object(), {}) });

        specs.push_back(MethodSpec{
            "set_weights",
            "Retune the ranking by changing any subset of the weights; the fields you "
            "omit keep their current values and the result is validated before it is "
            "stored. Use it when the queue does not reflect what the user cares about "
            "— for example raising urgency to make deadlines dominate.",
            objectSchema(
                nlohmann::json{
                    { "importance", numberProp("Weight of the importance term.") },
                    { "urgency", numberProp("Weight of the urgency (deadline) term.") },
                    { "age", numberProp("Weight of the aging (anti-starvation) term.") },
                    { "blocks",
                      numberProp("Weight of the blocks (downstream dependents) term.") },
                    { "urgency_horizon_days",
                      numberProp("Days over which urgency ramps from 0 to 1. Must be "
                                 "greater than 0.") },
                },
                {}) });

        return specs;
    }();

    return kSpecs;
}

const MethodSpec *Services::findSpec(const std::string &name)
{
    const std::vector<MethodSpec> &specs = methodSpecs();
    const auto it = std::find_if(specs.begin(), specs.end(),
                                 [&name](const MethodSpec &spec) { return spec.name == name; });
    if (it == specs.end())
    {
        return nullptr;
    }
    return &*it;
}

// --- parameter readers --------------------------------------------------
//
// Precondition for all three: `params` is a JSON object. invoke() is the only
// caller and it rejects anything else up front, so a non-object can never
// reach them and a defensive branch here would be dead code.

Result<std::string> Services::requiredString(const nlohmann::json &params, const char *key)
{
    const auto it = params.find(key);
    if (it == params.end())
    {
        return Error::invalidArgument(std::string{ "missing required parameter: " } + key);
    }
    if (!it->is_string())
    {
        return Error::invalidArgument(std::string{ "parameter '" } + key +
                                      "' must be a string, got " + it->type_name());
    }
    return it->get<std::string>();
}

Result<std::int64_t> Services::requiredInt(const nlohmann::json &params, const char *key)
{
    const auto it = params.find(key);
    if (it == params.end())
    {
        return Error::invalidArgument(std::string{ "missing required parameter: " } + key);
    }
    if (!it->is_number_integer())
    {
        return Error::invalidArgument(std::string{ "parameter '" } + key +
                                      "' must be an integer, got " + it->type_name());
    }

    // Converted by a checked reader rather than by a try/catch around
    // get<std::int64_t>(): the conversion does not throw for an integer that
    // does not fit, it wraps (see readInt64), so a catch block here would be a
    // handler that can never run.
    return readInt64(*it, key);
}

Status Services::optionalInt(const nlohmann::json &params, const char *key,
                             std::optional<std::int64_t> &out)
{
    const auto it = params.find(key);
    if (it == params.end())
    {
        // Left untouched: the caller's initial value is the fallback, which
        // lets a three-state field (absent / value / clear) be expressed by
        // the caller's own nullopt.
        return Unit{};
    }
    if (!it->is_number_integer())
    {
        return Error::invalidArgument(std::string{ "parameter '" } + key +
                                      "' must be an integer, got " + it->type_name());
    }
    const Result<std::int64_t> read = readInt64(*it, key);
    if (!read.ok())
    {
        // `out` keeps the caller's fallback: a rejected value must not leave a
        // half-written parameter behind, or the handler would carry on with a
        // value the caller never sent.
        return read.error();
    }
    out = read.value();
    return Unit{};
}

// --- introspection ------------------------------------------------------

RpcResult Services::handleGetStatus(const nlohmann::json &params)
{
    const std::int64_t now = sampledNow(params);

    // `now` comes from the same sampled instant the ranking uses, so the
    // overdue and due_within_24h tallies cannot disagree with get_queue about
    // what "today" means.
    const Result<Stats> stats = m_store.stats(now);
    if (!stats.ok())
    {
        return stats.error();
    }

    // The store's tallies are merged at the top level rather than nested, and
    // they are written FIRST so that the identity fields below can never be
    // shadowed by a future stats key of the same name.
    nlohmann::json result = toJson(stats.value());

    // Tombstones are reported beside the task tallies because the two are read
    // together: the task counts say what is here, and the tombstone count says
    // how much was deliberately removed and must stay removed. It is also the
    // cheapest sanity signal that a merge has not rewritten history — a
    // tombstone count only ever grows (see TaskStore::tombstoneCount).
    const Result<std::size_t> tombstones = m_store.tombstoneCount();
    if (!tombstones.ok())
    {
        return tombstones.error();
    }
    result["tombstones"] = tombstones.value();

    result["version"] = m_version;
    result["uptime_seconds"] = now > m_startedAt ? now - m_startedAt : 0;
    result["db_path"] = m_store.path();
    result["method_count"] = static_cast<int>(methodSpecs().size());
    result["now"] = now;
    return result;
}

RpcResult Services::handleDescribeMethods(const nlohmann::json &params)
{
    static_cast<void>(params); // Takes no parameters.

    nlohmann::json catalog = nlohmann::json::array();
    for (const MethodSpec &spec : methodSpecs())
    {
        catalog.push_back(nlohmann::json{
            { "name", spec.name },
            { "description", spec.description },
            { "params_schema", spec.params_schema },
        });
    }
    return catalog;
}

// --- task mutations -----------------------------------------------------

RpcResult Services::handleAddTask(const nlohmann::json &params)
{
    const Result<std::string> title = requiredString(params, "title");
    if (!title.ok())
    {
        return title.error();
    }

    Task draft;
    // id, created_at and updated_at are deliberately left at their defaults:
    // the store assigns them, so a client cannot forge history.
    draft.title = title.value();
    draft.status = TaskStatus::kOpen;

    const Status notes = optionalString(params, "notes", draft.notes);
    if (!notes.ok())
    {
        return notes.error();
    }
    const Status tags = optionalStringArray(params, "tags", draft.tags);
    if (!tags.ok())
    {
        return tags.error();
    }

    std::optional<std::int64_t> importance;
    if (const Status read = optionalInt(params, "importance", importance); !read.ok())
    {
        return read.error();
    }
    if (importance.has_value())
    {
        const Result<int> narrowed = toIntField(*importance, "importance");
        if (!narrowed.ok())
        {
            return narrowed.error();
        }
        draft.importance = narrowed.value();
    }

    std::optional<std::int64_t> blocks;
    if (const Status read = optionalInt(params, "blocks", blocks); !read.ok())
    {
        return read.error();
    }
    if (blocks.has_value())
    {
        const Result<int> narrowed = toIntField(*blocks, "blocks");
        if (!narrowed.ok())
        {
            return narrowed.error();
        }
        draft.blocks = narrowed.value();
    }

    std::optional<std::int64_t> dueAt;
    if (const Status read = optionalInt(params, "due_at", dueAt); !read.ok())
    {
        return read.error();
    }
    draft.due_at = dueAt;

    // Domain validation (non-blank title, importance 1..5, blocks >= 0,
    // non-blank tags) belongs to the store, which documents itself as the one
    // gate every write path goes through; repeating it here would create a
    // second copy of the rules to keep in sync.
    Result<Task> created = m_store.addTask(std::move(draft));
    if (!created.ok())
    {
        return created.error();
    }
    return toJson(created.value());
}

RpcResult Services::handleUpdateTask(const nlohmann::json &params)
{
    const Result<std::int64_t> id = requiredInt(params, "id");
    if (!id.ok())
    {
        return id.error();
    }

    TaskPatch patch;

    if (params.contains("title"))
    {
        std::string value;
        if (const Status read = optionalString(params, "title", value); !read.ok())
        {
            return read.error();
        }
        patch.title = std::move(value);
    }

    if (params.contains("notes"))
    {
        std::string value;
        if (const Status read = optionalString(params, "notes", value); !read.ok())
        {
            return read.error();
        }
        patch.notes = std::move(value);
    }

    if (params.contains("importance"))
    {
        std::optional<std::int64_t> value;
        if (const Status read = optionalInt(params, "importance", value); !read.ok())
        {
            return read.error();
        }
        if (value.has_value())
        {
            const Result<int> narrowed = toIntField(*value, "importance");
            if (!narrowed.ok())
            {
                return narrowed.error();
            }
            patch.importance = narrowed.value();
        }
    }

    if (params.contains("blocks"))
    {
        std::optional<std::int64_t> value;
        if (const Status read = optionalInt(params, "blocks", value); !read.ok())
        {
            return read.error();
        }
        if (value.has_value())
        {
            const Result<int> narrowed = toIntField(*value, "blocks");
            if (!narrowed.ok())
            {
                return narrowed.error();
            }
            patch.blocks = narrowed.value();
        }
    }

    if (params.contains("tags"))
    {
        std::vector<std::string> value;
        if (const Status read = optionalStringArray(params, "tags", value); !read.ok())
        {
            return read.error();
        }
        patch.tags = std::move(value);
    }

    if (params.contains("status"))
    {
        std::string value;
        if (const Status read = optionalString(params, "status", value); !read.ok())
        {
            return read.error();
        }
        const std::optional<TaskStatus> parsed = taskStatusFromString(value);
        if (!parsed.has_value())
        {
            // An unrecognised status name is named back to the caller with the
            // accepted vocabulary, so the fix needs no documentation lookup.
            return Error::invalidArgument("parameter 'status' must be one of: "
                                          "open, in_progress, done, archived; got '" +
                                          value + "'");
        }
        patch.status = *parsed;
    }

    bool clearDueAt = false;
    if (const Status read = optionalBool(params, "clear_due_at", clearDueAt); !read.ok())
    {
        return read.error();
    }
    // due_at (set a value) and clear_due_at (remove it) together are always a
    // caller bug, and silently letting one win would hide it. clear_due_at
    // false is not a conflict: it asks for nothing to change.
    if (params.contains("due_at") && clearDueAt)
    {
        return Error::invalidArgument(
            "parameters 'due_at' and 'clear_due_at' cannot both be set");
    }
    patch.clear_due_at = clearDueAt;
    if (params.contains("due_at"))
    {
        std::optional<std::int64_t> value;
        if (const Status read = optionalInt(params, "due_at", value); !read.ok())
        {
            return read.error();
        }
        patch.due_at = value;
    }

    Result<Task> updated = m_store.updateTask(id.value(), patch);
    if (!updated.ok())
    {
        return updated.error();
    }
    return toJson(updated.value());
}

RpcResult Services::handleCompleteTask(const nlohmann::json &params)
{
    const Result<std::int64_t> id = requiredInt(params, "id");
    if (!id.ok())
    {
        return id.error();
    }

    // The transition goes through the store's patch path rather than a
    // dedicated statement, so completed_at is set (and cleared on reopen) by
    // the one piece of code that owns that rule.
    TaskPatch patch;
    patch.status = TaskStatus::kDone;

    Result<Task> completed = m_store.updateTask(id.value(), patch);
    if (!completed.ok())
    {
        return completed.error();
    }
    return toJson(completed.value());
}

RpcResult Services::handleReopenTask(const nlohmann::json &params)
{
    const Result<std::int64_t> id = requiredInt(params, "id");
    if (!id.ok())
    {
        return id.error();
    }

    TaskPatch patch;
    patch.status = TaskStatus::kOpen;

    Result<Task> reopened = m_store.updateTask(id.value(), patch);
    if (!reopened.ok())
    {
        return reopened.error();
    }
    return toJson(reopened.value());
}

RpcResult Services::handleDeleteTask(const nlohmann::json &params)
{
    const Result<std::int64_t> id = requiredInt(params, "id");
    if (!id.ok())
    {
        return id.error();
    }

    // The store reports a missing row as kNotFound; that is propagated rather
    // than flattened into {"deleted": true}, so a double delete is visible to
    // the caller instead of looking like a success.
    const Status removed = m_store.deleteTask(id.value());
    if (!removed.ok())
    {
        return removed.error();
    }
    return nlohmann::json{ { "deleted", true }, { "id", id.value() } };
}

// --- queries ------------------------------------------------------------

RpcResult Services::handleGetQueue(const nlohmann::json &params)
{
    const std::int64_t now = sampledNow(params);

    std::optional<std::int64_t> limit;
    if (const Status read = optionalInt(params, "limit", limit); !read.ok())
    {
        return read.error();
    }
    std::size_t rowLimit = 0;
    if (const Status read = readLimit(limit, "limit", rowLimit); !read.ok())
    {
        return read.error();
    }

    // Default true: "the queue" means everything actionable, and a caller who
    // only wants not-yet-started work says so explicitly.
    bool includeInProgress = true;
    if (const Status read = optionalBool(params, "include_in_progress", includeInProgress);
        !read.ok())
    {
        return read.error();
    }

    const Result<Weights> weights = m_store.weights();
    if (!weights.ok())
    {
        return weights.error();
    }

    // The store returns an unordered set and ranks nothing, so its own limit
    // is left at 0: trimming there would cut an arbitrary subset before the
    // ranker ever saw it, which is not a "top N" of anything.
    const Result<std::vector<Task>> tasks = m_store.listTasks(TaskFilter{});
    if (!tasks.ok())
    {
        return tasks.error();
    }

    std::vector<Task> candidates = tasks.value();
    if (!includeInProgress)
    {
        std::erase_if(candidates, [](const Task &candidate) {
            return TaskStatus::kInProgress == candidate.status;
        });
    }

    // rank() drops anything not actionable, so done and archived tasks can
    // never appear here even though the filter asked for every status.
    std::vector<RankedTask> ranked = PriorityEngine::rank(candidates, now, weights.value());
    if (0 != rowLimit && ranked.size() > rowLimit)
    {
        ranked.resize(rowLimit);
    }

    nlohmann::json queue = nlohmann::json::array();
    for (const RankedTask &entry : ranked)
    {
        // toJson(RankedTask) flattens the task's fields and adds `score` plus
        // `score_parts`, which is what makes the ordering explainable to a
        // caller rather than a bare sequence it has to trust.
        queue.push_back(toJson(entry));
    }

    return nlohmann::json{
        { "now", now },
        { "weights", toJson(weights.value()) },
        { "queue", std::move(queue) },
    };
}

RpcResult Services::handleListTasks(const nlohmann::json &params)
{
    const std::int64_t now = sampledNow(params);

    TaskFilter filter;

    if (params.contains("status"))
    {
        std::string value;
        if (const Status read = optionalString(params, "status", value); !read.ok())
        {
            return read.error();
        }
        const std::optional<TaskStatus> parsed = taskStatusFromString(value);
        if (!parsed.has_value())
        {
            return Error::invalidArgument("parameter 'status' must be one of: "
                                          "open, in_progress, done, archived; got '" +
                                          value + "'");
        }
        filter.status = *parsed;
    }

    if (params.contains("tag"))
    {
        std::string value;
        if (const Status read = optionalString(params, "tag", value); !read.ok())
        {
            return read.error();
        }
        filter.tag = std::move(value);
    }

    std::optional<std::int64_t> limit;
    if (const Status read = optionalInt(params, "limit", limit); !read.ok())
    {
        return read.error();
    }
    std::size_t rowLimit = 0;
    if (const Status read = readLimit(limit, "limit", rowLimit); !read.ok())
    {
        return read.error();
    }

    // The initial value is the fallback: an absent `order` leaves the default
    // in place, while a present-but-unknown one is rejected below.
    std::string order = "score";
    if (const Status read = optionalString(params, "order", order); !read.ok())
    {
        return read.error();
    }
    if ("score" != order && "created" != order && "due" != order)
    {
        return Error::invalidArgument(
            "parameter 'order' must be one of: score, created, due; got '" + order + "'");
    }

    // Layering: the store returns an unordered set by design, so ordering is
    // applied here, in the layer that has the engine and the clock — and so is
    // the limit, for the same reason. A limit pushed into the storage query
    // would cut an arbitrary subset of that unordered set before the ordering
    // ran, which is not a "first N" of anything the caller asked for.
    const Result<std::vector<Task>> tasks = m_store.listTasks(filter);
    if (!tasks.ok())
    {
        return tasks.error();
    }
    std::vector<Task> ordered = tasks.value();

    if ("score" == order)
    {
        const Result<Weights> weights = m_store.weights();
        if (!weights.ok())
        {
            return weights.error();
        }
        PriorityEngine::sortByScore(ordered, now, weights.value());
    }
    else if ("created" == order)
    {
        // Ascending: "in creation order" means the oldest first. Every
        // comparator here breaks ties on id, so the output never depends on
        // the sort implementation or on the order rows came out of SQLite.
        std::sort(ordered.begin(), ordered.end(), [](const Task &lhs, const Task &rhs) {
            if (lhs.created_at != rhs.created_at)
            {
                return lhs.created_at < rhs.created_at;
            }
            return lhs.id < rhs.id;
        });
    }
    else
    {
        // Deadlines first, undated tasks last: a task with no deadline has no
        // position in a deadline ordering, and treating it as "due at the
        // epoch" would put the entire undated backlog in front of real dates.
        std::sort(ordered.begin(), ordered.end(), [](const Task &lhs, const Task &rhs) {
            if (lhs.due_at.has_value() != rhs.due_at.has_value())
            {
                return lhs.due_at.has_value();
            }
            if (lhs.due_at.has_value() && *lhs.due_at != *rhs.due_at)
            {
                return *lhs.due_at < *rhs.due_at;
            }
            return lhs.id < rhs.id;
        });
    }

    if (0 != rowLimit && ordered.size() > rowLimit)
    {
        ordered.resize(rowLimit);
    }

    nlohmann::json list = nlohmann::json::array();
    for (const Task &task : ordered)
    {
        list.push_back(toJson(task));
    }
    return list;
}

RpcResult Services::handleGetTask(const nlohmann::json &params)
{
    const Result<std::int64_t> id = requiredInt(params, "id");
    if (!id.ok())
    {
        return id.error();
    }

    const std::int64_t now = sampledNow(params);

    const Result<Task> task = m_store.getTask(id.value());
    if (!task.ok())
    {
        return task.error();
    }
    const Result<Weights> weights = m_store.weights();
    if (!weights.ok())
    {
        return weights.error();
    }

    // The score is reported for every status, not only the actionable ones:
    // the queue filters, the math does not, and a caller asking about one
    // specific task wants the number it would score, not a zero it cannot
    // explain. Both derived inputs are filled in so the breakdown stays
    // reproducible by hand.
    RankedTask ranked;
    ranked.task = task.value();
    ranked.score = PriorityEngine::score(ranked.task, now, weights.value());
    ranked.urgency_factor = PriorityEngine::urgencyFactor(
        ranked.task.due_at, now, weights.value().urgency_horizon_days);
    ranked.age_days = PriorityEngine::ageDays(ranked.task.created_at, now);
    return toJson(ranked);
}

RpcResult Services::handleGetStats(const nlohmann::json &params)
{
    const std::int64_t now = sampledNow(params);

    // Both reads are measured against the one instant sampled above; the
    // tallies below are only interpretable next to the `now` returned with
    // them, because overdue and due_within_24h are relative to it.
    const Result<Stats> stats = m_store.stats(now);
    if (!stats.ok())
    {
        return stats.error();
    }
    const Result<Weights> weights = m_store.weights();
    if (!weights.ok())
    {
        return weights.error();
    }

    // The whole backlog is loaded and ranked, and there is deliberately no
    // `limit` parameter to bound that: the storage layer returns an unordered
    // set, so a bound applied there would cut an arbitrary subset, and the top
    // of a truncated set is not the top of the queue. Resizing an already
    // ranked list changes nothing about what front() reports, so a `limit` here
    // would be a knob that cannot turn anything — it is gone rather than
    // advertised as a way to make this call cheap.
    const Result<std::vector<Task>> tasks = m_store.listTasks(TaskFilter{});
    if (!tasks.ok())
    {
        return tasks.error();
    }

    const std::vector<RankedTask> ranked =
        PriorityEngine::rank(tasks.value(), now, weights.value());

    nlohmann::json result = toJson(stats.value());
    if (ranked.empty())
    {
        // null rather than a missing key: a stable schema is easier for a
        // client (and for an LLM writing code against it) than a key that is
        // sometimes absent.
        result["top_title"] = nullptr;
        result["top_score"] = nullptr;
    }
    else
    {
        result["top_title"] = ranked.front().task.title;
        result["top_score"] = ranked.front().score.total;
    }
    result["now"] = now;
    return result;
}

// --- synchronisation ----------------------------------------------------

RpcResult Services::handleExportTasks(const nlohmann::json &params)
{
    static_cast<void>(params); // Takes no parameters.

    const Result<std::string> jsonl = m_store.exportJsonl();
    if (!jsonl.ok())
    {
        return jsonl.error();
    }

    // The count is taken from the parser the importer uses rather than from a
    // line count of our own, so the file the caller commits and the merge that
    // will later read it agree by construction about how many records it
    // holds. It also means a line our own parser would reject fails the export
    // here, instead of being committed on this machine as a file the other one
    // cannot read.
    const Result<std::vector<SyncRecord>> records = parseJsonl(jsonl.value());
    if (!records.ok())
    {
        return records.error();
    }

    // One reply carries the whole backlog: at this scale (a personal backlog,
    // thousands of lines at most) that is no burden, and it is the point — the
    // document is one file with one line per uid, so paging it would hand back
    // something the caller still had to reassemble before committing it.
    // `jsonl` IS that file's contents, which is what makes this the endpoint a
    // client calls to produce the file it commits to the data repository.
    return nlohmann::json{
        { "format", kExportFormatVersion },
        { "count", records.value().size() },
        { "jsonl", jsonl.value() },
    };
}

RpcResult Services::handleImportTasks(const nlohmann::json &params)
{
    const Result<std::string> jsonl = requiredString(params, "jsonl");
    if (!jsonl.ok())
    {
        return jsonl.error();
    }

    // A merge can overwrite and remove local work, so it is the one operation
    // here whose destructive form has to be ASKED FOR: the default is the dry
    // run, and a caller that forgets the flag previews rather than applies.
    // The catalog description says the same thing, because the description is
    // what an LLM reads when it chooses how to call this.
    //
    // An explicit null means "no opinion" rather than "false" — the same
    // normalisation the MCP transport applies to a null `arguments` — so the
    // conservative default survives every way of leaving the flag out.
    bool dryRun = true;
    if (params.contains("dry_run") && !params.at("dry_run").is_null())
    {
        if (const Status read = optionalBool(params, "dry_run", dryRun); !read.ok())
        {
            return read.error();
        }
    }

    const Result<MergeReport> report = m_store.mergeJsonl(jsonl.value(), dryRun);
    if (!report.ok())
    {
        return report.error();
    }

    // The counts are identical whether or not anything was written, so the
    // reply states which of the two happened: without this key a caller that
    // omitted dry_run could not tell a preview from an applied merge, and
    // would be left believing that a merge it never asked for had run.
    nlohmann::json result = toJson(report.value());
    // The report is an object by contract, and the check is what keeps a break
    // in that contract a described failure: indexing a non-object throws, and
    // an exception from a handler would reach the client as a protocol fault
    // with no hint that the report was the thing that was malformed.
    if (!result.is_object())
    {
        return Error::internal("the merge report did not serialize as an object");
    }
    result["dry_run"] = dryRun;
    return result;
}

// --- settings -----------------------------------------------------------

RpcResult Services::handleGetWeights(const nlohmann::json &params)
{
    static_cast<void>(params); // Takes no parameters.

    const Result<Weights> weights = m_store.weights();
    if (!weights.ok())
    {
        return weights.error();
    }
    return toJson(weights.value());
}

RpcResult Services::handleSetWeights(const nlohmann::json &params)
{
    // The merge is a single locked read-modify-write inside the store. Doing it
    // here — read the weights, merge keys, write them back — releases the store
    // lock between the read and the write, so two concurrent partial updates
    // can each start from the same snapshot and the later one silently discards
    // the earlier caller's key. See TaskStore::patchWeights.
    //
    // Everything the caller did not name keeps its stored value, and the merged
    // result is validated (a number, finite, >= 0, horizon > 0) before it is
    // written, so an invalid value is refused without half-applying. The
    // validation lives in weightsFromJson, which is the same code path a
    // full-document store write goes through — one set of rules, not two.
    const Result<Weights> patched = m_store.patchWeights(params);
    if (!patched.ok())
    {
        return patched.error();
    }
    return toJson(patched.value());
}

// --- dispatch -----------------------------------------------------------

RpcResult Services::invoke(const std::string &method, const nlohmann::json &params)
{
    using MemberHandler = RpcResult (Services::*)(const nlohmann::json &);

    // One row per method in the catalog above. The table lives inside this
    // member function because only a member may name the private handlers;
    // being function-local static, it is initialized once, thread-safely, and
    // is read-only afterwards.
    static const std::unordered_map<std::string, MemberHandler> kDispatch{
        { "get_status", &Services::handleGetStatus },
        { "describe_methods", &Services::handleDescribeMethods },
        { "add_task", &Services::handleAddTask },
        { "update_task", &Services::handleUpdateTask },
        { "complete_task", &Services::handleCompleteTask },
        { "reopen_task", &Services::handleReopenTask },
        { "delete_task", &Services::handleDeleteTask },
        { "get_queue", &Services::handleGetQueue },
        { "list_tasks", &Services::handleListTasks },
        { "get_task", &Services::handleGetTask },
        { "get_stats", &Services::handleGetStats },
        { "export_tasks", &Services::handleExportTasks },
        { "import_tasks", &Services::handleImportTasks },
        { "get_weights", &Services::handleGetWeights },
        { "set_weights", &Services::handleSetWeights },
    };

    const auto handler = kDispatch.find(method);
    if (kDispatch.end() == handler)
    {
        // Checked before the params shape so that a mistyped method name is
        // always reported as such: an unknown method is the more useful of the
        // two facts, and the message carries the name so the caller (often an
        // LLM) can correct itself without guessing.
        return Error::invalidArgument("unknown method: " + method);
    }

    if (!params.is_object() && !params.is_null())
    {
        return Error::invalidArgument(std::string{ "params must be a JSON object, got " } +
                                      params.type_name());
    }

    // A JSON-RPC request that omits "params" arrives as null; normalizing it
    // here lets every handler assume an object and keeps that assumption in
    // one place. The copy is also what makes the stamping below safe on a
    // concurrently-used instance: nothing shared is written.
    nlohmann::json stamped = params.is_null() ? nlohmann::json::object() : params;

    // Sampled ONCE per call, before any handler runs, and handed down with the
    // params of this call. Sampling inside each task's scoring instead would
    // let a slow query rank two tasks against two different instants, which is
    // exactly the non-reproducible ordering PriorityEngine's total order
    // exists to prevent.
    stamped[kNowParamKey] = m_clock.nowEpochSeconds();

    return (this->*(handler->second))(stamped);
}

MethodRegistry Services::registry()
{
    MethodRegistry registry;
    registry.reserve(methodSpecs().size());
    for (const MethodSpec &spec : methodSpecs())
    {
        // Every entry routes back through invoke(). That keeps one code path
        // for both transports — a method cannot behave differently depending
        // on which frontend called it — and it means the catalog/dispatch
        // cross-check in the unit tests covers the registry too.
        const std::string name = spec.name;
        registry.emplace(name, [this, name](const nlohmann::json &params) {
            return invoke(name, params);
        });
    }
    return registry;
}

} // namespace taskpilot
