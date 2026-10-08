// test_mcp_server.cpp — protocol tests for the stdio MCP bridge
//
// Every case drives McpServer::handleLine()/run() directly, with a fake Backend
// and in-memory streams, so the suite needs no daemon, no socket, and no store:
// the injection point that keeps production code decoupled is the same one the
// tests use. What is asserted here is the wire contract — framing, era
// negotiation, error codes, and the notification rule — because a mistake in any
// of those reaches the user as "the MCP server disconnected", which names
// neither the rule that broke nor the message that broke it.

#include "control/McpServer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "control/Services.hpp"
#include "core/Result.hpp"

namespace taskpilot
{
namespace
{

/// The `_meta` key a modern request uses to declare its protocol version.
constexpr const char *kProtocolVersionMetaKey{
    "io.modelcontextprotocol/protocolVersion"
};

/// Identity injected into every case, so the tests assert the value that was
/// actually passed in rather than the struct's defaults.
const ServerInfo kTestServerInfo{ "taskpilot-test", "9.9.9-test" };

/// Stand-in for the ControlClient-backed Backend: records what the bridge
/// forwarded and answers with a canned Result, so "the daemon failed this call"
/// can be exercised as cheaply as "the daemon answered".
class FakeBackend
{
  public:
    FakeBackend()
        : m_backend([this](const std::string &method,
                           const nlohmann::json &params) -> RpcResult
                    {
                        last_method = method;
                        last_params = params;
                        ++calls;
                        return m_reply;
                    })
    {
    }

    // The lambda above captures `this`, so copying this object would leave the
    // copy's callback pointing at the original.
    FakeBackend(const FakeBackend &) = delete;
    FakeBackend &operator=(const FakeBackend &) = delete;

    /// Answer the next call with a successful payload.
    void setReply(const nlohmann::json &payload) { m_reply = RpcResult{ payload }; }

    /// Answer the next call with a tool-level failure.
    void setFailure(const Error &failure) { m_reply = RpcResult{ failure }; }

    [[nodiscard]] McpServer::Backend &backend() { return m_backend; }

    std::string last_method;
    nlohmann::json last_params;
    int calls{ 0 };

