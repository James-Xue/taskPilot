// ControlClient.cpp — blocking client for the daemon's control socket
//
// Implements the contract in ControlClient.hpp: one request, one reply, every
// failure returned as an Error and nothing thrown. The header lists the five
// failure modes; this is where each one is produced:
//
//   mode                              produced at        error code
//   1. cannot connect                 step 2             kStorageFailure,
//                                                        with the actionable
//                                                        ./run.sh serve hint
//   2. timeout writing or reading     steps 3 and 4      kInternal, naming the
//                                                        timeout in ms
//   3. peer closed mid-read           step 4             kInternal, plus an
//                                                        excerpt of what did
//                                                        arrive
//   4. JSON-RPC error reply           step 6             mapped back through
//                                                        fromRpcErrorCode()
//   5. malformed reply                steps 5 and 6      kInternal, plus an
//                                                        excerpt
//
// How a call runs:
//   1. Serialize the request into one line:
//      {"jsonrpc":"2.0","id":1,"method":<method>,"params":<params>}\n
//   2. Resolve and connect to (m_host, m_port).
//   3. Write the line. asio::async_write loops internally, so a partial write
//      is not possible here.
//   4. Read exactly one '\n'-terminated line back; the protocol is one JSON
//      object per line.
//   5. Parse it. Anything unparseable is reported with an excerpt of what
//      actually arrived, because "the daemon returned garbage" is not
//      actionable while "the daemon returned <this line>" is.
//   6. Return the `result`, or turn an `error` reply into a mapped Error.
//
// Why a fresh connection per call instead of a held-open socket:
//   Both callers (mcp, cli) are short-lived processes talking to a
//   long-running daemon, so a persistent socket buys almost nothing and costs
//   a recovery policy. After a daemon restart a held socket is dead, and the
//   next request then needs retry-with-backoff just to notice. Connecting per
//   call makes the client stateless: the call after a restart simply works.
//
// Why every step carries a deadline:
//   A blocking read with no timeout can hang indefinitely, and a wedged
//   daemon still holds its socket open, so "connected but silent" is
//   indistinguishable from "alive and working" without one. In an MCP host
//   that hang presents to the user as the client freezing, with no error to
//   act on — strictly worse than a timeout they can read. The budget is per
//   step, not per call, so a call can take up to three timeouts end to end;
//   that is the price of an error message that names which step stalled.
//
// This file writes nothing to stdout or stderr. In the `mcp` process stdout is
// the MCP protocol stream, so one stray diagnostic line corrupts the framing;
// diagnostics are carried inside the returned Error instead, where the caller
// decides how to surface them.

#include "control/ControlClient.hpp"

#include <asio/buffer.hpp>
#include <asio/connect.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/steady_timer.hpp>
#include <asio/write.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <system_error>
#include <utility>

