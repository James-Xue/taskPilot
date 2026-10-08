// Result.cpp — the non-template half of Result
//
// The success path lives entirely in the header, because Result<T> is a
// template. What is left for this translation unit is the error vocabulary:
// the five factories call sites spell as `Error::notFound("task 7")`, and the
// code -> name mapping the control layer puts on the wire.
//
// Keeping both here (rather than inline in the header) means there is exactly
// one definition of each wire string in the binary, so a grep for "not_found"
// finds the only place that decides it.

#include "core/Result.hpp"

#include <string>
#include <utility>

namespace taskpilot
{

// The factories exist so call sites read as intent. They all take the message
// by value and move it into the aggregate: the caller has already built the
// string, and the alternative (const&) would force a copy on every return.
//
// Each factory is a one-liner over the aggregate initializer rather than a
// member function body, so the struct stays a plain data container with no
// constructor of its own — handy for logging code that wants to build an
// Error{} and fill it in.

Error Error::invalidArgument(std::string message)
{
    return Error{ ErrorCode::kInvalidArgument, std::move(message) };
}

Error Error::notFound(std::string message)
{
    return Error{ ErrorCode::kNotFound, std::move(message) };
}

Error Error::conflict(std::string message)
{
    return Error{ ErrorCode::kConflict, std::move(message) };
}

Error Error::storageFailure(std::string message)
{
    return Error{ ErrorCode::kStorageFailure, std::move(message) };
}

Error Error::internal(std::string message)
{
    return Error{ ErrorCode::kInternal, std::move(message) };
}

// Stable lowercase names. These are an API, not a debug convenience: the
// control layer copies the value into JSON-RPC error data, and clients (the
// MCP bridge, scripts, a human reading a log) match on the string. Renaming
// one silently breaks every client that branches on it, which is why the
// spelling is asserted in tests/unit/test_result.cpp.
//
// The switch deliberately has no `default:` label, so adding an enumerator
// without teaching this function about it produces a -Wswitch warning (and
// the build treats warnings as errors).
std::string toString(ErrorCode code)
{
    switch (code)
    {
        case ErrorCode::kInvalidArgument:
            return "invalid_argument";
        case ErrorCode::kNotFound:
            return "not_found";
        case ErrorCode::kConflict:
            return "conflict";
        case ErrorCode::kStorageFailure:
            return "storage_failure";
        case ErrorCode::kInternal:
            return "internal";
    }

    // Reachable only through a static_cast of a value outside the enum, which
    // means corrupted input rather than a code path we designed. Reporting it
    // as "internal" keeps the log line a valid name instead of an empty
    // string, and it is the honest answer: a code we cannot name is a bug.
    return "internal";
}

} // namespace taskpilot
