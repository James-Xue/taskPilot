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

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "control/Services.hpp"

namespace taskpilot
{

/// One request/response exchange with the control socket.
class ControlClient
{
  public:
    ControlClient(std::string host, std::uint16_t port,
                  std::int64_t timeout_ms = 5000);

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
};

} // namespace taskpilot
