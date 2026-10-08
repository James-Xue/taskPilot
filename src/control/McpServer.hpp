#pragma once
// McpServer.hpp — stdio MCP (Model Context Protocol) server, dual-era
//
// Speaks the MCP stdio transport: newline-delimited JSON-RPC 2.0 on
// stdin/stdout. It is a pure bridge — it owns no store and no ranking logic.
// Every tools/call is forwarded to the running daemon through ControlClient,
// so the daemon stays the single writer and a restarted `mcp` process cannot
// diverge from it.
//
// Stdio hygiene (critical, and the one rule that silently breaks everything):
// stdout carries the protocol stream and NOTHING else. One stray log line on
// stdout corrupts the framing and the host drops the server. So: log to
// stderr only, and never write to std::cout anywhere in this process.
//
// ---------------------------------------------------------------------------
// Dual-era support
// ---------------------------------------------------------------------------
// MCP revision 2026-07-28 is the largest breaking change since 2024-11-05. It
// removed the initialize handshake, removed protocol-level sessions, and made
// the core stateless: each request declares its protocol version, client
// identity, and capabilities in `_meta` instead of in a handshake. It also
// removed `ping` and requires servers to implement `server/discover`.
//
// The spec names the two eras:
//   legacy — establishes a session via `initialize`
//   modern — per-request metadata, revision 2026-07-28 and later
// A server supporting both is "dual-era". This one is, on purpose: pulseTrader
// answers only the legacy handshake, which works today only because the client
// happens to negotiate down; the question disappears if both are supported.
//
// How a client chooses: on stdio it probes with `server/discover` and falls
// back to `initialize` when the probe does not yield a usable answer. So the
// contract implemented here is:
//   1. `server/discover` answers with supportedVersions + capabilities
//      -> modern clients proceed with no handshake.
//   2. `initialize` still negotiates the legacy set -> legacy clients work
//      exactly as before.
//   3. tools/list and tools/call are served whether or not a handshake
//      happened. Nothing is gated on session state, which is what lets (1) and
//      (2) coexist, and is also simply true to the modern stateless model.
//
// Wire shapes below are taken from the reference implementation shipped in
// @modelcontextprotocol/server 2.0.0 (DiscoverResultSchema and the
// UnsupportedProtocolVersionError payload builder), not guessed. Two details
// are easy to get wrong and are silent when wrong — a client that cannot parse
// either reply simply classifies the server as legacy and never says why:
//   - the discover result key is `supportedVersions`, NOT `protocolVersions`;
//   - the -32022 error data is `{supported, requested}`, NOT a version list
//     under any other name. If `data.supported` is unreadable the client
//     abandons negotiation and falls back to `initialize` with no diagnostic.
//
// Tool list: derived from Services::methodSpecs(), not hand-copied. Because
// the catalog is static data in this same binary, tools/list still works when
// the daemon is down — the user sees the available tools and gets a clear
// error on the call, instead of an empty tool list with no explanation.

#include <functional>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "control/Services.hpp"

namespace taskpilot
{

/// Server identity reported in `initialize` and `server/discover`.
struct ServerInfo
{
    std::string name{ "taskpilot" };
    std::string version{ "0.1.0" };
};

/// Bridges MCP to the daemon.
class McpServer
{
  public:
    /// Forward (method, params) to the daemon. Injected so unit tests can
    /// exercise the protocol without a socket.
    using Backend = std::function<RpcResult(const std::string &method,
                                            const nlohmann::json &params)>;

    McpServer(Backend backend, ServerInfo info);

    /// Read requests from `in` until EOF, notifications/exit, or a closed
    /// stream; write responses to `out`.
    void run(std::istream &in, std::ostream &out);

    /// Handle one request line, returning the response line ("" when the
    /// request was a notification and must not be answered).
    ///
    /// `should_exit` is set on notifications/exit so the run loop can stop.
    [[nodiscard]] static std::string handleLine(const std::string &line,
                                                Backend &backend,
                                                const ServerInfo &info,
                                                bool &should_exit);

    /// The MCP tool list, derived from Services::methodSpecs().
    [[nodiscard]] static nlohmann::json toolDefinitions();

    /// Legacy (handshake-era) versions, newest last:
    /// 2024-10-07, 2024-11-05, 2025-03-26, 2025-06-18, 2025-11-25.
    ///
    /// `initialize` negotiates only within this list — a server MUST NOT
    /// answer a legacy handshake with a modern version, because a legacy
    /// client cannot parse a modern session.
    [[nodiscard]] static const std::vector<std::string> &legacyProtocolVersions();

    /// Newest legacy version — the cap for a legacy `initialize` reply.
    [[nodiscard]] static const std::string &newestLegacyProtocolVersion();

    /// Modern (stateless-era) versions: 2026-07-28 and later. Returned by
    /// `server/discover` and by the -32022 unsupported-version error.
    [[nodiscard]] static const std::vector<std::string> &modernProtocolVersions();

    /// Every version this server speaks: modern first, then legacy descending.
    /// This is the list advertised in `server/discover` and in an -32022 error.
    [[nodiscard]] static const std::vector<std::string> &supportedProtocolVersions();

    /// Select the version to answer a legacy `initialize` with:
    /// 1. The client's requested version when it is a supported legacy
    ///    version — the spec requires echoing a supported version verbatim.
    /// 2. Otherwise the newest legacy version, as the spec's counter-offer.
    ///    This is not an error; the client decides whether to continue.
    [[nodiscard]] static std::string negotiateLegacyVersion(const std::string &requested);

    /// Protocol version a modern request declared in `_meta`, if any.
    /// Returns an empty string when the request carries no version metadata
    /// (a legacy client, or a modern client that omits it).
    ///
    /// The key is `io.modelcontextprotocol/protocolVersion`, confirmed against
    /// the 2.0.0 SDK's request-metadata envelope.
    [[nodiscard]] static std::string declaredProtocolVersion(const nlohmann::json &params);

    /// True when `version` is in supportedProtocolVersions().
    [[nodiscard]] static bool isVersionSupported(const std::string &version);

    /// The `server/discover` result.
    ///
    /// Shape (reference SDK, DiscoverResultSchema):
    ///   supportedVersions : string[]      -- REQUIRED, this exact key
    ///   capabilities      : object        -- REQUIRED
    ///   instructions      : string        -- optional; the server omits it
    ///   ttlMs             : number        -- optional, defaults to 0
    ///   cacheScope        : string        -- optional, defaults to "private"
    ///
    /// ttlMs 0 and cacheScope "private" are stated explicitly rather than left
    /// to the client's defaults: the reply is specific to this server build,
    /// so caching it would be wrong.
    [[nodiscard]] static nlohmann::json discoverResult(const ServerInfo &info);

    /// The -32022 error payload for `requested`.
    ///
    /// Shape (reference SDK, and its client reads exactly these two keys):
    ///   { "supported": string[], "requested": string }
    ///
    /// The client uses `supported` to pick a mutually supported version and
    /// retry. If the list is unreadable it treats the server as legacy instead
    /// and never retries — which is why the key names matter more than they
    /// look like they should.
    [[nodiscard]] static nlohmann::json unsupportedVersionData(const std::string &requested);

  private:
    Backend m_backend;
    ServerInfo m_info;
};

/// JSON-RPC code for "unsupported protocol version", introduced by MCP
/// 2026-07-28. The error data carries the supported list so the client can
/// pick a mutually supported version and retry.
inline constexpr int kErrorUnsupportedProtocolVersion{ -32022 };

} // namespace taskpilot
