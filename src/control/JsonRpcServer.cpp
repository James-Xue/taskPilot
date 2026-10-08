// JsonRpcServer.cpp — see JsonRpcServer.hpp
//
// Threading model, in one place because it is the only subtle part of this
// file:
//   1. start() binds, listens, and runs acceptLoop() on its own thread.
//   2. acceptLoop() accepts one connection at a time and hands it to a fresh
//      handleSession() thread, recording the thread, the socket, and a
//      completion flag together in one Session record.
//   3. handleSession() blocks in read_until(), answering one line at a time,
//      and raises the record's completion flag as its last act.
//   4. acceptLoop() reaps every record whose flag is set — joining the thread
//      and dropping the record — so a record lives as long as its connection
//      rather than as long as the daemon.
//   5. stop() unblocks everything above and joins it.
//
// Reaping is not housekeeping. An exited thread that has never been joined
// keeps its stack mapping until the process exits (measured here at roughly
// 8 MB of address space per connection), and both clients of this socket open
// one connection per call (see ControlClient.hpp), so leaving records in place
// would make the retained address space grow with every call the daemon ever
// answered instead of with the connections in flight.
//
// m_sessionMutex guards m_sessions, the shutdown sweep stop() runs over the
// sockets in it, and each session's own final close. A socket is not
// thread-safe, so every operation that can run while another thread still holds
// the socket goes through the lock.
//
// Ownership rule that keeps the teardown race-free: a session socket is closed
// by its own session thread and by nobody else. stop() wakes it with
// shutdown() and then joins it, so no thread ever closes a descriptor another
// thread is still using. The acceptor follows the same rule — it is closed only
// after the accept thread has been joined.
//
// The reaper honours the same rule from the other side: it joins a thread only
// once that thread has raised its completion flag, which happens after its last
// lock acquisition, so joining can neither block nor race with a session that
// is still serving.
//
// Nothing in this file is on a hot path. Each connection is independent, and
// the only shared mutable state behind the handlers is TaskStore, which
// serializes its own access.

#include "control/JsonRpcServer.hpp"

#include <asio/error.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/system_error.hpp>
#include <asio/write.hpp>

#include <poll.h>

#include <cstddef>
#include <iostream>
#include <string>
#include <utility>