  private:
    RpcResult m_reply{ nlohmann::json::object() };
    McpServer::Backend m_backend;
};

/// A request line: has an id, therefore MUST be answered.
std::string requestLine(const std::string &method,
                        const nlohmann::json &params = nlohmann::json::object(),
                        const nlohmann::json &id = 1)
{
    nlohmann::json request{
        { "jsonrpc", "2.0" },
        { "id", id },
        { "method", method },
    };
    if (!params.is_null())
    {
        request["params"] = params;
    }
    return request.dump();
}

/// The same message without an id: a notification, which must NOT be answered.
std::string notificationLine(const std::string &method,
                             const nlohmann::json &params = nlohmann::json::object())
{
    nlohmann::json request{
        { "jsonrpc", "2.0" },
        { "method", method },
    };
    if (!params.is_null())
    {
        request["params"] = params;
    }
    return request.dump();
}

/// params carrying a modern (stateless-era) version declaration.
nlohmann::json paramsWithDeclaredVersion(const std::string &version,
                                         nlohmann::json params = nlohmann::json::object())
{
    params["_meta"] = nlohmann::json{ { kProtocolVersionMetaKey, version } };
    return params;
}

/// Drive one line through the static handler and parse the reply. A request
/// that produced no reply is a framing bug, so it fails the calling test rather
/// than returning something the assertions below would misread.
nlohmann::json exchange(const std::string &line, FakeBackend &backend,
                        bool &should_exit)
{
    const std::string response =
        McpServer::handleLine(line, backend.backend(), kTestServerInfo, should_exit);
    if (response.empty())
    {
        ADD_FAILURE() << "expected a reply for: " << line;
        return nlohmann::json::object();
    }
    return nlohmann::json::parse(response);
}

/// A tool name taken from the compiled-in catalog, so these tests never
/// hard-code a method name the catalog is free to spell differently.
std::string anyCatalogTool()
{
    const std::vector<MethodSpec> &specs = Services::methodSpecs();
    EXPECT_FALSE(specs.empty());
    return specs.empty() ? std::string{} : specs.front().name;
}

// ---------------------------------------------------------------------------
// Version catalog and negotiation (pure functions)
// ---------------------------------------------------------------------------
TEST(McpServerVersions, SupportedListIsModernFirstThenLegacyDescending)
{
    const std::vector<std::string> &supported = McpServer::supportedProtocolVersions();

    ASSERT_EQ(McpServer::modernProtocolVersions().size()
                  + McpServer::legacyProtocolVersions().size(),
              supported.size());
    ASSERT_FALSE(supported.empty());
    EXPECT_EQ(std::string{ "2026-07-28" }, supported.front());
    // Second place is the newest LEGACY version: the list is newest-first
    // throughout, so a client that takes the first entry it recognises lands on
    // the newest version both sides speak.
    EXPECT_EQ(std::string{ "2025-11-25" }, supported[1]);
    // Oldest overall, i.e. the LAST legacy revision: the two era lists are
    // joined newest-first, so the tail is the oldest thing this server speaks.
    EXPECT_EQ(std::string{ "2024-10-07" }, supported.back());
    EXPECT_TRUE(std::is_sorted(supported.begin(), supported.end(),
                               std::greater<std::string>()));
}

TEST(McpServerVersions, LegacyListIsTheFiveSdkRevisionsAscendingNewestLast)
{
    const std::vector<std::string> &legacy = McpServer::legacyProtocolVersions();

    // The exact set AND the exact order are both load-bearing: negotiation
    // echoes a member of this list, the counter-offer cap is its last entry, and
    // supportedProtocolVersions() reverses it to build the wire list. A revision
    // inserted out of chronological place would silently change which version a
    // legacy client is offered, so the expectation is spelled out in full rather
    // than derived from the code under test.
    const std::vector<std::string> expected{
        "2024-10-07",
        "2024-11-05",
        "2025-03-26",
        "2025-06-18",
        "2025-11-25",
    };
    ASSERT_EQ(5u, legacy.size());
    EXPECT_EQ(expected, legacy);
    // Ascending: every neighbour pair is strictly increasing. The identifiers
    // are ISO dates, so lexicographic order IS chronological order — which is
    // how negotiateLegacyVersion() can reason about the list without parsing it.
    for (std::size_t i = 0; i + 1 < legacy.size(); ++i)
    {
        EXPECT_LT(legacy[i], legacy[i + 1]) << "not ascending at index " << i;
    }
    EXPECT_EQ(std::string{ "2024-10-07" }, legacy.front());
    EXPECT_NE(legacy.end(), std::find(legacy.begin(), legacy.end(), "2024-10-07"));
    EXPECT_EQ(std::string{ "2025-11-25" }, legacy.back());
    EXPECT_EQ(std::string{ "2025-11-25" }, McpServer::newestLegacyProtocolVersion());
}

TEST(McpServerVersions, NewestLegacyIsTheCapOfTheLegacyList)
{
    EXPECT_EQ(McpServer::legacyProtocolVersions().back(),
              McpServer::newestLegacyProtocolVersion());
}

TEST(McpServerVersions, IsVersionSupportedSpansBothEras)
{
    for (const std::string &version : McpServer::supportedProtocolVersions())
    {
        EXPECT_TRUE(McpServer::isVersionSupported(version)) << version;
    }
    EXPECT_FALSE(McpServer::isVersionSupported("1999-01-01"));
    EXPECT_FALSE(McpServer::isVersionSupported(""));
    EXPECT_FALSE(McpServer::isVersionSupported("2025-06-1"));
}

TEST(McpServerVersions, NegotiationEchoesSupportedLegacyAndCountersOtherwise)
{
    for (const std::string &version : McpServer::legacyProtocolVersions())
    {
        EXPECT_EQ(version, McpServer::negotiateLegacyVersion(version));
    }
    EXPECT_EQ(McpServer::newestLegacyProtocolVersion(),
              McpServer::negotiateLegacyVersion("1999-01-01"));
    EXPECT_EQ(McpServer::newestLegacyProtocolVersion(),
              McpServer::negotiateLegacyVersion(""));
    // A modern version must not be echoed back into a legacy handshake.
    EXPECT_EQ(McpServer::newestLegacyProtocolVersion(),
              McpServer::negotiateLegacyVersion("2026-07-28"));
}

TEST(McpServerVersions, DeclaredVersionReaderReadsOnlyMeta)
{
    EXPECT_EQ(std::string{}, McpServer::declaredProtocolVersion(nlohmann::json::object()));
    EXPECT_EQ(std::string{ "2026-07-28" },
              McpServer::declaredProtocolVersion(paramsWithDeclaredVersion("2026-07-28")));

    // `protocolVersion` at the params level is the LEGACY declaration and must
    // not be mistaken for modern `_meta` metadata.
    EXPECT_EQ(std::string{},
              McpServer::declaredProtocolVersion(
                  nlohmann::json{ { "protocolVersion", "2025-06-18" } }));

    // Malformed metadata reads as "nothing declared" rather than as a fault.
    EXPECT_EQ(std::string{}, McpServer::declaredProtocolVersion(
                                 nlohmann::json{ { "_meta", 7 } }));
    EXPECT_EQ(std::string{}, McpServer::declaredProtocolVersion(
                                 nlohmann::json{ { "_meta", nlohmann::json::object() } }));
    EXPECT_EQ(std::string{},
              McpServer::declaredProtocolVersion(nlohmann::json{
                  { "_meta", nlohmann::json{ { kProtocolVersionMetaKey, 7 } } } }));

    // JSON-RPC also allows positional params; an array is not fatal here.
    EXPECT_EQ(std::string{}, McpServer::declaredProtocolVersion(
                                 nlohmann::json::array({ 1, 2 })));
}

// ---------------------------------------------------------------------------
// Legacy era: initialize
// ---------------------------------------------------------------------------
TEST(McpServerLegacyHandshake, InitializeEchoesEachSupportedLegacyVersion)
{
    FakeBackend backend;
    bool should_exit = false;

    for (const std::string &version : McpServer::legacyProtocolVersions())
    {
        const nlohmann::json reply = exchange(
            requestLine("initialize", nlohmann::json{ { "protocolVersion", version } }),
            backend, should_exit);

        ASSERT_TRUE(reply.contains("result")) << version << ": " << reply.dump();
        EXPECT_EQ(version, reply["result"]["protocolVersion"].get<std::string>());
        EXPECT_FALSE(should_exit);
    }
}

TEST(McpServerLegacyHandshake, InitializeCounterOffersNewestLegacyForUnknownVersion)
{
    FakeBackend backend;
    bool should_exit = false;

    const nlohmann::json reply = exchange(
        requestLine("initialize", nlohmann::json{ { "protocolVersion", "1999-01-01" } }),
        backend, should_exit);

    // A counter-offer is not an error: the client decides whether it can speak
    // the version we offer.
    ASSERT_TRUE(reply.contains("result")) << reply.dump();
    EXPECT_EQ(McpServer::newestLegacyProtocolVersion(),
              reply["result"]["protocolVersion"].get<std::string>());
}

TEST(McpServerLegacyHandshake, InitializeNeverAnswersWithAModernVersion)
{
    // The failure this prevents: a legacy client accepts a handshake that names
    // 2026-07-28 and then cannot parse the modern session shape that follows.
    const std::vector<std::string> requests{
        "2999-01-01", "2026-07-28", "",
    };
    for (const std::string &requested : requests)
    {
        FakeBackend backend;
        bool should_exit = false;

        const nlohmann::json reply = exchange(
            requestLine("initialize", nlohmann::json{ { "protocolVersion", requested } }),
            backend, should_exit);

        ASSERT_TRUE(reply.contains("result")) << requested << ": " << reply.dump();
        const std::string agreed = reply["result"]["protocolVersion"].get<std::string>();
        for (const std::string &modern : McpServer::modernProtocolVersions())
        {
            EXPECT_NE(modern, agreed) << "legacy handshake answered with " << agreed;
        }
    }
}

TEST(McpServerLegacyHandshake, InitializeEchoesServerInfoAndCapabilities)
{
    FakeBackend backend;
    bool should_exit = false;

    const nlohmann::json reply = exchange(requestLine("initialize"), backend, should_exit);

    ASSERT_TRUE(reply.contains("result")) << reply.dump();
    EXPECT_EQ(kTestServerInfo.name,
              reply["result"]["serverInfo"]["name"].get<std::string>());
    EXPECT_EQ(kTestServerInfo.version,
              reply["result"]["serverInfo"]["version"].get<std::string>());
    // listChanged is false because the catalog is compiled-in data: it cannot
    // change while the process runs.
    EXPECT_EQ(false, reply["result"]["capabilities"]["tools"]["listChanged"].get<bool>());
}

// ---------------------------------------------------------------------------
// Modern era: server/discover
// ---------------------------------------------------------------------------
TEST(McpServerModernEra, DiscoverListsBothErasWithoutAHandshake)
{
    FakeBackend backend;
    bool should_exit = false;

    // No `initialize` first — serving this is the whole point of the modern
    // path, and it must work on a cold process.
    const nlohmann::json reply = exchange(
        requestLine("server/discover", paramsWithDeclaredVersion("2026-07-28")),
        backend, should_exit);

    ASSERT_TRUE(reply.contains("result")) << reply.dump();
    const nlohmann::json &result = reply["result"];

    // The key is `supportedVersions` — the reference schema's exact spelling,
    // asserted by presence AND by absence. The absent half is the load-bearing
    // one: a reply that still carried the old `protocolVersions` name would make
    // a modern client classify this server as legacy, silently and with no
    // diagnostic, so the old spelling must not be able to creep back in beside
    // the new one where a client might pick it up instead.
    ASSERT_TRUE(result.contains("supportedVersions")) << result.dump();
    ASSERT_TRUE(result["supportedVersions"].is_array()) << result.dump();
    EXPECT_FALSE(result.contains("protocolVersions")) << result.dump();
    EXPECT_FALSE(result.contains("supportedProtocolVersions")) << result.dump();

    const std::vector<std::string> listed =
        result["supportedVersions"].get<std::vector<std::string>>();
    EXPECT_NE(listed.end(), std::find(listed.begin(), listed.end(), "2026-07-28"));
    EXPECT_NE(listed.end(), std::find(listed.begin(), listed.end(), "2025-11-25"));

    // Cacheability is stated rather than defaulted. ttlMs 0 means "do not reuse
    // this reply", and cacheScope "private" means "this answer belongs to this
    // client's connection to this build" — both must be present with exactly
    // those values, because a client defaulting them differently is precisely
    // the case the explicit values exist to prevent.
    ASSERT_TRUE(result.contains("ttlMs")) << result.dump();
    EXPECT_EQ(0, result["ttlMs"].get<int>());
    ASSERT_TRUE(result.contains("cacheScope")) << result.dump();
    EXPECT_EQ("private", result["cacheScope"].get<std::string>());

    // `instructions` is optional and this server omits it.
    EXPECT_FALSE(result.contains("instructions")) << result.dump();

    EXPECT_EQ(kTestServerInfo.name,
              result["serverInfo"]["name"].get<std::string>());
    EXPECT_TRUE(result["capabilities"]["tools"].contains("listChanged"));
}

TEST(McpServerModernEra, DiscoverIsServedAsTheVeryFirstMessageOnAColdProcess)
{
    FakeBackend backend;

    // Same claim as above, driven through run() rather than handleLine(): the
    // host's real sequence is "write the probe, block on the reply", with no
    // initialize before it and no session established by anything else.
    std::istringstream input{
        requestLine("server/discover", paramsWithDeclaredVersion("2026-07-28"), 1) + "\n"
    };
    std::ostringstream output;

    McpServer server{ backend.backend(), kTestServerInfo };
    server.run(input, output);

    const std::string produced = output.str();
    ASSERT_FALSE(produced.empty());
    EXPECT_EQ(1, static_cast<int>(std::count(produced.begin(), produced.end(), '\n')));
    const nlohmann::json reply = nlohmann::json::parse(produced);
    EXPECT_EQ(1, reply["id"].get<int>());
    ASSERT_TRUE(reply.contains("result")) << produced;
    EXPECT_TRUE(reply["result"].contains("supportedVersions")) << produced;

    // Discovery is a probe, not a tool call: it must never touch the daemon.
    EXPECT_EQ(0, backend.calls);
}

TEST(McpServerModernEra, DiscoverAndUnsupportedPayloadsShareOneVersionList)
{
    const nlohmann::json discover = McpServer::discoverResult(kTestServerInfo);
    const nlohmann::json unsupported = McpServer::unsupportedVersionData("1999-01-01");

    // One catalog feeds both replies. If they ever disagree, a client that
    // discovered a version would be told a moment later that the same version is
    // not supported, and the negotiation retry would never converge — the worst
    // possible outcome, because both replies look individually correct.
    EXPECT_EQ(McpServer::supportedProtocolVersions(),
              discover["supportedVersions"].get<std::vector<std::string>>());
    EXPECT_EQ(McpServer::supportedProtocolVersions(),
              unsupported["supported"].get<std::vector<std::string>>());
}

TEST(McpServerModernEra, EveryModernResultIsStampedWithAResultType)
{
    // Revision 2026-07-28 requires every result to carry `resultType`, and the
    // "absent means complete" bridge is granted only to earlier-revision
    // servers. This was found the hard way: a live modern client parsed our
    // discover reply, accepted the version list, then rejected tools/list with
    // "missing required resultType" — so the model was handed no tools at all
    // while the server still looked connected. That silent half-state, where
    // negotiation succeeds and the catalog never arrives, is what this test
    // exists to prevent.
    FakeBackend backend;
    bool should_exit = false;

    const nlohmann::json discover = exchange(
        requestLine("server/discover", paramsWithDeclaredVersion("2026-07-28")),
        backend, should_exit);
    ASSERT_TRUE(discover.contains("result")) << discover.dump();
    EXPECT_EQ("complete", discover["result"].value("resultType", ""));

    const nlohmann::json tools = exchange(
        requestLine("tools/list", paramsWithDeclaredVersion("2026-07-28")),
        backend, should_exit);
    ASSERT_TRUE(tools.contains("result")) << tools.dump();
    EXPECT_EQ("complete", tools["result"].value("resultType", ""));
    // The 2026-07-28 schema for this result declares both of these explicitly.
    EXPECT_EQ(0, tools["result"].value("ttlMs", -1));
    EXPECT_EQ("private", tools["result"].value("cacheScope", ""));

    nlohmann::json call_params = paramsWithDeclaredVersion(
        "2026-07-28",
        nlohmann::json{ { "name", anyCatalogTool() },
                        { "arguments", nlohmann::json::object() } });
    backend.setReply(nlohmann::json{ { "ok", true } });
    const nlohmann::json called = exchange(requestLine("tools/call", call_params),
                                           backend, should_exit);
    ASSERT_TRUE(called.contains("result")) << called.dump();
    EXPECT_EQ("complete", called["result"].value("resultType", ""));

    // A tool-level failure is still a RESULT (isError:true), so it is stamped
    // too. An error reply is not, and the assertions below pin that difference.
    backend.setFailure(Error::notFound("no task with id 7"));
    const nlohmann::json failed = exchange(requestLine("tools/call", call_params),
                                           backend, should_exit);
    ASSERT_TRUE(failed.contains("result")) << failed.dump();
    EXPECT_TRUE(failed["result"].value("isError", false));
    EXPECT_EQ("complete", failed["result"].value("resultType", ""));
}

TEST(McpServerLegacyHandshake, HandshakeEraResultsAreNotStampedWithAResultType)
{
    // No `_meta` means a handshake-era client, whose result schemas predate
    // resultType. Stamping it there would describe an era the client never
    // asked for, and the field is meaningless to it.
    FakeBackend backend;
    bool should_exit = false;

    const nlohmann::json tools =
        exchange(requestLine("tools/list"), backend, should_exit);
    ASSERT_TRUE(tools.contains("result")) << tools.dump();
    EXPECT_FALSE(tools["result"].contains("resultType")) << tools.dump();

    const nlohmann::json init = exchange(
        requestLine("initialize", nlohmann::json{ { "protocolVersion", "2025-06-18" } }),
        backend, should_exit);
    ASSERT_TRUE(init.contains("result")) << init.dump();
    EXPECT_FALSE(init["result"].contains("resultType")) << init.dump();

    // An error reply carries no result, so there is nothing to stamp — and
    // stamping into an envelope that has no `result` would be the bug the
    // guard in the respond funnel exists to avoid.
    const nlohmann::json rejected = exchange(
        requestLine("tools/list",
                    paramsWithDeclaredVersion("1999-01-01")),
        backend, should_exit);
    ASSERT_TRUE(rejected.contains("error")) << rejected.dump();
    EXPECT_EQ(-32022, rejected["error"].value("code", 0));
}

// ---------------------------------------------------------------------------
// tools/list
// ---------------------------------------------------------------------------
TEST(McpServerTools, ListWorksWithoutAHandshakeAndMatchesTheCatalog)
{
    FakeBackend backend;
    bool should_exit = false;

    const nlohmann::json reply = exchange(requestLine("tools/list"), backend, should_exit);

    ASSERT_TRUE(reply.contains("result")) << reply.dump();
    const nlohmann::json &tools = reply["result"]["tools"];
    ASSERT_TRUE(tools.is_array());
    EXPECT_FALSE(tools.empty());

    const std::vector<MethodSpec> &specs = Services::methodSpecs();
    ASSERT_EQ(specs.size(), tools.size());
    for (std::size_t i = 0; i < specs.size(); ++i)
    {
        // One entry per MethodSpec, name and description verbatim...
        EXPECT_EQ(specs[i].name, tools[i]["name"].get<std::string>());
        EXPECT_EQ(specs[i].description, tools[i]["description"].get<std::string>());
        // ...and an object schema, which is what an MCP host requires before it
        // will offer the tool to a model at all.
        ASSERT_TRUE(tools[i]["inputSchema"].is_object()) << specs[i].name;
        EXPECT_EQ("object", tools[i]["inputSchema"]["type"].get<std::string>())
            << specs[i].name;
    }
}

TEST(McpServerTools, DefinitionsAreDerivedFromTheMethodCatalog)
{
    const nlohmann::json tools = McpServer::toolDefinitions();
    const std::vector<MethodSpec> &specs = Services::methodSpecs();

    ASSERT_TRUE(tools.is_array());
    EXPECT_FALSE(tools.empty());
    ASSERT_EQ(specs.size(), tools.size());
    for (std::size_t i = 0; i < specs.size(); ++i)
    {
        EXPECT_EQ(specs[i].name, tools[i]["name"].get<std::string>());
        EXPECT_EQ("object", tools[i]["inputSchema"]["type"].get<std::string>());
    }
}

// ---------------------------------------------------------------------------
// tools/call
// ---------------------------------------------------------------------------
TEST(McpServerTools, CallWrapsSuccessAsTextContent)
{
    FakeBackend backend;
    const nlohmann::json payload{ { "id", 7 }, { "title", "write the report" } };
    backend.setReply(payload);
    bool should_exit = false;

    const nlohmann::json reply = exchange(
        requestLine("tools/call",
                    nlohmann::json{ { "name", anyCatalogTool() },
                                    { "arguments", nlohmann::json{ { "id", 7 } } } }),
        backend, should_exit);

    ASSERT_TRUE(reply.contains("result")) << reply.dump();
    EXPECT_FALSE(reply.contains("error"));
    EXPECT_EQ(false, reply["result"]["isError"].get<bool>());

    ASSERT_EQ(1u, reply["result"]["content"].size());
    EXPECT_EQ("text", reply["result"]["content"][0]["type"].get<std::string>());

    // The text is the pretty-printed payload, so it round-trips back to what the
    // daemon answered — the guarantee a model relies on when it reads the text
    // rather than a typed value.
    const std::string text = reply["result"]["content"][0]["text"].get<std::string>();
    EXPECT_EQ(payload, nlohmann::json::parse(text));

    // And the forward was faithful: same tool name, same arguments.
    EXPECT_EQ(anyCatalogTool(), backend.last_method);
    EXPECT_EQ(7, backend.last_params["id"].get<int>());
    EXPECT_EQ(1, backend.calls);
}

TEST(McpServerTools, CallWrapsFailureAsIsErrorContent)
{
    FakeBackend backend;
    backend.setFailure(Error::notFound("task 7"));
    bool should_exit = false;

    const nlohmann::json reply = exchange(
        requestLine("tools/call", nlohmann::json{ { "name", anyCatalogTool() } }),
        backend, should_exit);

    // A tool that fails is a SUCCESSFUL JSON-RPC call reporting isError:true.
    // That is the whole reason the model can read the message and correct itself
    // instead of seeing an opaque protocol fault it cannot act on.
    ASSERT_TRUE(reply.contains("result")) << reply.dump();
    EXPECT_FALSE(reply.contains("error"));
    EXPECT_TRUE(reply["result"]["isError"].get<bool>());

    ASSERT_EQ(1u, reply["result"]["content"].size());
    const std::string text = reply["result"]["content"][0]["text"].get<std::string>();

    // The code is the stable NAME, matching the vocabulary JsonRpcServer puts in
    // error.data.code. The ordinal it replaces ("code=1") tells a model nothing
    // about whether to fix its arguments or report a system fault — and a model
    // is the reader of this text, so an unreadable category is a real defect.
    EXPECT_NE(std::string::npos, text.find("error (not_found):")) << text;
    EXPECT_NE(std::string::npos, text.find("task 7")) << text;
    // The old bare-ordinal form ("code=1", kNotFound's ordinal) is gone, and
    // nothing else in the message may reintroduce the token.
    EXPECT_EQ(std::string::npos, text.find("code=1")) << text;
    EXPECT_EQ(std::string::npos, text.find("code=")) << text;
}

TEST(McpServerTools, FailureTextNamesTheErrorCodeForEveryCategory)
{
    FakeBackend backend;
    bool should_exit = false;

    // One case per category a model must act on differently: a bad argument
    // means "fix the call", a missing entity means "the world changed", a
    // conflict means "someone else moved first", and a storage or internal fault
    // means "do not retry this, report it". A shared ordinal collapses all of
    // those into the same opaque token.
    const std::vector<std::pair<Error, std::string>> cases{
        { Error::notFound("task 7"), "not_found" },
        { Error::invalidArgument("priority must be 1..5"), "invalid_argument" },
        { Error::conflict("task 7 is already complete"), "conflict" },
        { Error::storageFailure("database is locked"), "storage_failure" },
        { Error::internal("unreachable ranking branch"), "internal" },
    };
    for (const auto &[failure, expected_name] : cases)
    {
        backend.setFailure(failure);

        const nlohmann::json reply = exchange(
            requestLine("tools/call", nlohmann::json{ { "name", anyCatalogTool() } }),
            backend, should_exit);

        ASSERT_TRUE(reply.contains("result")) << reply.dump();
        ASSERT_TRUE(reply["result"]["isError"].get<bool>()) << reply.dump();
        const std::string text = reply["result"]["content"][0]["text"].get<std::string>();
        EXPECT_NE(std::string::npos, text.find("error (" + expected_name + "):")) << text;
        EXPECT_NE(std::string::npos, text.find(failure.message)) << text;
        EXPECT_EQ(std::string::npos, text.find("code=")) << text;
    }
}

TEST(McpServerTools, CallRejectsMissingOrNonStringName)
{
    FakeBackend backend;
    bool should_exit = false;

    const std::vector<nlohmann::json> bad_params{
        nlohmann::json::object(),
        nlohmann::json{ { "arguments", nlohmann::json::object() } },
        nlohmann::json{ { "name", 7 } },
        nlohmann::json{ { "name", nullptr } },
        nlohmann::json::array({ "positional", "params" }),
    };
    for (const nlohmann::json &params : bad_params)
    {
        const nlohmann::json reply = exchange(requestLine("tools/call", params),
                                              backend, should_exit);

        ASSERT_TRUE(reply.contains("error")) << reply.dump();
        EXPECT_EQ(-32602, reply["error"]["code"].get<int>()) << reply.dump();
        EXPECT_FALSE(reply.contains("result"));
    }
    // Nothing malformed ever reached the daemon.
    EXPECT_EQ(0, backend.calls);
}

TEST(McpServerTools, CallRejectsUnknownToolByName)
{
    FakeBackend backend;
    bool should_exit = false;

    const nlohmann::json reply = exchange(
        requestLine("tools/call",
                    nlohmann::json{ { "name", "definitely_not_a_tool" } }),
        backend, should_exit);

    ASSERT_TRUE(reply.contains("error")) << reply.dump();
    EXPECT_EQ(-32602, reply["error"]["code"].get<int>());
    // The message names the offending tool: the MCP layer owns the vocabulary,
    // so it can be more specific than the daemon's generic "no such method".
    const std::string message = reply["error"]["message"].get<std::string>();
    EXPECT_NE(std::string::npos,
              message.find("Unknown tool: definitely_not_a_tool"))
        << message;
    EXPECT_EQ(0, backend.calls);
}

TEST(McpServerTools, CallNormalisesNullArgumentsToAnEmptyObject)
{
    FakeBackend backend;
    bool should_exit = false;

    exchange(requestLine("tools/call",
                         nlohmann::json{ { "name", anyCatalogTool() },
                                         { "arguments", nullptr } }),
             backend, should_exit);

    ASSERT_EQ(1, backend.calls);
    EXPECT_TRUE(backend.last_params.is_object());
    EXPECT_TRUE(backend.last_params.empty());
}

// ---------------------------------------------------------------------------
// Protocol-version gate (-32022)
// ---------------------------------------------------------------------------
TEST(McpServerVersionGate, UnsupportedDeclaredVersionIsRejectedWithTheList)
{
    FakeBackend backend;
    bool should_exit = false;

    const nlohmann::json reply = exchange(
        requestLine("tools/list", paramsWithDeclaredVersion("1999-01-01")),
        backend, should_exit);

    ASSERT_TRUE(reply.contains("error")) << reply.dump();
    EXPECT_EQ(kErrorUnsupportedProtocolVersion, reply["error"]["code"].get<int>());
    EXPECT_FALSE(reply.contains("result"));

    // The data field is the recovery instruction, and it is read by exact key:
    // the SDK's client takes `supported`, picks a version both sides speak, and
    // retries; it takes `requested` to report which version caused the mismatch.
    // If `supported` is unreadable the client abandons negotiation and falls
    // back to `initialize` with no diagnostic at all — which is why both names
    // are asserted, along with the absence of a version list under any other
    // name.
    ASSERT_TRUE(reply["error"].contains("data")) << reply.dump();
    const nlohmann::json &data = reply["error"]["data"];
    ASSERT_TRUE(data.contains("supported")) << data.dump();
    ASSERT_TRUE(data["supported"].is_array()) << data.dump();
    const std::vector<std::string> listed =
        data["supported"].get<std::vector<std::string>>();
    EXPECT_NE(listed.end(), std::find(listed.begin(), listed.end(), "2026-07-28"));
    EXPECT_NE(listed.end(), std::find(listed.begin(), listed.end(), "2025-11-25"));

    ASSERT_TRUE(data.contains("requested")) << data.dump();
    EXPECT_EQ(std::string{ "1999-01-01" }, data["requested"].get<std::string>());

    // Exactly the two keys the client reads. A version list under a second name
    // is not a harmless alias: it is a second contract to keep in sync whose
    // reader does not exist, and the -32022 payload is small enough that its
    // exact size is worth pinning.
    EXPECT_FALSE(data.contains("protocolVersions")) << data.dump();
    EXPECT_FALSE(data.contains("supportedProtocolVersions")) << data.dump();
    EXPECT_EQ(2u, data.size()) << data.dump();
}

TEST(McpServerVersionGate, EverySupportedDeclaredVersionIsAccepted)
{
    for (const std::string &version : McpServer::supportedProtocolVersions())
    {
        FakeBackend backend;
        bool should_exit = false;

        const nlohmann::json reply = exchange(
            requestLine("tools/list", paramsWithDeclaredVersion(version)),
            backend, should_exit);

        ASSERT_TRUE(reply.contains("result")) << version << ": " << reply.dump();
        EXPECT_FALSE(reply.contains("error")) << version;
    }
}

TEST(McpServerVersionGate, RequestWithoutDeclaredVersionIsServed)
{
    // A legacy client declares its version inside `initialize`, not in `_meta`,
    // so the gate must never stand between it and its calls.
    FakeBackend backend;
    bool should_exit = false;

    const nlohmann::json reply = exchange(
        requestLine("tools/list", nlohmann::json{ { "protocolVersion", "2025-06-18" } }),
        backend, should_exit);

    ASSERT_TRUE(reply.contains("result")) << reply.dump();
    EXPECT_FALSE(reply.contains("error"));
}

TEST(McpServerVersionGate, NotificationWithUnsupportedVersionIsDroppedSilently)
{
    FakeBackend backend;
    bool should_exit = false;

    EXPECT_EQ(std::string{},
              McpServer::handleLine(notificationLine("tools/list",
                                                     paramsWithDeclaredVersion("1999-01-01")),
                                    backend.backend(), kTestServerInfo, should_exit));
}

// ---------------------------------------------------------------------------
// Protocol errors
// ---------------------------------------------------------------------------
TEST(McpServerErrors, MalformedJsonIsAParseErrorWithANullId)
{
    FakeBackend backend;
    bool should_exit = false;

    const nlohmann::json reply = exchange("{not json", backend, should_exit);

    ASSERT_TRUE(reply.contains("error")) << reply.dump();
    EXPECT_EQ(-32700, reply["error"]["code"].get<int>());
    EXPECT_TRUE(reply["id"].is_null());
    EXPECT_EQ("2.0", reply["jsonrpc"].get<std::string>());
}

TEST(McpServerErrors, NonObjectOrMethodlessMessagesAreInvalidRequests)
{
    const std::vector<std::string> lines{
        "[1,2,3]",
        "\"just a string\"",
        "42",
        R"({"jsonrpc":"2.0"})",
        R"({"jsonrpc":"2.0","id":1,"method":7})",
        R"({"jsonrpc":"2.0","id":1,"method":null})",
    };
    for (const std::string &line : lines)
    {
        FakeBackend backend;
        bool should_exit = false;

        const nlohmann::json reply = exchange(line, backend, should_exit);

        ASSERT_TRUE(reply.contains("error")) << line;
        EXPECT_EQ(-32600, reply["error"]["code"].get<int>()) << line;
        // No trustworthy id inside a message that is not a request.
        EXPECT_TRUE(reply["id"].is_null()) << line;
    }
}

TEST(McpServerErrors, UnknownMethodIsMethodNotFoundAndEchoesTheId)
{
    FakeBackend backend;
    bool should_exit = false;

    const nlohmann::json reply = exchange(
        requestLine("no_such_method", nlohmann::json::object(), 41), backend, should_exit);

    ASSERT_TRUE(reply.contains("error")) << reply.dump();
    EXPECT_EQ(-32601, reply["error"]["code"].get<int>());
    EXPECT_EQ(41, reply["id"].get<int>());
    EXPECT_EQ(0, backend.calls);
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------
TEST(McpServerNotifications, AreNeverAnswered)
{
    const std::vector<std::string> silent{
        notificationLine("notifications/initialized"),
        notificationLine("notifications/initialized", nullptr),
        notificationLine("tools/list"),
        notificationLine("no_such_method"),
        notificationLine("tools/call", nlohmann::json{ { "name", anyCatalogTool() } }),
        notificationLine("initialize", nlohmann::json{ { "protocolVersion", "2025-06-18" } }),
    };

    for (const std::string &line : silent)
    {
        FakeBackend backend;
        bool should_exit = false;

        // The EXACT empty string, not merely "did not throw": any byte written
        // for a notification desynchronises the host's reply pairing.
        EXPECT_EQ(std::string{},
                  McpServer::handleLine(line, backend.backend(), kTestServerInfo, should_exit))
            << line;
    }
}

TEST(McpServerNotifications, NullIdCountsAsANotification)
{
    FakeBackend backend;
    bool should_exit = false;

    EXPECT_EQ(std::string{},
              McpServer::handleLine(R"({"jsonrpc":"2.0","id":null,"method":"tools/list"})",
                                    backend.backend(), kTestServerInfo, should_exit));
}

TEST(McpServerNotifications, ExitIsSilentAndStopsTheServer)
{
    FakeBackend backend;
    bool should_exit = false;

    EXPECT_EQ(std::string{},
              McpServer::handleLine(notificationLine("notifications/exit"),
                                    backend.backend(), kTestServerInfo, should_exit));
    EXPECT_TRUE(should_exit);
}

// ---------------------------------------------------------------------------
// run(): framing over a stream
// ---------------------------------------------------------------------------
TEST(McpServerRun, WritesExactlyOneLinePerRequestAndNoneForNotifications)
{
    FakeBackend backend;
    bool should_exit = false;

    std::istringstream input{
        requestLine("initialize", nlohmann::json{ { "protocolVersion", "2025-06-18" } }, 1)
            + "\n"
        + notificationLine("notifications/initialized") + "\n"
        + requestLine("tools/list", nlohmann::json::object(), 2) + "\n"
        + requestLine("tools/call", nlohmann::json{ { "name", anyCatalogTool() } }, 3) + "\n"
    };
    std::ostringstream output;

    McpServer server{ backend.backend(), kTestServerInfo };
    server.run(input, output);

    const std::string produced = output.str();
    EXPECT_EQ(3, static_cast<int>(std::count(produced.begin(), produced.end(), '\n')));
    EXPECT_FALSE(should_exit);

    // The replies come back in request order, each correlating with the id it
    // was sent, and only the tools/call reached the daemon.
    std::istringstream produced_stream{ produced };
    std::vector<nlohmann::json> replies;
    std::string line;
    while (std::getline(produced_stream, line))
    {
        replies.push_back(nlohmann::json::parse(line));
    }
    ASSERT_EQ(3u, replies.size());
    EXPECT_EQ(1, replies[0]["id"].get<int>());
    EXPECT_EQ(2, replies[1]["id"].get<int>());
    EXPECT_EQ(3, replies[2]["id"].get<int>());
    EXPECT_EQ(1, backend.calls);
}

TEST(McpServerRun, StopsAtNotificationsExit)
{
    FakeBackend backend;

    std::istringstream input{
        requestLine("tools/list", nlohmann::json::object(), 1) + "\n"
        + notificationLine("notifications/exit") + "\n"
        + requestLine("tools/list", nlohmann::json::object(), 2) + "\n"
    };
    std::ostringstream output;

    McpServer server{ backend.backend(), kTestServerInfo };
    server.run(input, output);

    // The message after `exit` was never read, and exit itself wrote nothing.
    const std::string produced = output.str();
    EXPECT_EQ(1, static_cast<int>(std::count(produced.begin(), produced.end(), '\n')));
    EXPECT_NE(std::string::npos, produced.find("\"id\":1"));
    EXPECT_EQ(std::string::npos, produced.find("\"id\":2"));
}

TEST(McpServerRun, SkipsBlankLinesAndAcceptsCrlfFraming)
{
    FakeBackend backend;

    std::istringstream input{
        "\n"
        "   \n"
        + requestLine("tools/list", nlohmann::json::object(), 7) + "\r\n"
        + "\n"
    };
    std::ostringstream output;

    McpServer server{ backend.backend(), kTestServerInfo };
    server.run(input, output);

    // Blank lines are not requests, and a CR must not poison an otherwise valid
    // message: exactly one line out for the one real request.
    const std::string produced = output.str();
    EXPECT_EQ(1, static_cast<int>(std::count(produced.begin(), produced.end(), '\n')));
    ASSERT_FALSE(produced.empty());
    EXPECT_EQ(7, nlohmann::json::parse(produced)["id"].get<int>());
}

TEST(McpServerFraming, ResponseIsOneLineEvenWhenTheTextIsMultiLine)
{
    FakeBackend backend;
    backend.setReply(nlohmann::json{
        { "note", "line one\nline two" },
        { "nested", nlohmann::json{ { "list", nlohmann::json::array({ 1, 2, 3 }) } } },
    });
    bool should_exit = false;

    const std::string response = McpServer::handleLine(
        requestLine("tools/call", nlohmann::json{ { "name", anyCatalogTool() } }),
        backend.backend(), kTestServerInfo, should_exit);

    // The payload is pretty-printed (multi-line) text INSIDE a JSON string, and
    // still the whole envelope occupies one line: one raw newline anywhere in it
    // would split a single response into two malformed lines and the host would
    // drop the server. handleLine also adds no trailing newline, so run() owns
    // the exactly-one-per-response framing.
    EXPECT_EQ(std::string::npos, response.find('\n'));
    EXPECT_EQ(std::string::npos, response.find('\r'));
    ASSERT_FALSE(response.empty());
    EXPECT_NE('\n', response.back());

    const nlohmann::json reply = nlohmann::json::parse(response);
    const std::string text = reply["result"]["content"][0]["text"].get<std::string>();
    // The internal newline survived as an escape rather than being lost.
    EXPECT_NE(std::string::npos, text.find("line two"));
    EXPECT_NE(std::string::npos, text.find('\n'));
}

} // namespace
} // namespace taskpilot
