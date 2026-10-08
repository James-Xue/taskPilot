#pragma once
// Result.hpp — lightweight success-or-error return type
//
// Why this exists instead of std::expected:
//   taskPilot targets C++20. std::expected is C++23, so we carry a tiny
//   equivalent rather than raise the language standard for one type.
//
// Why a Result at all instead of exceptions:
//   Every error crossing the RPC boundary must become a structured JSON-RPC
//   error object. Encoding failure in the return type forces each call site
//   to acknowledge it, and lets the dispatcher map ErrorCode -> wire code in
//   one place instead of catching exceptions scattered across the stack.
//
// Usage:
//   Result<Task> t = store.getTask(7);
//   if (!t.ok()) { return t.error(); }      // propagate
//   use(t.value());
//
// The Error constructors are intentionally implicit so a function returning
// Result<T> can `return Error{...}` without ceremony.

#include <string>
#include <utility>
#include <variant>

namespace taskpilot
{

/// Failure categories. Each maps to exactly one JSON-RPC error code in the
/// control layer (see Services.cpp::rpcErrorCode) — keep the two in sync.
enum class ErrorCode
{
    kInvalidArgument, ///< Caller-supplied input failed validation.
    kNotFound,        ///< Referenced entity does not exist.
    kConflict,        ///< Request conflicts with current state.
    kStorageFailure,  ///< SQLite or filesystem layer failed.
    kInternal,        ///< Bug or unexpected state; not the caller's fault.
};

/// A structured failure. `message` is human-readable and safe to surface to
/// an LLM client — never embed credentials or absolute paths in it.
struct Error
{
    ErrorCode code{ ErrorCode::kInternal };
    std::string message;

    /// Convenience factory so call sites read as `Error::notFound("task 7")`.
    [[nodiscard]] static Error invalidArgument(std::string message);
    [[nodiscard]] static Error notFound(std::string message);
    [[nodiscard]] static Error conflict(std::string message);
    [[nodiscard]] static Error storageFailure(std::string message);
    [[nodiscard]] static Error internal(std::string message);
};

/// Stable lowercase name for an error code, used in log lines and in the
/// `data` field of JSON-RPC errors.
[[nodiscard]] std::string toString(ErrorCode code);

/// Placeholder for "success with no payload", so void-returning operations
/// can use the same Result machinery (aliased below as Status).
struct Unit
{
};

/// Either a value or an Error — never both, never neither.
///
/// Thread-safety: a Result is an ordinary value; copying/moving it is safe.
/// It carries no locks and makes no guarantee about the thread-safety of T.
template <typename T>
class Result
{
  public:
    /// Implicit success construction: `return task;` from a Result<Task> fn.
    Result(T value) : m_data{ std::move(value) } {}

    /// Implicit failure construction: `return Error::notFound("...");`
    Result(Error error) : m_data{ std::move(error) } {}

    [[nodiscard]] bool ok() const noexcept
    {
        return std::holds_alternative<T>(m_data);
    }

    /// Precondition: ok(). Calling value() on a failed Result is a
    /// programming error and throws std::bad_variant_access.
    [[nodiscard]] const T &value() const & { return std::get<T>(m_data); }
    [[nodiscard]] T &value() & { return std::get<T>(m_data); }
    [[nodiscard]] T &&value() && { return std::get<T>(std::move(m_data)); }

    /// Precondition: !ok().
    [[nodiscard]] const Error &error() const { return std::get<Error>(m_data); }

    /// Value if present, otherwise `fallback`. Does not consume the Result.
    [[nodiscard]] T valueOr(T fallback) const
    {
        return ok() ? std::get<T>(m_data) : std::move(fallback);
    }

  private:
    std::variant<T, Error> m_data;
};

/// Result for operations that succeed with no payload.
using Status = Result<Unit>;

} // namespace taskpilot
