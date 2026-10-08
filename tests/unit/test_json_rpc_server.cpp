// test_json_rpc_server.cpp — unit tests for the JSON-RPC dispatch layer
//
// Only dispatchLine() is exercised here: it is the whole protocol, and it is
// static and socket-free on purpose so every branch can be asserted without a
// listener, a port, or a thread. Real socket I/O — accept, framing, session
// reaping, stop() — belongs to tests/integration/test_daemon_roundtrip.cpp.
//
// The one exception is the session-bookkeeping contract, asserted at the bottom
// of this file: sessionCount() and stop() need no socket, and the connection-
// leak check in the integration suite is written against exactly that contract.
//
// Each test parses the returned line as JSON rather than comparing strings, so
// a change in key order (nlohmann emits keys sorted) can never fail a test
// that is really about protocol behaviour.

#include "control/JsonRpcServer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

using namespace taskpilot;

namespace
{

/// Registry used by the tests that are not about one specific handler:
///   echo    — returns its params, so a test can see what the handler received
///   fail    — reports a domain failure (an unknown task)
///   thrower — a buggy handler
MethodRegistry makeTestRegistry()
{
    MethodRegistry registry;
    registry["echo"] = [](const nlohmann::json &params) -> RpcResult
    {
        return params;
    };
    registry["fail"] = [](const nlohmann::json &) -> RpcResult
    {
        return Error::notFound("task 42 does not exist");
    };
    registry["thrower"] = [](const nlohmann::json &) -> RpcResult
    {
        throw std::runtime_error("boom");
    };
    return registry;
}

/// Build a request line with `id` and `method` substituted in, so a test reads
/// as the bytes a client actually puts on the wire.
std::string requestLine(const std::string &id, const std::string &method)
{
    return R"({"jsonrpc":"2.0","id":)" + id + R"(,"method":")" + method
           + R"(","params":{}})";
}

} // namespace

// ---------------------------------------------------------------------------
// Protocol-level errors
// ---------------------------------------------------------------------------
TEST(JsonRpcServer, ParseErrorIsReportedWithNullId)
{
    const MethodRegistry registry = makeTestRegistry();
    const std::string line = JsonRpcServer::dispatchLine("{not json", registry);

    ASSERT_FALSE(line.empty());
    const nlohmann::json response = nlohmann::json::parse(line);
    EXPECT_EQ(std::string("2.0"), response["jsonrpc"].get<std::string>());
    EXPECT_TRUE(response["id"].is_null());
    EXPECT_EQ(-32700, response["error"]["code"]);
    EXPECT_FALSE(response["error"]["message"].get<std::string>().empty());
    EXPECT_FALSE(response.contains("result"));
}

TEST(JsonRpcServer, NonObjectEnvelopeIsAnInvalidRequest)
{
    const MethodRegistry registry = makeTestRegistry();

    // A batch array, a bare scalar, and a string are all valid JSON and none of
    // them is a request object.
    for (const std::string &raw : { std::string("[1,2]"), std::string("42"),
                                    std::string("\"echo\"") })
    {
        const std::string line = JsonRpcServer::dispatchLine(raw, registry);
        ASSERT_FALSE(line.empty()) << "input: " << raw;
        const nlohmann::json response = nlohmann::json::parse(line);
        EXPECT_TRUE(response["id"].is_null()) << "input: " << raw;
        EXPECT_EQ(-32600, response["error"]["code"]) << "input: " << raw;
        EXPECT_FALSE(response.contains("result")) << "input: " << raw;
    }
}

TEST(JsonRpcServer, MissingMethodIsAnInvalidRequest)
{
    const MethodRegistry registry = makeTestRegistry();
    const std::string line =
        JsonRpcServer::dispatchLine(R"({"jsonrpc":"2.0","id":1})", registry);

    const nlohmann::json response = nlohmann::json::parse(line);
    EXPECT_EQ(-32600, response["error"]["code"]);
    EXPECT_TRUE(response["id"].is_null());
}