namespace taskpilot
{

namespace
{

/// Protocol version stamped on every response. JSON-RPC 2.0 has exactly one
/// version string, so unlike the MCP bridge there is nothing to negotiate.
constexpr const char *kJsonRpcVersion = "2.0";

/// The three protocol-level codes from the error table in JsonRpcServer.hpp.
/// The application range (-32000..-32099) is produced by rpcErrorCode()
/// instead, so the two sets can never be confused with each other.
constexpr int kParseErrorCode{ -32700 };
constexpr int kInvalidRequestCode{ -32600 };
constexpr int kMethodNotFoundCode{ -32601 };

/// How long poll() waits for a connection before acceptLoop() re-checks
/// m_running. This is the upper bound on how long stop() waits for the accept
/// thread, and it costs nothing in latency: poll() returns the instant a
/// connection is queued, so a real client is never delayed by it.
constexpr int kAcceptPollMilliseconds{ 100 };

/// Render one complete error response line.
///
/// The id is echoed exactly as handed in — including null, which is what a
/// protocol-level failure reports when the request could not be attributed to
/// a caller (a parse error or a malformed envelope carries no usable id, and
/// inventing one would make the client match a response it never asked for).
std::string errorLine(const nlohmann::json &id, int code, const std::string &message)
{
    return nlohmann::json{
        { "jsonrpc", kJsonRpcVersion },
        { "id", id },
        { "error", nlohmann::json{ { "code", code }, { "message", message } } },
    }.dump();
}

/// Remove the line terminator left by the read: the delimiter itself, plus the
/// '\r' a CRLF client (telnet, PowerShell, a raw socket probe) puts in front of
/// it. Only the tail is touched, so a '\r' inside a JSON string value — which
/// would have to be escaped anyway — cannot be damaged.
void stripLineTerminator(std::string &line)
{
    if (!line.empty() && '\n' == line.back())
    {
        line.pop_back();
    }
    if (!line.empty() && '\r' == line.back())
    {
        line.pop_back();
    }
}

/// Call one handler, turning a thrown exception into an internal error.
///
/// A handler that throws is a bug rather than a client error, and it must not
/// take the connection down with it: the session that reported it stays usable
/// for the next request.
RpcResult callHandler(const MethodHandler &handler, const nlohmann::json &params)
{
    try
    {
        return handler(params);
    }
    catch (const std::exception &e)
    {
        return Error::internal(std::string("handler threw: ") + e.what());
    }
}

/// Dispatch one unterminated line and send its response, if any.
///
/// A notification produces no response and therefore no write. Exceptions from
/// the write are left to the caller: only it knows whether the session is over.
void serveLine(asio::ip::tcp::socket &socket, const std::string &line,
               const MethodRegistry &registry)
{
    const std::string response = JsonRpcServer::dispatchLine(line, registry);
    if (response.empty())
    {
        return;
    }
    // One response, one line. There is nothing to flush afterwards: asio's
    // blocking write puts every byte on the wire before returning, which is
    // also why a short write cannot silently truncate a frame here.
    const std::string frame = response + '\n';
    asio::write(socket, asio::buffer(frame));
}

} // namespace

// ---------------------------------------------------------------------------
// Dispatch — pure, socket-free, and therefore fully unit-testable
// ---------------------------------------------------------------------------
std::string JsonRpcServer::dispatchLine(const std::string &line,
                                        const MethodRegistry &registry)
{
    // 1. Parse. A line that is not JSON cannot be attributed to any request,
    //    so the response carries a null id, as JSON-RPC requires.
    nlohmann::json request;
    try
    {
        request = nlohmann::json::parse(line);
    }
    catch (const nlohmann::json::exception &)
    {
        return errorLine(nullptr, kParseErrorCode, "Parse error");
    }

    // 2. Envelope. Only an object with a string "method" is a request: a batch
    //    array, a bare scalar, or a missing/wrong-typed method is invalid. The
    //    message names what was expected rather than echoing the input, which
    //    may be large.
    if (!request.is_object() || !request.contains("method")
        || !request.at("method").is_string())
    {
        return errorLine(nullptr, kInvalidRequestCode,
                         "Invalid request: expected an object with a string \"method\"");
    }

    // 3. Notification detection, and it must come BEFORE the lookup below: a
    //    notification is never answered, so there is no response to carry an
    //    unknown-method error either. Checking first is also what keeps a
    //    fire-and-forget client from having its handler run.
    if (!request.contains("id") || request.at("id").is_null())
    {
        return "";
    }

    const nlohmann::json id = request.at("id");

    // 4. Lookup. The registry is the single source of truth for what the
    //    daemon can do — the same names the MCP tool list publishes.
    const std::string method = request.at("method").get<std::string>();
    const auto handler = registry.find(method);
    if (registry.end() == handler)
    {
        return errorLine(id, kMethodNotFoundCode, "Method not found: " + method);
    }

    // 5. Call. Absent params become an empty object, which is what every
    //    handler in the catalog can accept; a present params value of any type
    //    is passed through so the handler can report its own argument error.
    const nlohmann::json params =
        request.contains("params") ? request.at("params") : nlohmann::json::object();

    RpcResult result = callHandler(handler->second, params);

    // 6. Response. Success and failure are mutually exclusive — a response
    //    carrying both "result" and "error" is the one shape a conforming
    //    client is entitled to reject.
    if (result.ok())
    {
        return nlohmann::json{
            { "jsonrpc", kJsonRpcVersion },
            { "id", id },
            { "result", result.value() },
        }.dump();
    }

    const Error &failure = result.error();
    return nlohmann::json{
        { "jsonrpc", kJsonRpcVersion },
        { "id", id },
        { "error",
          nlohmann::json{
              { "code", rpcErrorCode(failure.code) },
              { "message", failure.message },
              // Machine-readable twin of the wire code: the numeric code is
              // what a client switches on, but the stable name survives a
              // remapping of the -32000 block and is what a human or an LLM
              // reads in a log. The message is repeated here so a client that
              // only surfaces `data` still shows something useful.
              { "data",
                nlohmann::json{
                    { "code", toString(failure.code) },
                    { "message", failure.message },
                } } } },
    }.dump();
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
JsonRpcServer::JsonRpcServer(std::string bind_address, std::uint16_t port,
                             MethodRegistry registry, std::string version)
    : m_bindAddress{ std::move(bind_address) }
    , m_port{ port }
    , m_registry{ std::move(registry) }
    , m_version{ std::move(version) }
    , m_acceptor{ m_ioCtx }
{
}

JsonRpcServer::~JsonRpcServer()
{
    stop();
}

Status JsonRpcServer::start()
{
    // A second start() would try to open an acceptor that is already open and
    // report it as a bind failure, which reads as "another daemon is running"
    // when the caller's own bug is the real cause.
    if (m_running.load(std::memory_order_acquire))
    {
        return Error::conflict("control socket " + m_bindAddress + ":"
                               + std::to_string(m_port) + " is already started");
    }

    try
    {
        // resolve() rather than make_address() so a configured name such as
        // "localhost" works as well as a literal address.
        asio::ip::tcp::resolver resolver(m_ioCtx);
        const auto endpoints = resolver.resolve(m_bindAddress, std::to_string(m_port));
        if (endpoints.empty())
        {
            return Error::invalidArgument("control socket " + m_bindAddress + ":"
                                          + std::to_string(m_port)
                                          + ": the bind address resolved to no endpoint");
        }
        const asio::ip::tcp::endpoint endpoint = endpoints.begin()->endpoint();

        m_acceptor.open(endpoint.protocol());
        // reuse_address lets a restart take the port immediately instead of
        // waiting out TIME_WAIT on the sockets of the previous run.
        m_acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        m_acceptor.bind(endpoint);
        m_acceptor.listen(asio::socket_base::max_listen_connections);

        // Non-blocking accept is what makes stop() possible: a blocking
        // accept() cannot be interrupted from another thread on Linux, so the
        // loop polls instead (see acceptLoop).
        asio::error_code option_error;
        m_acceptor.non_blocking(true, option_error);
        if (option_error)
        {
            asio::error_code close_error;
            m_acceptor.close(close_error);
            return Error::internal("control socket " + m_bindAddress + ":"
                                   + std::to_string(m_port)
                                   + ": cannot switch the acceptor to non-blocking mode: "
                                   + option_error.message());
        }
    }
    catch (const asio::system_error &e)
    {
        // Leave the acceptor closed so that a retry — with a different port,
        // say — starts from a clean state instead of tripping over the
        // half-open socket this attempt left behind.
        asio::error_code close_error;
        m_acceptor.close(close_error);

        // The address is reported verbatim: the caller's very next thought is
        // "is a daemon already running?", and an errno would not answer it.
        const std::string where =
            m_bindAddress + ":" + std::to_string(m_port);
        if (asio::error::address_in_use == e.code())
        {
            return Error::conflict(
                "control socket " + where
                + " is already in use: a taskPilot daemon is most likely "
                  "running and holding that port. Stop it, or start this one "
                  "with another port. (os error: "
                + e.code().message() + ")");
        }
        return Error::conflict("control socket " + where
                               + " could not be opened: " + e.code().message()
                               + ". If a taskPilot daemon is already running on "
                                 "that port, stop it or choose another port.");
    }

    // Port 0 means "the OS picks", so report back the port that was actually
    // assigned — the caller (and the integration test) has no other way to
    // learn it.
    if (0 == m_port)
    {
        m_port = m_acceptor.local_endpoint().port();
    }

    // Publish the running flag before the thread exists, so the loop can never
    // observe a stale false and exit immediately.
    m_running.store(true, std::memory_order_release);
    try
    {
        m_acceptThread = std::thread(&JsonRpcServer::acceptLoop, this);
    }
    catch (const std::system_error &e)
    {
        // No accept thread means no service. Report it instead of leaving a
        // flag set on a server that will never answer, and release the port on
        // the way out.
        m_running.store(false, std::memory_order_release);
        asio::error_code close_error;
        m_acceptor.close(close_error);
        return Error::internal("control socket " + m_bindAddress + ":"
                               + std::to_string(m_port)
                               + ": could not start the accept thread: " + e.what());
    }

    return Unit{};
}

void JsonRpcServer::stop()
{
    // Idempotence comes from the operations, not from a "stopped" flag:
    // closing an already-closed acceptor and joining an already-joined thread
    // are both no-ops, so a second call — or a call after a start() that
    // failed part-way — does nothing.
    m_running.store(false, std::memory_order_release);

    // 1. Join the accept thread first, before touching either the acceptor or
    //    the session handles. The loop is parked in poll() for at most
    //    kAcceptPollMilliseconds, so the wait is bounded, and afterwards no
    //    thread but this one can touch the acceptor or append a session. Note
    //    that the loop does not need the acceptor closed to wake up — which is
    //    why it polls instead of blocking in accept().
    if (m_acceptThread.joinable())
    {
        m_acceptThread.join();
    }

    // 2. Close the acceptor only now that nothing else is using it. Closing it
    //    earlier would write the acceptor's internal state while the accept
    //    thread is reading that same state inside accept() — a data race with
    //    no purpose, since nothing needed waking.
    asio::error_code ignored;
    m_acceptor.close(ignored);

    // 3. Take the session records, then wake every parked read_until with
    //    shutdown(). Deliberately not close(): asio's close() also rewrites
    //    the socket's internal state, which the session thread may be reading
    //    at that instant, whereas shutdown() is a plain syscall. Closing is
    //    left to the thread that owns the socket — its own session thread,
    //    which is why every session below must be joined.
    std::vector<Session> sessions;
    {
        std::lock_guard lock(m_sessionMutex);
        sessions.swap(m_sessions);
        for (const Session &session : sessions)
        {
            session.socket->shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        }
    }

    // 4. Join OUTSIDE the lock: a session's own exit path takes
    //    m_sessionMutex to close its socket, so joining while holding it would
    //    deadlock against a session that is finishing right now. Dropping the
    //    records at the end of this function then destroys the sockets safely —
    //    every thread that could still hold one has been joined by then. A
    //    record whose session had already finished is joined here just as
    //    readily: the reaper is the fast path, not the only one.
    for (Session &session : sessions)
    {
        if (session.thread.joinable())
        {
            session.thread.join();
        }
    }
}

std::size_t JsonRpcServer::sessionCount() const
{
    // The lock is what makes this usable while the accept loop is registering
    // or reaping: the count can never be read from a half-updated vector. It is
    // the only const query in this class that needs it, which is why
    // m_sessionMutex is the one mutable member.
    std::lock_guard lock(m_sessionMutex);
    return m_sessions.size();
}

void JsonRpcServer::acceptLoop()
{
    while (m_running.load(std::memory_order_acquire))
    {
        // Reap at the top of every pass, which includes the pass an idle poll
        // timeout produces. Relying on the accept path alone would leave the
        // most recent connection's thread and socket retained for as long as no
        // further client appears — on a desktop daemon, potentially until it
        // exits.
        reapFinishedSessions();

        auto socket = std::make_shared<asio::ip::tcp::socket>(m_ioCtx);

        asio::error_code accept_error;
        m_acceptor.accept(*socket, accept_error);

        if (accept_error)
        {
            if (asio::error::would_block == accept_error
                || asio::error::try_again == accept_error)
            {
                // Nothing queued. Poll instead of retrying in a spin loop:
                // it blocks until either a connection arrives (immediately
                // readable on a listening socket) or the timeout expires and
                // m_running is re-checked.
                struct pollfd descriptor
                {
                    m_acceptor.native_handle(), POLLIN, 0
                };
                ::poll(&descriptor, 1, kAcceptPollMilliseconds);
                continue;
            }
            if (asio::error::operation_aborted == accept_error
                || asio::error::bad_descriptor == accept_error)
            {
                // The acceptor is gone — somebody closed it while this loop
                // was between two checks. Nothing left to serve.
                break;
            }
            // Something else went wrong with one pending connection (a client
            // that reset between the SYN and the accept, for instance). The
            // listening socket is still good, so report and keep serving.
            std::cerr << "taskpilot: control socket accept failed: "
                      << accept_error.message() << "\n";
            continue;
        }

        // The acceptor is non-blocking; whether the accepted socket inherits
        // that is a platform detail this must not depend on. A non-blocking
        // session socket would return would_block from the first read_until()
        // and end every session at its first request, so set it explicitly.
        asio::error_code ignored;
        socket->non_blocking(false, ignored);

        // Register the record under the lock, and note what makes that
        // ordering sufficient:
        // 1. stop() joins this thread before it looks at m_sessions, so it can
        //    never observe a record mid-construction; there is no window in
        //    which a running session is missing from the vector.
        // 2. The whole record — thread, socket, completion flag — is built in
        //    one expression, so a failure to spawn the thread throws before
        //    m_sessions changes and leaves nothing behind to undo.
        // 3. The lambda copies the two shared_ptrs rather than referring to the
        //    record's members, so it stays valid across the move into the
        //    vector when the vector regrows.
        auto done = std::make_shared<std::atomic<bool>>(false);
        try
        {
            std::lock_guard lock(m_sessionMutex);
            m_sessions.push_back(Session{
                socket,
                done,
                std::thread([this, socket, done]() {
                    handleSession(socket);

                    // The record's completion flag, raised last: it is the
                    // reaper's permission to join this thread and drop the
                    // record, so it must not be set while the session could
                    // still close its socket or take m_sessionMutex. The
                    // release store pairs with the reaper's acquire load, which
                    // is what lets the reaper trust everything the session did
                    // before raising it.
                    done->store(true, std::memory_order_release);
                }),
            });
        }
        catch (const std::system_error &e)
        {
            // The thread could not be spawned (thread or descriptor
            // exhaustion). Letting that escape would end the accept thread —
            // and with it the daemon — because an exception leaving a thread
            // function calls std::terminate. Nothing was registered, so there
            // is no bookkeeping to undo; close the connection rather than
            // leaving the client waiting on a socket nobody will ever read.
            asio::error_code close_error;
            socket->close(close_error);
            std::cerr << "taskpilot: control socket cannot serve a connection: "
                      << e.what() << "\n";
        }
    }
}

void JsonRpcServer::reapFinishedSessions()
{
    // Phase 1 — collect under the lock. A finished record leaves m_sessions the
    // instant it is seen, so stop() and sessionCount() stop counting it here;
    // it is not destroyed yet, because the join still has to happen and a join
    // has no business holding the lock that every finishing session takes.
    std::vector<Session> finished;
    {
        std::lock_guard lock(m_sessionMutex);
        auto it = m_sessions.begin();
        while (m_sessions.end() != it)
        {
            // The acquire load pairs with the session thread's release store:
            // a record seen here as finished is one whose thread has released
            // every resource the record owns, and joining it cannot block.
            if (!it->done->load(std::memory_order_acquire))
            {
                ++it;
                continue;
            }
            finished.push_back(std::move(*it));
            it = m_sessions.erase(it);
        }
    }

    // Phase 2 — join outside the lock, for the same reason stop() does: a
    // session's exit path takes m_sessionMutex to close its socket, and holding
    // the lock across a join would make the reaper a second party to that
    // interaction with nothing to gain. Every thread here has already run its
    // last statement, so each join returns immediately.
    for (Session &session : finished)
    {
        if (session.thread.joinable())
        {
            session.thread.join();
        }
    }

    // Phase 3 is the end of this scope: the records die here and the last
    // reference to each socket dies with them. That is safe only now — a socket
    // destroyed while its thread could still use it would be a use-after-free,
    // and a record kept past this point would retain the thread's stack, which
    // is the leak this function exists to prevent.
}

void JsonRpcServer::handleSession(std::shared_ptr<asio::ip::tcp::socket> socket)
{
    if (nullptr == socket)
    {
        return; // never happens; acceptLoop always hands over a live socket
    }

    asio::streambuf buffer;
    try
    {
        while (m_running.load(std::memory_order_acquire))
        {
            const std::size_t bytes = asio::read_until(*socket, buffer, '\n');
            if (0 == bytes)
            {
                break;
            }

            // read_until leaves exactly one line in the get area, delimiter
            // included; consume it before dispatching so the next iteration
            // starts from a clean buffer even if the handler throws.
            std::string line(asio::buffer_cast<const char *>(buffer.data()), bytes);
            buffer.consume(bytes);

            stripLineTerminator(line);
            if (line.empty())
            {
                // Keep-alive newline (or CRLF): nothing to answer.
                continue;
            }

            serveLine(*socket, line, m_registry);
        }
    }
    catch (const asio::system_error &e)
    {
        // A client that disconnects mid-line is normal traffic, not an error
        // worth logging. The one case worth serving: a client that closed
        // right after its last line without sending a final newline — the
        // read fails with eof but the bytes it did send are still in the
        // buffer, and answering them is what makes a hand-run `printf | nc`
        // probe work.
        if (asio::error::eof == e.code() && 0 < buffer.size())
        {
            std::string line(asio::buffer_cast<const char *>(buffer.data()),
                             buffer.size());
            stripLineTerminator(line);
            if (!line.empty())
            {
                try
                {
                    serveLine(*socket, line, m_registry);
                }
                catch (const std::exception &)
                {
                    // The peer is gone; the answer to its last line is lost.
                }
            }
        }
    }
    catch (const std::exception &)
    {
        // Anything not derived from asio::system_error is unexpected, but the
        // session is over either way and there is no recovery to attempt.
    }

    // The session thread is the only thread that ever closes its own socket,
    // which is what makes the socket's lifetime easy to reason about; the lock
    // is shared with stop()'s shutdown sweep so the two can never interleave.
    // shutdown() before close() because a close() with unread data still in the
    // receive queue turns into a reset, and a client being told the daemon is
    // going away deserves an orderly end of stream instead.
    std::lock_guard lock(m_sessionMutex);
    asio::error_code ignored;
    socket->shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket->close(ignored);
}

} // namespace taskpilot