namespace taskpilot
{

namespace
{

// ---------------------------------------------------------------------------
// Limits and shared constants
// ---------------------------------------------------------------------------

/// Floor applied to a caller-supplied timeout.
///
/// The header documents timeouts as mandatory, so a caller that passes 0 or a
/// negative value must not get a timer that fires immediately (every call
/// would fail for a reason that looks like a network fault) and must not get
/// an unbounded wait either (the case the header exists to forbid). Clamping
/// once, in the constructor, keeps the read and write paths free of special
/// cases.
constexpr std::int64_t kMinTimeoutMs{ 1 };

/// Longest slice of an unparseable reply echoed back in an error message.
///
/// 200 characters is enough to recognise what arrived — a truncated JSON
/// object, a stray log line, an HTML error page — while keeping a megabyte of
/// garbage out of the caller's error text and, through the MCP bridge, out of
/// the model's context. Whatever is wrong with a reply is visible in its
/// first line; the rest is noise.
constexpr std::size_t kMaxExcerptChars{ 200 };

/// Ceiling on one reply line. The largest legitimate reply (get_queue or
/// list_tasks over a personal backlog) is tens of kilobytes, so this bounds
/// nothing real; it exists so a peer that never sends a newline cannot make
/// the client allocate without limit. read_until reports error::not_found
/// when the buffer hits it.
constexpr std::size_t kMaxReplyBytes{ 1024 * 1024 };

/// The request id sent on every call. Matching replies against it would be
/// pointless: each call owns its connection, so the only line that can arrive
/// on that socket is this request's reply. A constant also keeps the wire
/// format identical to what a hand-written probe (nc, a REPL session) sends,
/// which makes those probes usable for debugging this client.
constexpr int kRequestId{ 1 };

/// The one instruction that fixes the most common failure by far. Kept in one
/// place so every transport failure gives the same wording: no daemon running
/// is the expected state on a fresh machine, so the message should be a
/// command to run rather than an errno to decode.
constexpr const char *kStartHint{ "start it with ./run.sh serve, then retry" };

// JSON-RPC wire codes for application errors, mirroring rpcErrorCode() in
// src/control/Services.cpp. The complete table — and why the two halves must
// never drift — sits above fromRpcErrorCode() below.
constexpr int kRpcParseError{ -32700 };
constexpr int kRpcInvalidRequest{ -32600 };
constexpr int kRpcMethodNotFound{ -32601 };
constexpr int kRpcInvalidParams{ -32602 };
constexpr int kRpcInternalError{ -32603 };
constexpr int kRpcNotFound{ -32001 };
constexpr int kRpcConflict{ -32002 };
constexpr int kRpcStorageFailure{ -32003 };

/// "127.0.0.1:8123". Every failure message names the address actually dialed:
/// when a call fails, which socket the client tried is the first thing worth
/// checking, and a daemon listening on a different port is a common surprise.
std::string addressText(const std::string &host, std::uint16_t port)
{
    return host + ":" + std::to_string(port);
}

/// A one-line, bounded rendering of a reply that could not be used as-is.
///
/// The excerpt is the difference between an error the reader can act on and
/// one they can only report, so it is always included when the reply text is
/// available — but it is capped (see kMaxExcerptChars) and never empty: an
/// empty excerpt would read as a bug in the client rather than as a daemon
/// that answered with nothing.
std::string replyExcerpt(const std::string &text)
{
    if (text.empty())
    {
        return "<nothing arrived>";
    }
    if (kMaxExcerptChars >= text.size())
    {
        return text;
    }
    return text.substr(0, kMaxExcerptChars) + "... (truncated)";
}

/// One TCP connection whose every step is bounded by a deadline.
///
/// asio has no synchronous operation that accepts a timeout, so each step
/// races its asynchronous form against a steady_timer on a private
/// io_context; whichever finishes first cancels the other. Two details make
/// that work, and both are load-bearing:
///
///   1. The step's own completion handler cancels the timer. Without it,
///      io_context::run() would keep running until the deadline expired even
///      on success, turning every call into a full-timeout wait.
///   2. The timer's expiry handler cancels the step in flight (closing the
///      socket, cancelling the resolver). The step then completes with
///      operation_aborted, so run() returns only after both handlers have
///      fired and no handler can outlive this object. The alternative —
///      stopping the io_context and returning with the step still pending —
///      also works, but only by relying on handlers being destroyed
///      uninvoked; cancelling keeps the lifetime reasoning local to this
///      class.
///
/// Not thread-safe: a call() creates one, walks it through connect/write/read,
/// and drops it.
class DeadlineSocket
{
  public:
    /// How one step ended. kTimedOut means the deadline expired and the step
    /// was cancelled; kFailed means error() says why.
    enum class Outcome
    {
        kOk,
        kTimedOut,
        kFailed,
    };

    explicit DeadlineSocket(std::int64_t timeout_ms)
        : m_io(),
          m_socket(m_io),
          m_resolver(m_io),
          m_timer(m_io),
          m_timeout(std::chrono::milliseconds(
              static_cast<std::chrono::milliseconds::rep>(timeout_ms)))
    {
    }