TEST(JsonRpcServer, NonStringMethodIsAnInvalidRequest)
{
    const MethodRegistry registry = makeTestRegistry();
    const std::string line =
        JsonRpcServer::dispatchLine(R"({"jsonrpc":"2.0","id":1,"method":7})", registry);

    const nlohmann::json response = nlohmann::json::parse(line);
    EXPECT_EQ(-32600, response["error"]["code"]);
    // The envelope is broken, so the response cannot be matched to the id the
    // client sent: JSON-RPC requires null rather than a guessed echo.
    EXPECT_TRUE(response["id"].is_null());
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------
TEST(JsonRpcServer, RequestWithoutIdIsANotificationAndIsNotDispatched)
{
    bool handlerCalled = false;
    MethodRegistry registry;
    registry["echo"] = [&handlerCalled](const nlohmann::json &params) -> RpcResult
    {
        handlerCalled = true;
        return params;
    };

    const std::string line = JsonRpcServer::dispatchLine(
        R"({"jsonrpc":"2.0","method":"echo","params":{"a":1}})", registry);

    EXPECT_TRUE(line.empty());
    EXPECT_FALSE(handlerCalled);
}

TEST(JsonRpcServer, NullIdIsANotificationAndIsNotDispatched)
{
    bool handlerCalled = false;
    MethodRegistry registry;
    registry["echo"] = [&handlerCalled](const nlohmann::json &params) -> RpcResult
    {
        handlerCalled = true;
        return params;
    };

    const std::string line = JsonRpcServer::dispatchLine(
        R"({"jsonrpc":"2.0","id":null,"method":"echo","params":{}})", registry);

    EXPECT_TRUE(line.empty());
    EXPECT_FALSE(handlerCalled);
}

TEST(JsonRpcServer, UnknownMethodInANotificationIsSilent)
{
    // A notification is never answered, so an unknown method cannot be
    // reported either — there is no response to carry the -32601.
    const MethodRegistry registry = makeTestRegistry();
    const std::string line = JsonRpcServer::dispatchLine(
        R"({"jsonrpc":"2.0","method":"no_such_method","params":{}})", registry);

    EXPECT_TRUE(line.empty());
}

// ---------------------------------------------------------------------------
// Method lookup and dispatch
// ---------------------------------------------------------------------------
TEST(JsonRpcServer, UnknownMethodIsReportedAndTheIdIsEchoed)
{
    const MethodRegistry registry = makeTestRegistry();
    const std::string line =
        JsonRpcServer::dispatchLine(requestLine("11", "no_such_method"), registry);

    const nlohmann::json response = nlohmann::json::parse(line);
    EXPECT_EQ(-32601, response["error"]["code"]);
    EXPECT_EQ(11, response["id"]);
    EXPECT_FALSE(response.contains("result"));
}

TEST(JsonRpcServer, ParamsReachTheHandler)
{
    const MethodRegistry registry = makeTestRegistry();
    const std::string line = JsonRpcServer::dispatchLine(
        R"({"jsonrpc":"2.0","id":1,"method":"echo","params":{"x":[1,2,3]}})", registry);

    const nlohmann::json response = nlohmann::json::parse(line);
    ASSERT_TRUE(response["result"].is_object());
    EXPECT_EQ(3U, response["result"]["x"].size());
    EXPECT_EQ(3, response["result"]["x"][2]);
}

TEST(JsonRpcServer, AbsentParamsBecomeAnEmptyObject)
{
    // Every handler in the catalog can accept an empty object, so a request
    // that omits params must not reach a handler with a null value.
    const MethodRegistry registry = makeTestRegistry();
    const std::string line =
        JsonRpcServer::dispatchLine(R"({"jsonrpc":"2.0","id":1,"method":"echo"})", registry);

    const nlohmann::json response = nlohmann::json::parse(line);
    ASSERT_TRUE(response["result"].is_object());
    EXPECT_TRUE(response["result"].empty());
    EXPECT_FALSE(response.contains("error"));
}

// ---------------------------------------------------------------------------
// The request id round-trips exactly
// ---------------------------------------------------------------------------
TEST(JsonRpcServer, NumericIdRoundTripsAsAnInteger)
{
    const MethodRegistry registry = makeTestRegistry();

    // 2^53 + 1 is the first integer a double cannot represent, so it fails
    // loudly if the id is ever routed through a floating-point value.
    const std::string line = JsonRpcServer::dispatchLine(
        requestLine("9007199254740993", "echo"), registry);

    const nlohmann::json response = nlohmann::json::parse(line);
    EXPECT_TRUE(response["id"].is_number_integer());
    EXPECT_EQ(std::int64_t{ 9007199254740993 }, response["id"].get<std::int64_t>());
}

TEST(JsonRpcServer, StringIdRoundTripsUnchanged)
{
    const MethodRegistry registry = makeTestRegistry();
    const std::string line =
        JsonRpcServer::dispatchLine(requestLine(R"("task-42")", "echo"), registry);

    const nlohmann::json response = nlohmann::json::parse(line);
    EXPECT_TRUE(response["id"].is_string());
    EXPECT_EQ(std::string("task-42"), response["id"].get<std::string>());
}

TEST(JsonRpcServer, IdIsEchoedVerbatimOnTheErrorPath)
{
    // The id has to survive a failure too, or a client that pipelines several
    // requests cannot tell which one failed.
    const MethodRegistry registry = makeTestRegistry();
    const std::string line =
        JsonRpcServer::dispatchLine(requestLine(R"("abc")", "fail"), registry);

    const nlohmann::json response = nlohmann::json::parse(line);
    EXPECT_EQ(std::string("abc"), response["id"].get<std::string>());
    EXPECT_EQ(-32001, response["error"]["code"]);
}

// ---------------------------------------------------------------------------
// Error mapping
// ---------------------------------------------------------------------------
TEST(JsonRpcServer, DomainErrorBecomesAnApplicationCodeWithAStableName)
{
    const MethodRegistry registry = makeTestRegistry();
    const std::string line = JsonRpcServer::dispatchLine(requestLine("7", "fail"), registry);

    const nlohmann::json response = nlohmann::json::parse(line);
    EXPECT_EQ(7, response["id"]);
    // An unknown task is an application failure, not a transport failure: it
    // must land in the -32000..-32099 range, never in -32603/-32700.
    EXPECT_EQ(rpcErrorCode(ErrorCode::kNotFound), response["error"]["code"]);
    EXPECT_EQ(-32001, response["error"]["code"]);
    EXPECT_EQ(std::string("not_found"), response["error"]["data"]["code"].get<std::string>());
    EXPECT_EQ(std::string("task 42 does not exist"),
              response["error"]["message"].get<std::string>());
    // The message is repeated inside data so a client that only surfaces
    // `data` still shows something a human can act on.
    EXPECT_EQ(std::string("task 42 does not exist"),
              response["error"]["data"]["message"].get<std::string>());
    EXPECT_FALSE(response.contains("result"));
}

TEST(JsonRpcServer, ThrowingHandlerIsReportedInsteadOfKillingTheSession)
{
    const MethodRegistry registry = makeTestRegistry();
    const std::string line = JsonRpcServer::dispatchLine(requestLine("3", "thrower"), registry);

    const nlohmann::json response = nlohmann::json::parse(line);
    EXPECT_EQ(3, response["id"]);
    EXPECT_TRUE(response.contains("error"));
    EXPECT_TRUE(response["error"]["data"]["code"].is_string());
    EXPECT_FALSE(response["error"]["data"]["code"].get<std::string>().empty());
    EXPECT_FALSE(response.contains("result"));
}

// ---------------------------------------------------------------------------
// Result fidelity and framing
// ---------------------------------------------------------------------------
TEST(JsonRpcServer, NestedResultRoundTripsIntact)
{
    MethodRegistry registry;
    registry["nested"] = [](const nlohmann::json &) -> RpcResult
    {
        return nlohmann::json{
            { "tasks",
              nlohmann::json::array(
                  { nlohmann::json{ { "id", 1 },
                                    { "title", "fix the \"ws\" bug" },
                                    { "tags", nlohmann::json::array({ "net", "urgent" }) },
                                    { "score", 1.5 } },
                    nlohmann::json{ { "id", 2 },
                                    { "title", "second" },
                                    { "tags", nlohmann::json::array() },
                                    { "score", -0.25 } } }) },
            { "count", 2 },
            { "empty", nullptr },
            { "deep", nlohmann::json{ { "deeper", true } } },
        };
    };

    const std::string line = JsonRpcServer::dispatchLine(requestLine("1", "nested"), registry);
    const nlohmann::json response = nlohmann::json::parse(line);

    ASSERT_TRUE(response["result"].is_object());
    EXPECT_EQ(2U, response["result"]["tasks"].size());
    EXPECT_EQ(std::string("fix the \"ws\" bug"),
              response["result"]["tasks"][0]["title"].get<std::string>());
    EXPECT_EQ(2U, response["result"]["tasks"][0]["tags"].size());
    EXPECT_DOUBLE_EQ(1.5, response["result"]["tasks"][0]["score"].get<double>());
    EXPECT_TRUE(response["result"]["tasks"][1]["tags"].empty());
    EXPECT_TRUE(response["result"]["empty"].is_null());
    EXPECT_TRUE(response["result"]["deep"]["deeper"].get<bool>());
    EXPECT_EQ(2, response["result"]["count"]);
}

TEST(JsonRpcServer, ResponseLineNeverContainsAnEmbeddedNewline)
{
    // Framing invariant: one line in, one line out. A stray raw newline in a
    // value would split the response into two frames and desynchronize the
    // client for every request that follows.
    MethodRegistry registry;
    registry["multiline"] = [](const nlohmann::json &) -> RpcResult
    {
        return nlohmann::json{ { "text", "line one\nline two\rline three" } };
    };
    registry["broken"] = [](const nlohmann::json &) -> RpcResult
    {
        return Error::notFound("gone\nand reset");
    };

    const std::string ok = JsonRpcServer::dispatchLine(requestLine("1", "multiline"), registry);
    EXPECT_EQ(std::string::npos, ok.find('\n'));
    EXPECT_EQ(std::string::npos, ok.find('\r'));
    const nlohmann::json okResponse = nlohmann::json::parse(ok);
    EXPECT_EQ(std::string("line one\nline two\rline three"),
              okResponse["result"]["text"].get<std::string>());

    const std::string failed = JsonRpcServer::dispatchLine(requestLine("1", "broken"), registry);
    EXPECT_EQ(std::string::npos, failed.find('\n'));
    EXPECT_EQ(std::string::npos, failed.find('\r'));
    const nlohmann::json failedResponse = nlohmann::json::parse(failed);
    EXPECT_EQ(-32001, failedResponse["error"]["code"]);
    EXPECT_EQ(std::string("gone\nand reset"),
              failedResponse["error"]["data"]["message"].get<std::string>());
}

// ---------------------------------------------------------------------------
// Session bookkeeping
//
// A session record is only created by a real accepted connection, so the check
// that finished sessions are reaped — the property that stops a long-lived
// daemon from retaining one thread stack per call it has ever answered — belongs
// to the integration suite, which can drive many sequential connect/request/
// reply/close cycles and assert sessionCount() stays bounded. What is assertable
// without a socket is the contract that check is built on: a server that is not
// serving retains no sessions, and stop() — including on a server that never
// started — is safe, idempotent, and leaves it that way.
// ---------------------------------------------------------------------------
TEST(JsonRpcServer, ServerThatIsNotServingRetainsNoSessions)
{
    MethodRegistry registry = makeTestRegistry();
    JsonRpcServer server("127.0.0.1", 0, std::move(registry), "0.1.0");

    // Zero before anything was served, and the destructor at the end of this
    // scope must be equally quiet: a record left here would mean the
    // bookkeeping outlived the thing that owns it.
    EXPECT_EQ(0U, server.sessionCount());
    server.stop();
    EXPECT_EQ(0U, server.sessionCount());
}

TEST(JsonRpcServer, StopOnANeverStartedServerIsIdempotentAndLeavesNoSessions)
{
    MethodRegistry registry = makeTestRegistry();
    JsonRpcServer server("127.0.0.1", 0, std::move(registry), "0.1.0");

    // This is the path a server whose start() failed takes, and the one the
    // destructor always takes: stop() exists to join whatever is running, and
    // with nothing running it must neither block nor leave session records
    // behind for a later stop() or sessionCount() to trip over. Repeating it
    // is part of the declared contract, not a courtesy.
    server.stop();
    server.stop();
    server.stop();

    EXPECT_EQ(0U, server.sessionCount());
}

TEST(JsonRpcServer, SessionCountIsZeroAcrossConstructStopAndDestroyCycles)
{
    // Session bookkeeping is per-instance state that is created and destroyed
    // with the server, and a daemon that cannot bind is constructed, started
    // and stopped more than once in a single run. Fifty cheap cycles are what
    // makes "nothing about that state outlives its instance, and the destructor
    // never waits on it" more than an assertion about one object.
    for (int cycle = 0; cycle < 50; ++cycle)
    {
        JsonRpcServer server("127.0.0.1", 0, makeTestRegistry(), "0.1.0");
        server.stop();
        EXPECT_EQ(0U, server.sessionCount()) << "cycle " << cycle;
    }
}
