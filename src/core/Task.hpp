#pragma once
// Task.hpp — the unit of work taskPilot ranks
//
// A Task is a pure-data record. It owns no behaviour: ranking lives in
// PriorityEngine, persistence in TaskStore, and serialization here + in
// Task.cpp. That split is deliberate — it lets the scoring rules be tested
// without a database and the storage layer be tested without scoring.
//
// Time is always epoch SECONDS (not milliseconds, not time_t), stored as
// int64 so the same value round-trips through SQLite, JSON, and the wire
// protocol without unit ambiguity. Use Clock to obtain it.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace taskpilot
{

/// Lifecycle of a task.
///
/// Only kOpen and kInProgress are "actionable" and therefore eligible for the
/// ranked queue. kDone and kArchived are kept for history and stats.
enum class TaskStatus
{
    kOpen,       ///< Created, not started. Default for new tasks.
    kInProgress, ///< Someone is actively working on it.
    kDone,       ///< Completed; completed_at is set.
    kArchived,   ///< Abandoned or parked — excluded from stats of open work.
};

/// One work item.
///
/// Field-by-field notes on the ones that drive ranking:
///   - importance   : 1 (trivia) .. 5 (critical). Clamped by validation, not
///                    by this struct — the struct is a plain container.
///   - due_at       : optional deadline. Absent means "no deadline", which
///                    scores ZERO urgency (not maximum) — see PriorityEngine.
///   - blocks       : how many downstream tasks/people are waiting on this
///                    one. A plain count, not a graph; taskPilot deliberately
///                    does not model dependencies (keep the simple version
///                    simple). The caller supplies the number.
///   - tags         : free-form labels for filtering.
struct Task
{
    std::int64_t id{ 0 }; ///< 0 until the store assigns one.
    std::string title;    ///< Required, non-empty after trimming.
    std::string notes;    ///< Free-form detail; may be empty.

    TaskStatus status{ TaskStatus::kOpen };
    int importance{ 3 }; ///< 1..5. Default is the neutral midpoint.
    std::optional<std::int64_t> due_at; ///< Epoch seconds, UTC.
    int blocks{ 0 };      ///< >= 0 downstream dependents.

    std::vector<std::string> tags;

    std::int64_t created_at{ 0 };   ///< Epoch seconds; set by the store.
    std::int64_t updated_at{ 0 };   ///< Epoch seconds; set by the store.
    std::optional<std::int64_t> completed_at; ///< Set when status -> kDone.

    /// True when the task still wants attention (kOpen or kInProgress).
    [[nodiscard]] bool isActionable() const noexcept
    {
        return status == TaskStatus::kOpen || status == TaskStatus::kInProgress;
    }
};

/// Wire/disk name for a status ("open", "in_progress", "done", "archived").
[[nodiscard]] std::string toString(TaskStatus status);

/// Parse a wire name. Returns nullopt for anything unrecognised so callers
/// can turn it into a kInvalidArgument error with a useful message rather
/// than silently defaulting to kOpen.
[[nodiscard]] std::optional<TaskStatus> taskStatusFromString(const std::string &text);

/// Serialize a task. `due_at`/`completed_at` are emitted as null when unset
/// so clients see a stable schema instead of a missing key.
[[nodiscard]] nlohmann::json toJson(const Task &task);

/// Tags round-trip through SQLite as a JSON array in a TEXT column. Using
/// JSON rather than a comma-joined string avoids the escaping questions that
/// a delimiter would raise (a tag may legitimately contain a comma).
[[nodiscard]] std::string serializeTags(const std::vector<std::string> &tags);

/// Inverse of serializeTags. Unparseable or non-array input yields an empty
/// vector rather than throwing: a corrupt tags column should degrade to
/// "no tags", never take down a list query.
[[nodiscard]] std::vector<std::string> parseTags(const std::string &text);

} // namespace taskpilot