    DeadlineSocket(const DeadlineSocket &) = delete;
    DeadlineSocket &operator=(const DeadlineSocket &) = delete;
    DeadlineSocket(DeadlineSocket &&) = delete;
    DeadlineSocket &operator=(DeadlineSocket &&) = delete;

    /// Why the last step failed. Meaningful only after a kFailed outcome: on a
    /// timeout the recorded code is the operation_aborted that the cancel
    /// produced, which says nothing about the peer and must not be reported as
    /// if it did.
    [[nodiscard]] const std::error_code &error() const { return m_ec; }

    /// Resolve `host` and connect to `port` on the first endpoint that
    /// accepts. An unresolvable name, a refused connection, and every other
    /// way of failing to reach the peer all end as kFailed — they differ only
    /// in the message the caller embeds.
    [[nodiscard]] Outcome connect(const std::string &host, std::uint16_t port)
    {
        // The service string must outlive the operation, so it lives here
        // rather than as a temporary in the call below.
        const std::string service = std::to_string(port);
        return run([this, &host, &service]() {
            m_resolver.async_resolve(
                host, service,
                [this](const std::error_code &resolve_ec,
                       const asio::ip::tcp::resolver::results_type &results) {
                    if (resolve_ec)
                    {
                        m_ec = resolve_ec;
                        m_timer.cancel();
                        return;
                    }
                    // The endpoint list is a member rather than a local so that
                    // it provably outlives the connect operation, whatever the
                    // implementation chooses to hold onto.
                    m_endpoints = results;
                    asio::async_connect(
                        m_socket, m_endpoints,
                        [this](const std::error_code &connect_ec,
                               const asio::ip::tcp::endpoint &) {
                            m_ec = connect_ec;
                            m_timer.cancel();
                        });
                });
        });
    }

    /// Send every byte of `data`. async_write loops until the buffer is fully
    /// drained, so a short write cannot be mistaken for success.
    [[nodiscard]] Outcome writeAll(const std::string &data)
    {
        return run([this, &data]() {
            asio::async_write(m_socket, asio::buffer(data),
                              [this](const std::error_code &write_ec, std::size_t) {
                                  m_ec = write_ec;
                                  m_timer.cancel();
                              });
        });
    }

    /// Read up to and including the first '\n' into `out` (which is cleared
    /// first). A buffer that fills without a newline fails with
    /// error::not_found; a peer that closes first fails with eof or a reset —
    /// in both cases `out` still holds whatever did arrive, which is the only
    /// diagnostic available.
    [[nodiscard]] Outcome readLine(std::string &out)
    {
        out.clear();
        return run([this, &out]() {
            asio::async_read_until(m_socket, asio::dynamic_buffer(out, kMaxReplyBytes), '\n',
                                   [this](const std::error_code &read_ec, std::size_t) {
                                       m_ec = read_ec;
                                       m_timer.cancel();
                                   });
        });
    }

  private:
    /// Drive one asynchronous step to completion under the deadline.
    ///
    /// `start` must issue exactly one async operation on this object's socket
    /// or resolver, and its completion handler must record the outcome in m_ec
    /// and then cancel m_timer.
    template <typename StartFn>
    Outcome run(StartFn start)
    {
        // io_context::run() may only be re-entered after restart(); without
        // this, the second step of a call would return immediately and be
        // misread as a completed (empty) operation.
        m_io.restart();
        m_ec.clear();
        m_timedOut = false;

        m_timer.expires_after(m_timeout);
        m_timer.async_wait([this](const std::error_code &timer_ec) {
            // operation_aborted means the step cancelled this wait, i.e. the
            // step won the race. Only a genuine expiry may declare a timeout,
            // otherwise a fast success would be misreported as one.
            if (asio::error::operation_aborted == timer_ec)
            {
                return;
            }
            m_timedOut = true;

            // Cancel the step in flight so its handler still fires and run()
            // can return through the normal path. The errors are ignored on
            // purpose: there is nothing useful to do with "the socket was
            // already closed" while handling a timeout.
            std::error_code ignored;
            m_socket.close(ignored);
            m_resolver.cancel();
        });

        start();
        m_io.run();

        // The outcome is fully determined by which handler fired first. The
        // timeout check comes first because a cancelled step reports
        // operation_aborted, which would otherwise be read as a peer failure.
        if (m_timedOut)
        {
            return Outcome::kTimedOut;
        }
        if (m_ec)
        {
            return Outcome::kFailed;
        }
        return Outcome::kOk;
    }

