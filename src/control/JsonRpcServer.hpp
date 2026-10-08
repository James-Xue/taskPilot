#pragma once
// JsonRpcServer.hpp — the backend daemon's control socket
//
// Newline-delimited JSON-RPC 2.0 over TCP. One JSON object per line; a
// trailing \r is stripped so a client may send CRLF.
//
//   Request      -> {"jsonrpc":"2.0","id":1,"method":"get_queue","params":{}}
//   Notification -> same, without "id"          (no response is sent)
//   Response     -> {"jsonrpc":"2.0","id":1,"result":...} | {...,"error":{...}}
//
// Error codes: -32700 parse, -32600 invalid request, -32601 method not found,
// -32602 invalid params, -32000..-32099 application errors (see rpcErrorCode).
//
// Binding: 127.0.0.1 by default. taskPilot is a single-user personal tool; the
// socket has no authentication, so exposing it beyond the loopback interface
// would hand anyone on the network write access to the backlog.
//
// dispatchLine() is a pure static function so the whole protocol can be tested
// without opening a socket.

#include <asio/ip/tcp.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "control/Services.hpp"

namespace taskpilot
{

/// Owns the listening socket and one thread per live connection.
///
/// A session is joined and dropped the moment its thread finishes, so what the
/// server retains is one session per CONCURRENT connection, never one per
/// connection it has served. That distinction is the whole reason the reaping
/// exists: an exited thread that has never been joined keeps its stack mapping
/// alive, and both clients here connect per call (see ControlClient.hpp), so
/// retention proportional to connections served would grow with every tool call
/// the daemon ever answered.
class JsonRpcServer
{
  public:
    JsonRpcServer(std::string bind_address, std::uint16_t port,
                  MethodRegistry registry, std::string version);

    ~JsonRpcServer();

    // Non-copyable, non-movable: owns threads and a socket.
    JsonRpcServer(const JsonRpcServer &) = delete;
    JsonRpcServer &operator=(const JsonRpcServer &) = delete;
    JsonRpcServer(JsonRpcServer &&) = delete;
    JsonRpcServer &operator=(JsonRpcServer &&) = delete;

    /// Bind and start accepting. Fails with a message naming the address when
    /// the port is taken — the common case when a daemon is already running,
    /// which the caller reports as "already running" rather than a raw errno.
    [[nodiscard]] Status start();

    /// Stop accepting, close every session, join all threads. Idempotent.
    void stop();

    /// The bound port. Meaningful after start(); when constructed with port 0
    /// the OS picks a free port and this reports the actual one, which is how
    /// the integration test gets a socket without hard-coding a port.
    [[nodiscard]] std::uint16_t port() const { return m_port; }

    /// The number of session records currently retained: one per live
    /// connection, plus any connection that has just ended and has not been
    /// reaped yet. It exists so the property above can be asserted directly:
    /// after a long run of one-connection-per-call cycles this must stay
    /// bounded, not climb with the cycle count (the check belongs in
    /// tests/integration/test_daemon_roundtrip.cpp, which has a socket to drive
    /// those cycles through).
    ///
    /// Thread-safe: it takes m_sessionMutex, so it may be called from any
    /// thread, including while the server is running.
    [[nodiscard]] std::size_t sessionCount() const;

    /// Pure dispatch of one request line to one response line ("" for
    /// notifications). Exposed for unit tests.
    [[nodiscard]] static std::string dispatchLine(const std::string &line,
                                                  const MethodRegistry &registry);

  private:
    /// One accepted connection: its thread, its socket, and the flag the thread
    /// raises as its last act.
    ///
    /// The three live together so a finished session can be reaped as a unit —
    /// join the thread, drop the record, release the socket — rather than as
    /// parallel containers that a reaping path would have to keep in step. A
    /// record exists before its thread can touch anything and until that thread
    /// has been joined; the .cpp enforces both ends of that (see
    /// reapFinishedSessions()).
    struct Session
    {
        /// The connection. Kept alive by this reference for as long as the
        /// thread may still use it; only the session's own thread ever closes
        /// it (see the ownership rule in the .cpp).
        std::shared_ptr<asio::ip::tcp::socket> socket;

        /// Raised by the session thread once it will never touch the socket or
        /// the session bookkeeping again, and read by the reaper as permission
        /// to join that thread without blocking. A shared_ptr rather than a
        /// plain atomic so the flag cannot die while the thread that writes it
        /// is still running.
        std::shared_ptr<std::atomic<bool>> done;

        /// The session thread. Never detached: only a join proves the thread is
        /// past its last use of the socket, which is what makes dropping the
        /// socket reference above safe.
        std::thread thread;
    };

    void acceptLoop();
    void handleSession(std::shared_ptr<asio::ip::tcp::socket> socket);

    /// Join and drop every session whose thread has already finished. Called by
    /// acceptLoop() alone — on every pass, including an idle wake-up, so a
    /// session that ends while no client is waiting is not retained
    /// indefinitely.
    void reapFinishedSessions();

    std::string m_bindAddress;
    std::uint16_t m_port;
    MethodRegistry m_registry;
    std::string m_version;

    asio::io_context m_ioCtx;
    asio::ip::tcp::acceptor m_acceptor;
    std::thread m_acceptThread;
    std::atomic<bool> m_running{ false };

    /// Guards m_sessions: the accept loop's registration and reaping, stop()'s
    /// shutdown sweep over the sockets in it, and each session's own final
    /// close. mutable so the const query sessionCount() can lock it too.
    mutable std::mutex m_sessionMutex;
    std::vector<Session> m_sessions;
};

} // namespace taskpilot
