#pragma once
// ControlClient.hpp — blocking client for the daemon's control socket
//
// Used by the `mcp` and `cli` subcommands, both of which are short-lived
// processes that talk to a long-running `serve`. The connection is opened per
// call rather than held open, which keeps the client stateless: if the daemon
// restarts, the next call reconnects with no recovery logic.
//
// Timeouts are mandatory. Without a read timeout a wedged or half-open daemon
// would make every MCP tool call hang until the host client gives up, which
// presents to the user as Claude Code freezing rather than as an error.
//
// The reply is capped as well, and the cap is a constructor parameter rather
// than a constant because ONE call legitimately outgrows the default:
// export_tasks answers with the entire backlog, so its callers raise the cap
// (see kDefaultMaxReplyBytes below for why that call is the exception).

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "control/Services.hpp"

namespace taskpilot
{

/// Default deadline for ONE STEP of a call — connect, write, read — in
/// milliseconds. The budget is per step rather than per call (see the file
/// header of ControlClient.cpp for why each step carries its own), so a call
/// can take up to three of these end to end.
///
/// Named rather than left as a literal in the default argument so that a
/// caller which must pass the reply cap — the fourth parameter, and it cannot
/// be reached positionally without the third — can say "the default timeout"
/// instead of repeating the number.
inline constexpr std::int64_t kDefaultTimeoutMs{ 5000 };

/// Default ceiling on one reply line, in bytes (1 MiB).
///
/// A runaway guard, NOT a size policy. The reply of an ordinary call (get_queue
/// or list_tasks over a personal backlog) is tens of kilobytes, so this bounds
/// nothing a healthy daemon produces; what it does bound is the one failure a
/// limit can prevent — a peer that answers without ever sending a newline and
/// would otherwise make the client allocate for as long as the peer keeps
/// talking.
///
/// export_tasks is the deliberate exception, and the reason the cap is a
/// parameter: its reply IS the whole backlog, so it grows as the backlog grows
/// and a fixed cap eventually makes the call fail permanently — the whole
/// synchronisation workflow with it, because the document must arrive as one
/// line and there is nothing to page. Callers that invoke it pass a larger cap
/// through the constructor; every other call keeps this one.
inline constexpr std::size_t kDefaultMaxReplyBytes{ 1024 * 1024 };

/// One request/response exchange with the control socket.
class ControlClient
{
  public:
    /// `timeout_ms` bounds each step of a call, floored at 1 ms so a zero or
    /// negative argument cannot produce a timer that fires before the step it
    /// is meant to bound.
    ///
    /// `max_reply_bytes` is the ceiling on the reply line: a peer that sends
    /// more than this without a newline fails the call with an Error naming the
    /// bound. The default suits every call whose reply does not grow with the
    /// stored data; raise it for the one that does (export_tasks).
    ControlClient(std::string host, std::uint16_t port,
                  std::int64_t timeout_ms = kDefaultTimeoutMs,
                  std::size_t max_reply_bytes = kDefaultMaxReplyBytes);

    /// Send one JSON-RPC request and return its `result`.
    ///
    /// Failure modes, all returned as Errors rather than thrown:
    ///   1. Cannot connect        -> kStorageFailure with "daemon not running"
    ///                               guidance, since that is by far the most
    ///                               common cause and the fix is a command.
    ///   2. Timeout writing/reading -> kInternal, naming the timeout.
    ///   3. Peer closed mid-read  -> kInternal.
    ///   4. JSON-RPC `error` reply -> mapped back from the wire code to the
    ///                               ErrorCode family (see fromRpcErrorCode).
    ///   5. Malformed reply       -> kInternal, including a truncated excerpt
    ///                               of what arrived so it can be diagnosed.
    ///   6. Reply over the cap    -> kInternal, naming the byte ceiling that
    ///                               was hit, so the number a caller has to
    ///                               raise is in the message it reads.
    [[nodiscard]] RpcResult call(const std::string &method,
                                 const nlohmann::json &params) const;

    /// True if a TCP connection can be established. Cheap liveness probe used
    /// by the CLI and by `mcp` at startup; intentionally does NOT send a
    /// request, so it stays useful when the daemon is up but the store is slow.
    [[nodiscard]] bool ping() const;

    /// Map a JSON-RPC error code back onto an ErrorCode. Inverse of
    /// rpcErrorCode(); unknown application codes degrade to kInternal.
    [[nodiscard]] static ErrorCode fromRpcErrorCode(int code);

    [[nodiscard]] const std::string &host() const { return m_host; }
    [[nodiscard]] std::uint16_t port() const { return m_port; }

  private:
    std::string m_host;
    std::uint16_t m_port;
    std::int64_t m_timeoutMs;
    std::size_t m_maxReplyBytes;
};

} // namespace taskpilot