    asio::io_context m_io;
    asio::ip::tcp::socket m_socket;
    asio::ip::tcp::resolver m_resolver;
    asio::steady_timer m_timer;
    std::chrono::milliseconds m_timeout;
    asio::ip::tcp::resolver::results_type m_endpoints;
    std::error_code m_ec;
    bool m_timedOut{ false };
};

} // namespace

// ---------------------------------------------------------------------------
// ErrorCode <-> JSON-RPC wire code
// ---------------------------------------------------------------------------
// ONE mapping, written down twice, and the two halves must not drift:
//
//   forward   rpcErrorCode()                     src/control/Services.cpp
//   inverse   ControlClient::fromRpcErrorCode()  this file, directly below
//
//   ErrorCode          wire code   meaning on the wire
//   kInvalidArgument   -32602      invalid params (a protocol-level fault)
//   kNotFound          -32001      no such task
//   kConflict          -32002      conflicts with current state
//   kStorageFailure    -32003      SQLite or filesystem failure
//   kInternal          -32603      bug or unexpected state
//
// The application codes (-32000..-32099) exist because the standard set has no
// room for domain failures: an unknown task id is not a transport error and
// must not be reported as one. A wire code in that range which is not listed
// above (McpServer's -32022 unsupported-version, a code added later) degrades
// to kInternal rather than being guessed at — an invented classification is
// worse than an honest "unexpected", because the caller would act on it.
//
// Changing either half is a two-file change; do both in one commit.
ErrorCode ControlClient::fromRpcErrorCode(int code)
{
    // Protocol-level faults that never reach a handler. The forward direction
    // collapses them too: the server answers an unknown method with the same
    // family as a bad argument, so a client cannot do better than
    // kInvalidArgument for any of the three.
    if (kRpcInvalidParams == code || kRpcInvalidRequest == code || kRpcMethodNotFound == code)
    {
        return ErrorCode::kInvalidArgument;
    }
    if (kRpcNotFound == code)
    {
        return ErrorCode::kNotFound;
    }
    if (kRpcConflict == code)
    {
        return ErrorCode::kConflict;
    }
    if (kRpcStorageFailure == code)
    {
        return ErrorCode::kStorageFailure;
    }

    // -32603 is the forward mapping of kInternal; -32700 (the daemon could not
    // parse the line we sent, i.e. the request was corrupted in transit) is a
    // client-side or transport-side bug with no better family. Everything
    // else — including the reserved-but-unassigned application codes — lands
    // here by the rule stated in the header.
    if (kRpcInternalError == code || kRpcParseError == code)
    {
        return ErrorCode::kInternal;
    }
    return ErrorCode::kInternal;
}

namespace
{

/// One full request/response exchange, steps 1 to 6 of the file header.
///
/// Split out of ControlClient::call() so that call() can stay a thin guard:
/// the transport below has several paths that throw (nlohmann's dumper and
/// parser, asio's io_context::run()) and the class contract is that no
/// exception ever leaves a public method.
[[nodiscard]] RpcResult callOnce(const std::string &host, std::uint16_t port,
                                 std::int64_t timeout_ms, const std::string &method,
                                 const nlohmann::json &params)
{
    // --- 1. Serialize -----------------------------------------------------
    // nlohmann refuses to dump a string that is not valid UTF-8 (a task title
    // can carry one), so this needs a guard: bad input from the caller is a
    // kInvalidArgument, not a crash.
    std::string request;
    try
    {
        const nlohmann::json envelope{
            { "jsonrpc", "2.0" },
            { "id", kRequestId },
            { "method", method },
            { "params", params },
        };
        request = envelope.dump();
        request.push_back('\n');
    }
    catch (const std::exception &exception)
    {
        return Error::invalidArgument("cannot serialize the request for '" + method +
                                      "': " + exception.what());
    }

    const std::string address = addressText(host, port);
    DeadlineSocket connection(timeout_ms);

    // --- 2. Connect -------------------------------------------------------
    // The most common failure, and the only one with a one-command fix, so it
    // gets kStorageFailure plus the run.sh hint rather than a raw errno.
    const DeadlineSocket::Outcome connected = connection.connect(host, port);
    if (DeadlineSocket::Outcome::kTimedOut == connected)
    {
        return Error::storageFailure("timed out after " + std::to_string(timeout_ms) +
                                     " ms connecting to the taskPilot daemon at " + address +
                                     " (nothing accepting there, or the port is filtered); " +
                                     kStartHint);
    }
    if (DeadlineSocket::Outcome::kFailed == connected)
    {
        return Error::storageFailure("cannot connect to the taskPilot daemon at " + address +
                                     ": " + connection.error().message() + "; " + kStartHint);
    }

    // --- 3. Write ---------------------------------------------------------
    const DeadlineSocket::Outcome written = connection.writeAll(request);
    if (DeadlineSocket::Outcome::kTimedOut == written)
    {
        return Error::internal("timed out after " + std::to_string(timeout_ms) +
                               " ms sending '" + method + "' to the taskPilot daemon at " +
                               address + ": the connection opened but nothing read from it");
    }
    if (DeadlineSocket::Outcome::kFailed == written)
    {
        return Error::internal("the connection to the taskPilot daemon at " + address +
                               " broke while sending '" + method + "' (" +
                               connection.error().message() + "); " + kStartHint);
    }

    // --- 4. Read one line -------------------------------------------------
    std::string line;
    const DeadlineSocket::Outcome received = connection.readLine(line);
    if (DeadlineSocket::Outcome::kTimedOut == received)
    {
        return Error::internal("timed out after " + std::to_string(timeout_ms) +
                               " ms waiting for the reply to '" + method + "' from " + address +
                               ": the daemon accepted the request but did not answer");
    }
    if (DeadlineSocket::Outcome::kFailed == received)
    {
        // A full buffer means the peer never sent a newline; anything else
        // means it closed on us. Both are diagnosed by the same thing — what
        // actually arrived — which read_until leaves in `line`.
        if (asio::error::not_found == connection.error())
        {
            return Error::internal("the reply to '" + method + "' from " + address +
                                   " exceeded " + std::to_string(kMaxReplyBytes) +
                                   " bytes without a newline; giving up");
        }
        return Error::internal("the daemon at " + address +
                               " closed the connection before finishing its reply to '" +
                               method + "' (" + connection.error().message() +
                               "); received: " + replyExcerpt(line) + "; " + kStartHint);
    }

    // Strip the terminator. The server strips a trailing '\r' on input and
    // writes '\n', but tolerating CRLF here costs nothing and keeps the client
    // usable against a proxy or a hand-written stub that answers with CRLF.
    if (!line.empty() && '\n' == line.back())
    {
        line.pop_back();
    }
    if (!line.empty() && '\r' == line.back())
    {
        line.pop_back();
    }

    // --- 5. Parse ---------------------------------------------------------
    nlohmann::json reply;
    try
    {
        reply = nlohmann::json::parse(line);
    }
    catch (const nlohmann::json::exception &exception)
    {
        // The parser's own message names the offset, which is useful for
        // spotting a truncated read; the excerpt shows what came through.
        return Error::internal("the daemon at " + address + " replied to '" + method +
                               "' with something that is not JSON (" + exception.what() +
                               "); received: " + replyExcerpt(line));
    }

    if (!reply.is_object())
    {
        return Error::internal("the reply to '" + method + "' from " + address +
                               " is not a JSON-RPC response object; received: " +
                               replyExcerpt(line));
    }

    // --- 6. Decode --------------------------------------------------------
    // JSON-RPC 2.0: a response carries exactly one of `result` or `error`. A
    // null `error` member is tolerated and ignored, because it is harmless and
    // some peers emit it next to a result; a non-null `error` wins over any
    // result, since the error is the informative half of a malformed pair.
    const auto error_it = reply.find("error");
    if (reply.end() != error_it && !error_it->is_null() && error_it->is_object())
    {
        const auto code_it = error_it->find("code");
        if (error_it->end() == code_it || !code_it->is_number_integer())
        {
            // No code means no mapping is possible, so this is a malformed
            // reply (mode 5) rather than a mapped domain error (mode 4).
            return Error::internal("the error reply to '" + method + "' from " + address +
                                   " carries no integer code; received: " + replyExcerpt(line));
        }

        const int code = code_it->get<int>();
        const ErrorCode mapped = ControlClient::fromRpcErrorCode(code);

        // The daemon's own wording is preferred: it names the offending
        // parameter or task id, which no generic mapping can. The synthesized
        // fallback exists only for a reply that carried a code but no message.
        const auto message_it = error_it->find("message");
        if (error_it->end() != message_it && message_it->is_string() &&
            !message_it->get_ref<const std::string &>().empty())
        {
            return Error{ mapped, message_it->get<std::string>() };
        }
        return Error{ mapped, "the daemon reported a " + toString(mapped) +
                                  " failure (JSON-RPC error " + std::to_string(code) +
                                  ") while handling '" + method + "'" };
    }
    if (reply.end() != error_it && !error_it->is_null())
    {
        return Error::internal("the 'error' member of the reply to '" + method + "' from " +
                               address + " is not an object; received: " + replyExcerpt(line));
    }

    const auto result_it = reply.find("result");
    if (reply.end() != result_it)
    {
        return *result_it;
    }

    return Error::internal("the reply to '" + method + "' from " + address +
                           " carries neither a result nor an error; received: " +
                           replyExcerpt(line));
}

} // namespace

ControlClient::ControlClient(std::string host, std::uint16_t port, std::int64_t timeout_ms)
    : m_host(std::move(host)),
      m_port(port),
      m_timeoutMs(timeout_ms > 0 ? timeout_ms : kMinTimeoutMs)
{
}

RpcResult ControlClient::call(const std::string &method, const nlohmann::json &params) const
{
    // The only try/catch in the class, and a backstop rather than a control
    // path: callOnce() returns an Error for every failure it can anticipate.
    // It exists because asio and nlohmann both have throw paths, and a client
    // that promises "never throws" must not be the component that takes the
    // host process down with an escaping exception.
    try
    {
        return callOnce(m_host, m_port, m_timeoutMs, method, params);
    }
    catch (const std::exception &exception)
    {
        return Error::internal("the control-socket call '" + method +
                               "' failed unexpectedly: " + exception.what());
    }
    catch (...)
    {
        return Error::internal("the control-socket call '" + method +
                               "' failed with a non-standard exception");
    }
}

bool ControlClient::ping() const
{
    // Connect and let the socket close when `connection` goes out of scope —
    // deliberately no request, so the probe still means "the daemon is
    // listening" when its store is busy (see the header).
    //
    // The catch-all is the point of the function: a liveness probe answers
    // yes/no, and a caller has no better response to an exception than
    // "false". The reason is deliberately not reported here either; the next
    // real call() produces a message that names the address and the fix.
    try
    {
        DeadlineSocket connection(m_timeoutMs);
        return DeadlineSocket::Outcome::kOk == connection.connect(m_host, m_port);
    }
    catch (...)
    {
        return false;
    }
}

} // namespace taskpilot
