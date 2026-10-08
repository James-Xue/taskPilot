// McpServer.cpp — see McpServer.hpp
//
// This file is a protocol adapter and nothing else: it turns one JSON-RPC
// message into one response line, and it forwards calls to the daemon through
// the injected Backend. Three invariants drive every decision below.
//
//   1. Framing. stdout carries protocol objects and nothing else, one object
//      per line. nlohmann::json::dump() escapes control characters, so a
//      payload that itself contains newlines still serializes onto a single
//      line; handleLine() never appends a newline, and run() appends exactly
//      one per answered request. Diagnostics therefore go to stderr only.
//
//   2. Era coexistence. Nothing is gated on session state: tools/list and
//      tools/call answer identically whether or not an `initialize` handshake
//      happened. That single property is what lets a handshake-era client and a
//      stateless-era client talk to the same process without either protocol
//      needing to know the other exists — and it is also simply true to the
//      2026-07-28 stateless model, where there is no session to key on.
//
//   3. Notifications are never answered. Every response goes through one
//      `respond` lambda whose first act is to return an empty string when the
//      request carried no id. Replying to a notification desynchronises the
//      host's request/response pairing, so the rule must not be a per-site
//      decision that a future edit can forget.

#include "control/McpServer.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Result.hpp"

namespace taskpilot
{

namespace
{

// ---------------------------------------------------------------------------
// JSON-RPC 2.0 protocol codes. The application-error range (-32000..-32099) is
// extended by kErrorUnsupportedProtocolVersion, which the header declares
// because the MCP spec gives it a name.
// ---------------------------------------------------------------------------
constexpr int kParseError{ -32700 };
constexpr int kInvalidRequest{ -32600 };
constexpr int kMethodNotFound{ -32601 };
constexpr int kInvalidParams{ -32602 };

/// The `_meta` key a modern request uses to declare the protocol version it
/// speaks. It is one flat key that happens to contain a slash — not a nested
/// path — so it must be looked up verbatim.
constexpr const char *kProtocolVersionMetaKey{
    "io.modelcontextprotocol/protocolVersion"
};

/// Build the `error` member of a JSON-RPC error response.
///
/// `data` is attached only when it is non-null. Ordinary protocol faults
/// (-32700/-32600/-32601/-32602) keep the minimal two-field shape from the
/// spec; only -32022 carries data, because the spec defines that payload as the
/// recovery instruction the client needs in order to retry.
nlohmann::json errorPayload(int code, const std::string &message,
                            const nlohmann::json &data = nullptr)
{
    nlohmann::json error{
        { "code", code },
        { "message", message },
    };
    if (!data.is_null())
    {
        error["data"] = data;
    }
    return nlohmann::json{ { "error", error } };
}

/// Build the `result` member of a successful JSON-RPC response.
nlohmann::json resultPayload(nlohmann::json result)
{
    return nlohmann::json{ { "result", std::move(result) } };
}

/// Wrap a `result`/`error` payload in the JSON-RPC 2.0 envelope.
///
/// `id` is echoed verbatim, or null when the message could not be trusted
/// enough to read an id from it (see the two call sites in handleLine).
std::string encodeResponse(const nlohmann::json &id, const nlohmann::json &payload)
{
    nlohmann::json response{
        { "jsonrpc", "2.0" },
        { "id", id },
    };
    // payload is exactly one of {"result":...} / {"error":...}; update() merges
    // that single key into the envelope.
    response.update(payload);
    return response.dump();
}

/// Wrap a tool outcome as MCP text content.
///
/// `isError` is the MCP-level failure flag and is deliberately independent of
/// the JSON-RPC status: a tool that fails is a *successful* call that reports
/// isError:true, so the model still receives a readable message to act on.
nlohmann::json textContent(const std::string &text, bool is_error)
{
    return nlohmann::json{
        { "content", nlohmann::json::array({ nlohmann::json{
              { "type", "text" },
              { "text", text } } }) },
        { "isError", is_error },
    };
}

/// The capabilities block, byte-identical in `initialize` and
/// `server/discover` because it describes the server rather than a session.
///
/// `listChanged:false` is not a placeholder: the tool catalog is static data
/// compiled into this binary (Services::methodSpecs()), so it cannot change
/// while the process runs and no notification will ever contradict that.
nlohmann::json capabilitiesPayload()
{
    return nlohmann::json{
        { "tools", nlohmann::json{ { "listChanged", false } } },
    };
}

/// Server identity, shaped as the spec's ServerInfo (name + version).
nlohmann::json serverInfoPayload(const ServerInfo &info)
{
    return nlohmann::json{
        { "name", info.name },
        { "version", info.version },
    };
}

/// Read `params.protocolVersion` — the handshake-era version declaration.
///
/// A wrong-typed or absent value yields an empty string rather than an
/// exception: version negotiation can still succeed by counter-offer, and
/// throwing on client input would turn a recoverable mismatch into a crash.
std::string requestedLegacyVersion(const nlohmann::json &params)
{
    if (!params.is_object() || !params.contains("protocolVersion")
        || !params["protocolVersion"].is_string())
    {
        return std::string{};
    }
    return params["protocolVersion"].get<std::string>();
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
McpServer::McpServer(Backend backend, ServerInfo info)
    : m_backend{ std::move(backend) }, m_info{ std::move(info) }
{
    // Nothing to initialise beyond the two injected dependencies: the server is
    // deliberately stateless, which is what lets tools/list and tools/call be
    // served before, during, and after a handshake without any ordering rules.
}

// ---------------------------------------------------------------------------
// Version catalog
// ---------------------------------------------------------------------------
const std::vector<std::string> &McpServer::legacyProtocolVersions()
{
    // Ascending, newest last — the order is a contract, not a convenience:
    // `initialize` negotiation and the counter-offer cap are both phrased in
    // terms of it, and supportedProtocolVersions() reverses it to build the
    // newest-first wire list. So a revision inserted anywhere but its
    // chronological place silently changes which version a legacy client is
    // offered.
    //
    // The set is the SDK's legacy revisions. 2024-10-07 is the oldest and is
    // listed for completeness rather than for its features: a client that old
    // is rare, but omitting it costs such a client its echoed handshake for no
    // gain, since echoing an older version it asked for is exactly what the
    // spec requires.
    static const std::vector<std::string> kVersions{
        "2024-10-07",
        "2024-11-05",
        "2025-03-26",
        "2025-06-18",
        "2025-11-25",
    };
    return kVersions;
}

const std::string &McpServer::newestLegacyProtocolVersion()
{
    // Derived from the list rather than repeated as a literal, so the cap can
    // never disagree with the list it caps.
    static const std::string kNewest{ legacyProtocolVersions().back() };
    return kNewest;
}

const std::vector<std::string> &McpServer::modernProtocolVersions()
{
    static const std::vector<std::string> kVersions{
        "2026-07-28",
    };
    return kVersions;
}

const std::vector<std::string> &McpServer::supportedProtocolVersions()
{
    // Built once, on first use. Modern first, then legacy DESCENDING, so the
    // list reads newest-to-oldest throughout and matches the spec's example
    // ordering of ["2026-07-28", "2025-11-25"]. A client that simply takes the
    // first entry it recognises therefore lands on the newest version both
    // sides speak, without having to understand the era split.
    static const std::vector<std::string> kVersions = [] {
        std::vector<std::string> versions = modernProtocolVersions();
        const std::vector<std::string> &legacy = legacyProtocolVersions();
        versions.insert(versions.end(), legacy.rbegin(), legacy.rend());
        return versions;
    }();
    return kVersions;
}

bool McpServer::isVersionSupported(const std::string &version)
{
    const std::vector<std::string> &supported = supportedProtocolVersions();
    return std::find(supported.begin(), supported.end(), version) != supported.end();
}

// ---------------------------------------------------------------------------
// Modern-era wire payloads
//
// Both shapes below come from the reference SDK (@modelcontextprotocol/server
// 2.0.0) rather than from a reading of the prose spec, because both are silent
// when wrong: a client that cannot parse either reply classifies this server as
// legacy and never reports why. The key names are therefore the whole contract,
// and they are built here — in one place — so no reply can drift from the other.
// ---------------------------------------------------------------------------
nlohmann::json McpServer::discoverResult(const ServerInfo &info)
{
    // The answer to the modern probe. Five decisions:
    //
    // 1. `supportedVersions` — REQUIRED, exactly this spelling (plural
    //    "supported", no "protocol"). A reply missing it makes the client fall
    //    back to `initialize`, which is the failure this whole path exists to
    //    remove. Taken from the catalog rather than repeated, so it can never
    //    disagree with the -32022 payload a moment later.
    // 2. `capabilities` — REQUIRED, and the same block `initialize` reports: it
    //    describes the server build, not a session, and there is no session here
    //    to describe.
    // 3. `serverInfo` — the identity `initialize` reports as well, so a client
    //    that discovers rather than handshakes can still name this server in its
    //    UI and logs.
    // 4. `ttlMs` 0 and `cacheScope` "private" — stated rather than left to the
    //    client's defaults, although those defaults happen to be the same
    //    values. The reply is specific to this build (the catalog is compiled
    //    in), so a client that cached it and replayed it against another build
    //    would act on a version list that build does not honour; and a default
    //    documented elsewhere is exactly the kind of thing that can change
    //    without this file noticing.
    // 5. `instructions` is omitted — optional in the schema, and this bridge has
    //    no server-wide guidance to add that the per-tool descriptions do not
    //    already carry.
    return nlohmann::json{
        { "supportedVersions", supportedProtocolVersions() },
        { "capabilities", capabilitiesPayload() },
        { "serverInfo", serverInfoPayload(info) },
        { "ttlMs", 0 },
        { "cacheScope", "private" },
    };
}

nlohmann::json McpServer::unsupportedVersionData(const std::string &requested)
{
    // The -32022 payload, again by exact key, because the SDK's client reads
    // `data.supported` to pick a version both sides speak and `data.requested`
    // to report which version caused the mismatch. If `supported` is unreadable
    // the client abandons negotiation and falls back to `initialize` with no
    // diagnostic — so these two names are the entire recovery mechanism, and a
    // version list under any other key is not a harmless alias: it would be a
    // second contract to keep in sync that nothing on the client reads.
    //
    // `requested` is echoed verbatim. The client matches it against the version
    // it sent, so a "tidied" form here would break that match.
    return nlohmann::json{
        { "supported", supportedProtocolVersions() },
        { "requested", requested },
    };
}

std::string McpServer::negotiateLegacyVersion(const std::string &requested)
{
    // Two outcomes, one of which is an error in neither reading:
    //
    // 1. The client named a version this server speaks -> echo it verbatim.
    //    The spec requires the reply to carry a version the client offered, so
    //    the client can proceed without a second round trip.
    // 2. Anything else -> counter-offer the newest legacy version. The client
    //    decides whether it can speak that; a client that cannot will disconnect
    //    rather than be silently given a session it cannot parse.
    //
    // Note what is deliberately absent: no modern version can be returned here,
    // even though this server speaks one. A legacy client cannot parse a modern
    // session, so answering its handshake with 2026-07-28 would be the one
    // failure mode that makes dual-era support worse than picking an era.
    const std::vector<std::string> &legacy = legacyProtocolVersions();
    if (std::find(legacy.begin(), legacy.end(), requested) != legacy.end())
    {
        return requested;
    }
    return newestLegacyProtocolVersion();
}

std::string McpServer::declaredProtocolVersion(const nlohmann::json &params)
{
    // Modern requests declare their version per request instead of in a
    // handshake, under `params._meta`. Three readings are worth spelling out:
    //
    // 1. No `_meta`, or no version key inside it -> "". That is a legacy client
    //    (or a modern one that omits the declaration), and it must be served
    //    rather than rejected. This reader is what keeps the version gate from
    //    ever standing between a legacy client and its handshake.
    // 2. `_meta` present but not an object, or the version value not a string ->
    //    also "". The function reports the *presence* of a usable declaration,
    //    not the validity of the metadata block; a malformed declaration is
    //    treated as no declaration instead of as a protocol fault, which keeps
    //    an experimental client's stray metadata from failing a call that would
    //    otherwise have worked.
    // 3. A declared version is returned even when this server does not support
    //    it — the caller decides what to do with that, because only the caller
    //    knows it is a request (which gets -32022) or a notification (which
    //    gets silence).
    if (!params.is_object() || !params.contains("_meta"))
    {
        return std::string{};
    }
    const nlohmann::json &meta = params["_meta"];
    if (!meta.is_object() || !meta.contains(kProtocolVersionMetaKey))
    {
        return std::string{};
    }
    const nlohmann::json &version = meta[kProtocolVersionMetaKey];
    if (!version.is_string())
    {
        return std::string{};
    }
    return version.get<std::string>();
}

// ---------------------------------------------------------------------------
// Tool definitions
// ---------------------------------------------------------------------------
nlohmann::json McpServer::toolDefinitions()
{
    // Derived from the single method catalog — never a hand-copied list.
    //
    // The catalog is static data compiled into THIS binary, so the tool list is
    // available before any daemon is reached. That matters for diagnosis: when
    // the daemon is down, the host still sees the full tool set and gets a
    // clear error naming the connection failure on each call, instead of an
    // empty tool list that looks like a broken installation and explains
    // nothing.
    const std::vector<MethodSpec> &specs = Services::methodSpecs();

    nlohmann::json tools = nlohmann::json::array();
    for (const MethodSpec &spec : specs)
    {
        // `type: "object"` is enforced here rather than trusted from the
        // catalog: an MCP host rejects a tool whose inputSchema is not a JSON
        // Schema object, and a single catalog entry with a missing or non-object
        // schema would then cost the model EVERY tool in the list. Re-stating
        // the type is idempotent for a well-formed schema and repairs a
        // malformed one; all other schema keys pass through untouched.
        nlohmann::json schema = spec.params_schema.is_object()
            ? spec.params_schema
            : nlohmann::json::object();
        schema["type"] = "object";

        tools.push_back(nlohmann::json{
            { "name", spec.name },
            { "description", spec.description },
            { "inputSchema", schema },
        });
    }
    return tools;
}

// ---------------------------------------------------------------------------
// One line in, at most one line out
// ---------------------------------------------------------------------------
std::string McpServer::handleLine(const std::string &line, Backend &backend,
                                  const ServerInfo &info, bool &should_exit)
{
    // 1. Parse. Unparseable input gets the parse error with a NULL id: there is
    //    no trustworthy id inside a message that is not JSON, and the spec
    //    explicitly calls for null exactly here.
    nlohmann::json request;
    try
    {
        request = nlohmann::json::parse(line);
    }
    catch (const nlohmann::json::parse_error &)
    {
        return encodeResponse(nullptr, errorPayload(kParseError, "Parse error"));
    }

    // 2. Structural validation. A message that is not an object, or that has no
    //    string method, is not a request at all — it cannot be dispatched and it
    //    cannot be trusted to have been meant as a request rather than as a
    //    notification. Answering with a null id is the spec's rule for that
    //    case, and it keeps us from replying into a stream that was not waiting
    //    for a reply.
    if (!request.is_object() || !request.contains("method")
        || !request["method"].is_string())
    {
        return encodeResponse(nullptr,
                              errorPayload(kInvalidRequest, "Invalid request"));
    }

    const std::string method = request["method"].get<std::string>();

    // 3. The id decides whether a reply is allowed at all. JSON-RPC 2.0 defines
    //    a message without an id (or with a null id) as a notification: its side
    //    effects still happen, nothing is written back. is_notification is
    //    computed once, here, and consulted by every response path.
    const bool is_notification = !request.contains("id") || request["id"].is_null();

    // params is optional in JSON-RPC (a call may take no arguments). An absent
    // key is defaulted to an empty object; a present-but-not-an-object params is
    // passed through unchanged so the per-method validation below can reject it
    // by name instead of it being silently coerced into {}. (contains() plus
    // element access rather than value(): value() with a temporary default can
    // hand back a reference to that temporary.)
    nlohmann::json params = nlohmann::json::object();
    if (request.contains("params"))
    {
        params = request["params"];
    }

    // Whether this request arrived through the modern (per-request metadata)
    // era. A legacy client declares its version inside `initialize` instead of
    // in `_meta`, so a version present in `_meta` IS the era marker — there is
    // no other shape a modern request can take. Declared before the respond
    // funnel because the funnel reads it, and set below at the version gate.
    bool modern_request = false;

    // Single funnel for every response. Placing the notification test inside it
    // (rather than at each return site) is what makes the rule impossible to
    // forget as methods are added.
    auto respond = [&](const nlohmann::json &payload) -> std::string
    {
        if (is_notification)
        {
            return std::string{};
        }

        // Modern results must be self-describing. Revision 2026-07-28 requires
        // every result to carry `resultType`, and the "absent means complete"
        // bridge is granted only to earlier-revision servers. The cost of
        // omitting it is not a warning: a client that saw 2026-07-28 in our
        // version list rejects the result outright, and a rejected tools/list
        // means the model is handed no tools at all while the server still
        // looks connected. "complete" is the value the reference SDK defaults
        // to; the field is an open union, so a future partial-result mode can
        // add values without breaking this.
        //
        // Stamped here, in the one funnel, rather than at each result site —
        // same reasoning as the notification test above, and it is why adding
        // a method cannot forget it.
        if (modern_request && payload.contains("result") && payload["result"].is_object())
        {
            nlohmann::json stamped = payload;
            stamped["result"]["resultType"] = "complete";
            return encodeResponse(request["id"], stamped);
        }
        return encodeResponse(request["id"], payload);
    };

    // 4. Handshake-era bookkeeping notifications. Handled before the version
    //    gate on purpose: they are pure notifications with nothing to answer,
    //    and `exit` must stop the loop even if the message carries stray
    //    metadata.
    if ("notifications/initialized" == method)
    {
        // The legacy client confirming the handshake. There is nothing to
        // record — this server keeps no session state — so silence is the whole
        // acknowledgement.
        return std::string{};
    }
    if ("notifications/exit" == method)
    {
        // The host is shutting the session down. Recorded for the run loop,
        // which stops before reading another line; answering would be pointless
        // because the stream is about to close.
        should_exit = true;
        return std::string{};
    }

    // 5. Protocol-version gate (modern era). A request that declares a version
    //    in `_meta` and names one this server does not speak is refused with
    //    -32022 plus the supported list. That is not a dead end: the spec makes
    //    it the negotiation step — the client reads the list, picks a version
    //    both sides speak, and retries.
    //
    //    A request that declares nothing (a legacy client, which states its
    //    version inside `initialize` instead) sails straight through, which is
    //    what lets one dispatch path serve both eras. Because the gate responds
    //    through the funnel, a notification declaring an unsupported version is
    //    dropped in silence rather than provoking an illegal reply.
    const std::string declared = declaredProtocolVersion(params);
    // Recorded before the gate decides anything, because an UNSUPPORTED version
    // is still a modern request and its -32022 reply must be a well-formed
    // modern result. See the respond funnel for what this stamps.
    modern_request = !declared.empty();
    if (!declared.empty() && !isVersionSupported(declared))
    {
        return respond(errorPayload(kErrorUnsupportedProtocolVersion,
                                    "Unsupported protocol version: " + declared,
                                    unsupportedVersionData(declared)));
    }

    // 6. initialize — the legacy handshake.
    if ("initialize" == method)
    {
        // The requested version lives in `params.protocolVersion` here, NOT in
        // `_meta`: that placement difference is the entire distinction between
        // the two eras on the wire.
        const std::string requested = requestedLegacyVersion(params);
        const std::string agreed = negotiateLegacyVersion(requested);

        return respond(resultPayload(nlohmann::json{
            { "protocolVersion", agreed },
            { "capabilities", capabilitiesPayload() },
            { "serverInfo", serverInfoPayload(info) },
        }));
    }

    // 7. server/discover — the modern entry point.
    if ("server/discover" == method)
    {
        nlohmann::json result = discoverResult(info);

        // The one log line in this file, and it is on stderr: stdout is the
        // protocol stream. It exists because a discover probe is the client's
        // first message and its shape decides whether the modern era is
        // reachable at all, so observing a live host's probe should not require
        // a packet capture.
        std::cerr << "[mcp] server/discover -> "
                  << result["supportedVersions"].dump() << "\n";

        return respond(resultPayload(std::move(result)));
    }

    // 8. ping — removed by the modern era, retained for the legacy one.
    //    A legacy client uses it as a liveness probe; answering costs one empty
    //    result, and refusing would look like a dead server to a client that is
    //    otherwise negotiating correctly.
    if ("ping" == method)
    {
        return respond(resultPayload(nlohmann::json::object()));
    }

    // 9. tools/list — served with or without a handshake.
    if ("tools/list" == method)
    {
        // No session check, deliberately. A modern client calls this as its
        // first (or only) message; a legacy client calls it after `initialize`.
        // Both get the same answer, which is the coexistence contract.
        //
        // ttlMs/cacheScope are stated rather than left to the client's defaults
        // because the 2026-07-28 schema for this result declares both. Zero ttl
        // means "do not cache": the catalog is static, so caching would be
        // safe, but a stale tool list is the kind of failure that looks like a
        // broken server, and this call is cheap. `private` because the reply
        // describes this build of this server, not a shared resource.
        return respond(resultPayload(nlohmann::json{
            { "tools", toolDefinitions() },
            { "ttlMs", 0 },
            { "cacheScope", "private" },
        }));
    }

    // 10. tools/call — validate, then forward to the daemon.
    if ("tools/call" == method)
    {
        // 10a. Shape: params must be an object carrying a string name. Without
        //      a name there is no tool to call, so this is invalid params
        //      rather than an unknown tool.
        if (!params.is_object() || !params.contains("name")
            || !params["name"].is_string())
        {
            return respond(errorPayload(kInvalidParams, "Invalid params: name required"));
        }
        const std::string name = params["name"].get<std::string>();

        // 10b. The MCP layer owns the tool vocabulary, so it can name the
        //      offending tool — a much better message for a model than the
        //      daemon's generic "no such method". The catalog is local static
        //      data, so this check does not depend on the daemon being up.
        if (nullptr == Services::findSpec(name))
        {
            return respond(errorPayload(kInvalidParams, "Unknown tool: " + name));
        }

        // 10c. `arguments` is optional. An explicit null is how several clients
        //      spell "no arguments", so it is normalised to {} rather than
        //      forwarded as null into argument validation. Any other non-object
        //      value is forwarded untouched: the daemon owns parameter
        //      validation, and duplicating it here would be a second opinion
        //      that can disagree with the first.
        nlohmann::json arguments = nlohmann::json::object();
        if (params.contains("arguments") && !params["arguments"].is_null())
        {
            arguments = params["arguments"];
        }

        RpcResult outcome = backend(name, arguments);

        // 10d. Tool failure is NOT a JSON-RPC error. The call itself succeeded —
        //      it reached the tool and the tool said no — so the failure travels
        //      as isError:true content. That distinction is what lets the model
        //      read "error (not_found): no task with id 7" and correct itself,
        //      instead of seeing a protocol fault it cannot interpret.
        //
        //      The code is spelled as its stable NAME, through the same
        //      toString() the TCP transport puts in error.data.code. The enum
        //      ordinal it replaces ("code=1") names no category, so a model
        //      cannot tell a fixable argument from a system fault; sharing the
        //      TCP vocabulary means a model's rule for one transport is true for
        //      the other.
        if (outcome.ok())
        {
            // Pretty-printed because the text lands in a model's context: a
            // single 4 KB line of JSON is materially harder to read than an
            // indented one. The embedded newlines are escaped by dump() at
            // envelope level, so framing stays intact — see run().
            return respond(resultPayload(textContent(outcome.value().dump(2), false)));
        }
        const Error &failure = outcome.error();
        return respond(resultPayload(textContent(
            "error (" + toString(failure.code) + "): " + failure.message, true)));
    }

    // 11. Anything else. A valid request with an unimplemented method is
    //     -32601, echoed with its own id so the client can correlate it.
    return respond(errorPayload(kMethodNotFound, "Method not found"));
}

// ---------------------------------------------------------------------------
// Run loop
// ---------------------------------------------------------------------------
void McpServer::run(std::istream &in, std::ostream &out)
{
    bool should_exit = false;
    std::string line;

    // The loop condition tests should_exit as well as the stream: a
    // notifications/exit must stop us even though the host may not close stdin
    // for a while afterwards, and the close itself is the normal EOF exit.
    while (!should_exit && std::getline(in, line))
    {
        // 1. Tolerate CRLF framing. Some hosts write "\r\n" to a stream the
        //    server reads line-oriented, and a stray CR would otherwise turn
        //    every message into a parse error.
        if (!line.empty() && '\r' == line.back())
        {
            line.pop_back();
        }

        // 2. Lines that carry no request — blank, or the trailing whitespace a
        //    host flushed between messages — are skipped silently rather than
        //    answered with a parse error. There is nothing for the host to
        //    associate such an error with, so it would only add noise the client
        //    cannot act on.
        if (std::string::npos == line.find_first_not_of(" \t\r\n"))
        {
            continue;
        }

        const std::string response = handleLine(line, m_backend, m_info, should_exit);
        if (response.empty())
        {
            // A notification: the side effect already happened, and writing
            // anything here would desynchronise the host's reply pairing.
            continue;
        }

        // 3. One response object, one line, flushed immediately. Flushing per
        //    line matters more than batching: an MCP host blocks on each reply,
        //    so a buffered response is indistinguishable from a hung server.
        //    response is dump()ed JSON, which never contains a raw newline, so
        //    appending exactly one here is the framing guarantee this file
        //    exists to provide.
        out << response << '\n';
        out.flush();
    }
}

} // namespace taskpilot
