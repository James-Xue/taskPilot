// test_control_client.cpp — transport failure modes of the control-socket client
//
// The client's whole promise is that a broken daemon produces a readable error
// instead of a hang, and ControlClient.hpp spells out why: without a read
// timeout "every MCP tool call would hang until the host client gives up, which
// presents to the user as Claude Code freezing rather than as an error". A
// promise about what happens when the peer misbehaves cannot be checked against
// an injected fake caller, because the client opens its own socket and races
// every step against a real timer. So each case here puts a REAL listener on the
// other end, played by FakeServer below, and asserts on the error text the user
// would actually see:
//
//   header failure mode                script        assertion
//   1. cannot connect                  closed port   kStorageFailure + ./run.sh serve
//   2. timeout reading the reply       kStall        kInternal + the timeout named
//   3. peer closed mid-read            kClose        kInternal + the address named
//   4. JSON-RPC `error` reply          covered end to end by
//                                    tests/integration/test_daemon_roundtrip.cpp
//   5. malformed reply                 kJunkReply    kInternal + an excerpt
//   6. reply over the byte cap         kBigReply     kInternal + the bound named
//                                    (and, at a raised cap, every byte of it)
//
// These are unit tests in the sense that matters: no daemon, no store, no temp
// files. The only peer is a scripted listener inside this process, on the
// loopback interface.
//
// Two rules shape the file:
//
//   1. NO FIXED PORT. Every listener binds port 0 and reads the assigned port
//      back, so the suite can run beside the user's live daemon and in parallel
//      with itself.
//
//   2. NO UNBOUNDED WAIT. The regression each case guards against is "the call
//      never returns", so every call runs on a worker thread under an outer
//      deadline: a client that dropped its timeout fails the test by name in
//      seconds instead of parking the suite until the CTest timeout, which is
//      what a test that hangs on failure is worth.
//
// Every asio call below is written as `std::ignore = call(...)` because asio's
// error_code overloads hand the result back TWICE: they fill the caller's out
// parameter and return the same code, as ASIO_SYNC_OP_VOID expands to
// asio::error_code unless the build defines ASIO_NO_DEPRECATED. The out
// parameter is what these fixtures read — either immediately or at the end of a
// short sequence of setup calls — so the returned copy is redundant rather than
// missed, and the assignment says so. A bare `call(...);` cannot: it reads as a
// swallowed error, which is what clang-tidy's bugprone-unused-return-value is
// right to flag, and what a `(void)` cast fails to fix (that check's
// AllowCastToVoid option defaults to off).

#include "control/ControlClient.hpp"

#include <gtest/gtest.h>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/write.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>

#include "control/Services.hpp"
#include "core/Result.hpp"

namespace taskpilot
{
namespace
{

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// The interface every socket in this file uses. Both the listener and the
/// client must agree on it, so it is named once rather than repeated.
constexpr const char *kLoopback{ "127.0.0.1" };

/// Port 0 means "ask the OS for a free port"; the assigned one is read back.
/// Nothing here is ever hard-coded, for the reason in the file header.
constexpr std::uint16_t kAnyFreePort{ 0 };

/// Client timeout used wherever the value itself is not the subject. Short
/// enough that a stalled peer answers within the test, long enough that a
/// loopback connect on a loaded machine cannot plausibly race it.
constexpr std::int64_t kShortTimeoutMs{ 300 };

/// Timeout for the ping cases, where the assertion is that no reply is awaited
/// at all: the bound has to be comfortably larger than a connect so that
/// "returned quickly" and "waited out the timeout" cannot be confused.
constexpr std::int64_t kPingTimeoutMs{ 2000 };

/// Outer bound on one client call, the deadline runWithDeadline enforces. Over
/// sixteen times kShortTimeoutMs, so it can only fire when the client itself is
/// broken, never because a machine was busy.
constexpr std::chrono::milliseconds kCallDeadline{ 5000 };

/// How long the server thread sleeps between polls. Also the worst-case
/// shutdown latency, which is why it is this short.
constexpr std::chrono::milliseconds kPollInterval{ 2 };

/// Body of the junk reply: valid bytes, one line, not JSON. The newline is
/// added at the socket, so this same text is also what the excerpt assertions
/// look for inside the error message.
constexpr const char *kJunkReplyLine{ "not json at all" };

/// Markers at the two ENDS of the kBigReply payload.
///
/// The markers are what make "the whole reply arrived" checkable. A length
/// alone cannot tell a reply that lost its head from one that lost its tail,
/// nor either from one that never carried the payload at all; two markers, one
/// at each end, separate those cases from each other.
constexpr const char *kBigReplyBegin{ "BEGIN-OF-BIG-REPLY" };
constexpr const char *kBigReplyEnd{ "END-OF-BIG-REPLY" };

/// The default reply cap, written out as a literal rather than read from
/// kDefaultMaxReplyBytes.
///
/// Pinned on purpose, and the case that uses it is the reason: one of these
/// exists to catch a SILENT move of the default, and a test that took its
/// expected number from the same constant the client reads would follow such a
/// move instead of failing it. Editing this literal is the deliberate act that
/// says "the default really changed", and it has to be made together with the
/// header.
constexpr std::size_t kPinnedDefaultCapBytes{ 1024 * 1024 };

/// Filler bytes between the markers in the kBigReply payload: more than the
/// DEFAULT cap, so that a client built with that cap must refuse the reply and
/// one built with a raised cap must return every byte of it. Derived from the
/// pinned number, not from the header, for the reason above.
constexpr std::size_t kBigReplyFillerBytes{ kPinnedDefaultCapBytes + 512 * 1024 };

/// Filler for the case that checks the CONFIGURED bound, which is small: a cap
/// is a cap at any size, and 64 KiB overruns it without writing megabytes to
/// prove a point.
constexpr std::size_t kSmallBoundFillerBytes{ 64 * 1024 };

/// The cap the raised-cap case configures: twice the reply's payload, so the
/// cap cannot be the thing that truncates anything, and comfortably above the
/// default, so it is a ceiling that case really had to ask for.
constexpr std::size_t kRaisedReplyBytes{ 2 * kBigReplyFillerBytes };

/// The cap the configured-bound case configures. Deliberately not derived from
/// the default, so a message that named the default instead of the configured
/// value cannot pass.
constexpr std::size_t kTightReplyBytes{ 4 * 1024 };

/// Timeout for the case that must transfer the whole big reply. Comfortably
/// longer than kShortTimeoutMs and still well under kCallDeadline, because that
/// case is about the cap rather than about speed and a megabyte and a half is
/// real work for a loaded machine.
constexpr std::int64_t kBigReplyTimeoutMs{ 2000 };

/// Chunk size for the fixture's big writes. Small enough that a client which
/// gives up mid-line is noticed at the next chunk rather than after the whole
/// reply has been handed to the kernel in one call.
constexpr std::size_t kWriteChunkBytes{ 16 * 1024 };

// ---------------------------------------------------------------------------
// Support
// ---------------------------------------------------------------------------

/// One-line rendering of an error for assertion messages.
///
/// ErrorCode is a scoped enum, so it does not stream and this gtest has no enum
/// printer: without this a failure would read "expected kStorageFailure, got
/// 4-byte object <09 00 00 00>", which names the wrong thing. The code goes out
/// through toString(), the same stable name the wire carries.
[[nodiscard]] std::string describe(const Error &error)
{
    return toString(error.code) + ": " + error.message;
}

/// Run `work` on a worker thread and return its value, or an empty optional
/// when it had not finished within `deadline`.
///
/// This is what keeps a regression from becoming a hung suite. The worker is
/// deliberately DETACHED rather than joined when it overruns, because the only
/// way to get here is a call that ignored its own deadline: joining a thread
/// parked in an unbounded read is precisely the hang this helper exists to
/// prevent. Detaching is safe because the worker owns everything it touches —
/// the closure captures values only, and the promise below is kept alive by a
/// shared_ptr the worker holds — so the assertion at the call site still fails
/// the test, by name, while the orphan finishes (or not) on its own.
template <typename Fn>
[[nodiscard]] auto runWithDeadline(Fn work, std::chrono::milliseconds deadline)
    -> std::optional<std::invoke_result_t<Fn>>
{
    using Value = std::invoke_result_t<Fn>;

    auto promise = std::make_shared<std::promise<Value>>();
    std::future<Value> future = promise->get_future();

    std::thread worker(
        [promise, task = std::move(work)]() mutable
        {
            try
            {
                promise->set_value(task());
            }
            catch (...)
            {
                // An exception escaping a thread body calls std::terminate,
                // which would take the whole suite down instead of failing the
                // one test that provoked it.
                promise->set_exception(std::current_exception());
            }
        });

    if (std::future_status::ready != future.wait_for(deadline))
    {
        worker.detach();
        return std::nullopt;
    }

    std::optional<Value> value = future.get();
    worker.join();
    return value;
}

/// One `call()` against `port`, bounded by kCallDeadline.
///
/// The client is constructed INSIDE the worker and every capture is a copy: in
/// the overrun case the worker outlives this scope, and it must not be able to
/// reach a stack frame that has already been torn down.
[[nodiscard]] std::optional<RpcResult> callWithDeadline(std::uint16_t port,
                                                        std::int64_t timeout_ms,
                                                        const std::string &method)
{
    return runWithDeadline(
        [port, timeout_ms, method]() -> RpcResult
        {
            const ControlClient client(kLoopback, port, timeout_ms);
            return client.call(method, nlohmann::json::object());
        },
        kCallDeadline);
}

/// One `call()` against `port` on a client built the way every pre-cap caller
/// built one: ControlClient(host, port), with BOTH defaults untouched. It exists
/// so the claim "the defaults did not move" is asserted through the old two-
/// argument form — the one the export subcommand itself used — rather than
/// through the three-argument form that merely omits the new parameter.
[[nodiscard]] std::optional<RpcResult> callWithBothDefaults(std::uint16_t port,
                                                            const std::string &method)
{
    return runWithDeadline(
        [port, method]() -> RpcResult
        {
            const ControlClient client(kLoopback, port);
            return client.call(method, nlohmann::json::object());
        },
        kCallDeadline);
}

/// One `call()` against `port` with an explicit reply cap, bounded by
/// kCallDeadline. Same ownership rule as callWithDeadline: the client is
/// constructed inside the worker so that an overrun cannot reach a torn-down
/// stack frame.
[[nodiscard]] std::optional<RpcResult> callWithCap(std::uint16_t port,
                                                   std::int64_t timeout_ms,
                                                   std::size_t max_reply_bytes,
                                                   const std::string &method)
{
    return runWithDeadline(
        [port, timeout_ms, max_reply_bytes, method]() -> RpcResult
        {
            const ControlClient client(kLoopback, port, timeout_ms, max_reply_bytes);
            return client.call(method, nlohmann::json::object());
        },
        kCallDeadline);
}

/// One `ping()` against `port`, bounded by kCallDeadline. Same ownership rule
/// as callWithDeadline.
[[nodiscard]] std::optional<bool> pingWithDeadline(std::uint16_t port, std::int64_t timeout_ms)
{
    return runWithDeadline(
        [port, timeout_ms]()
        {
            const ControlClient client(kLoopback, port, timeout_ms);
            return client.ping();
        },
        kCallDeadline);
}

/// Assert that a failed call carries `expected` as its code and that its
/// message contains every fragment in `fragments`.
///
/// The fragments are the substance of these cases, not decoration: for a
/// transport failure the message IS the feature — it is what the user sees
/// instead of a freeze — so "it failed" is not enough. Mode 1 has to name
/// ./run.sh serve, mode 2 the timeout, and mode 5 an excerpt of what arrived.
void expectErrorMessage(const RpcResult &result, ErrorCode expected,
                        std::initializer_list<std::string> fragments)
{
    EXPECT_TRUE(expected == result.error().code)
        << "expected " << toString(expected) << ", got " << describe(result.error());
    for (const std::string &fragment : fragments)
    {
        EXPECT_NE(std::string::npos, result.error().message.find(fragment))
            << "the message must contain \"" << fragment << "\": " << result.error().message;
    }
}

/// A port this process has just bound and released, so nothing is listening on
/// it: the only way to name a closed port without hard-coding one that a real
/// daemon or another test might hold.
///
/// Returns kAnyFreePort when the reservation itself failed; every caller
/// asserts against that before dialing it.
[[nodiscard]] std::uint16_t reservedThenReleasedPort()
{
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io);
    const asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), kAnyFreePort);

    std::error_code ec;
    std::ignore = acceptor.open(endpoint.protocol(), ec);
    std::ignore = acceptor.bind(endpoint, ec);
    std::ignore = acceptor.listen(asio::socket_base::max_listen_connections, ec);

    std::uint16_t port = kAnyFreePort;
    if (!ec)
    {
        port = acceptor.local_endpoint(ec).port();
    }

    // Released unconditionally: the caller wants the closed state, not the
    // listener, and a reservation that failed has nothing left open anyway.
    std::error_code ignored;
    std::ignore = acceptor.close(ignored);
    return port;
}

/// The exact payload FakeServer::Script::kBigReply puts in its reply: filler
/// between the two markers, which is also what the case that reads it asserts
/// against.
[[nodiscard]] std::string bigReplyPayload(std::size_t filler_bytes)
{
    std::string payload(kBigReplyBegin);
    payload.append(filler_bytes, 'x');
    payload.append(kBigReplyEnd);
    return payload;
}

/// One well-formed JSON-RPC reply carrying that payload under "blob", and its
/// newline. Built with the JSON library rather than by hand so that the only
/// thing that can be wrong with it is its size — the case under test must fail
/// on the cap and not on a hand-escaped brace.
[[nodiscard]] std::string bigReplyLine(std::size_t filler_bytes)
{
    const nlohmann::json reply{
        { "jsonrpc", "2.0" },
        { "id", 1 },
        { "result", { { "blob", bigReplyPayload(filler_bytes) } } },
    };
    return reply.dump() + "\n";
}

/// Write `text` to `peer` in bounded chunks, stopping at the first error.
///
/// A client whose cap is smaller than the reply closes the connection while
/// this is still writing, so a failed send is an expected END to the case under
/// test rather than a fault in the fixture: the client has already received
/// what it came for — an error naming its cap — and the loop stops there
/// instead of reporting a peer that did exactly what it should.
///
/// Chunked rather than one asio::write because noticing that the peer has gone
/// away is the whole point: a single call hands the entire reply to the kernel,
/// where there is nothing left to stop at and nothing to learn.
void writeUntilTheClientGivesUp(asio::ip::tcp::socket &peer, const std::string &text)
{
    std::size_t sent_total = 0;
    while (sent_total < text.size())
    {
        const std::size_t chunk =
            std::min<std::size_t>(kWriteChunkBytes, text.size() - sent_total);
        std::error_code write_ec;
        const std::size_t sent =
            peer.write_some(asio::buffer(text.data() + sent_total, chunk), write_ec);
        sent_total += sent;
        if (write_ec || 0 == sent)
        {
            // A failed send, or (defensively — a blocking socket should not
            // produce it) one that wrote nothing without failing, means there
            // is no peer left to write to.
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// FakeServer — the peer the client's failure modes are defined against
// ---------------------------------------------------------------------------

/// A throwaway loopback listener that plays one fixed script per connection.
///
/// The client's failure modes are all about what the peer does AFTER the
/// connection exists, and none of them can be faked: "connected but silent",
/// "closed mid-read" and "answered with garbage" only exist if something real
/// is on the other end of a real socket.
///
/// Lifetime is the other half of the design. The acceptor, the peer socket and
/// the thread all live here, the thread owns every socket object it touches
/// (nothing is closed from another thread — on Linux that neither reliably
/// wakes a thread blocked in the call nor avoids racing the socket object), and
/// the destructor raises the stop flag and joins. An assertion that unwinds the
/// test body early therefore still leaves nothing running behind it.
class FakeServer
{
  public:
    /// What the server does with a connection it has accepted.
    enum class Script
    {
        kStall,     ///< Hold the socket open, write nothing: cause of mode 2.
        kClose,     ///< Close at once without replying: cause of mode 3.
        kJunkReply, ///< Read the request line, answer non-JSON: cause of mode 5.
        /// Read the request line, then answer with one valid reply whose payload
        /// is a megabyte and a half: cause of mode 6, and the only reply here
        /// that a caller has to ask for a bigger cap to read whole.
        kBigReply,
    };

    /// `reply_filler_bytes` is the payload size for kBigReply and is ignored by
    /// the other scripts, which write fixed bytes. It is a constructor
    /// parameter rather than a constant inside the script so that one case can
    /// overrun a SMALL configured cap with a modest reply while the others use
    /// the large one.
    explicit FakeServer(Script script, std::size_t reply_filler_bytes = kBigReplyFillerBytes)
        : m_io(),
          m_acceptor(m_io),
          m_script(script),
          m_replyFillerBytes(reply_filler_bytes)
    {
        const asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), kAnyFreePort);

        std::error_code ec;
        std::ignore = m_acceptor.open(endpoint.protocol(), ec);
        std::ignore = m_acceptor.bind(endpoint, ec);
        std::ignore = m_acceptor.listen(asio::socket_base::max_listen_connections, ec);
        if (ec)
        {
            // A loopback bind cannot fail for a reason a test can act on, and
            // gtest reports an exception thrown out of a test body, so this
            // throws rather than growing an error path every case must check.
            throw std::runtime_error("fake control server could not listen on loopback: " +
                                     ec.message());
        }

        m_port = m_acceptor.local_endpoint(ec).port();

        // Non-blocking so the accept loop can poll the stop flag: a blocking
        // accept() parked on a port nobody dials cannot be woken by close()
        // from another thread, and joining that thread would hang the suite.
        std::ignore = m_acceptor.non_blocking(true, ec);
        if (ec)
        {
            throw std::runtime_error("fake control server could not go non-blocking: " +
                                     ec.message());
        }

        m_thread = std::thread([this]() { acceptLoop(); });
    }

    ~FakeServer()
    {
        stop();
    }

    // The thread captures `this` and the sockets are owned by it, so a copy
    // would leave both objects fighting over one acceptor.
    FakeServer(const FakeServer &) = delete;
    FakeServer &operator=(const FakeServer &) = delete;

    [[nodiscard]] std::uint16_t port() const { return m_port; }

    /// The address the client dials, in the same host:port shape the client's
    /// own error messages use — which is what the cases assert against.
    [[nodiscard]] std::string address() const
    {
        return std::string(kLoopback) + ":" + std::to_string(m_port);
    }

  private:
    /// Accept connections until stop() asks the loop to leave.
    void acceptLoop()
    {
        for (;;)
        {
            if (isStopping())
            {
                break;
            }

            asio::ip::tcp::socket peer(m_io);
            std::error_code ec;
            std::ignore = m_acceptor.accept(peer, ec);
            if (asio::error::would_block == ec || asio::error::try_again == ec)
            {
                // Nobody is waiting yet; kPollInterval is what bounds how soon
                // a stop request is noticed.
                std::this_thread::sleep_for(kPollInterval);
                continue;
            }
            if (ec)
            {
                // The acceptor itself is unusable: there is nothing left to
                // serve and no case that could still pass.
                break;
            }

            playScript(peer);
        }

        // Closed on the thread that owns it, after the loop has decided to
        // leave. The stop flag, not this close, is what ends the loop.
        std::error_code ignored;
        std::ignore = m_acceptor.close(ignored);
    }

    /// Play `m_script` for one accepted connection, then close it.
    void playScript(asio::ip::tcp::socket &peer)
    {
        // Decision tree, one branch per scripted failure mode:
        //   1. kStall     — write nothing and hold the socket open. This is the
        //                   wedged-daemon state the read timeout exists for, so
        //                   NOT reading is the point: the client must give up
        //                   on its own.
        //   2. kClose     — drop the connection immediately, so the client sees
        //                   a close where a reply should be.
        //   3. kJunkReply — wait for the request line (a real daemon reads
        //                   before it writes), then answer with bytes that are
        //                   not JSON.
        //   4. kBigReply  — wait for the request line, then answer with a valid
        //                   reply large enough to overrun the client's default
        //                   cap. Whether the whole line lands depends on the
        //                   cap the client was given, and a client that refuses
        //                   it closes mid-write; that is the case passing, not
        //                   the fixture failing.
        switch (m_script)
        {
            case Script::kStall:
            {
                waitForStop();
                break;
            }
            case Script::kClose:
            {
                break;
            }
            case Script::kJunkReply:
            {
                std::string request;
                std::error_code write_ec;
                if (readRequestLine(peer, request))
                {
                    const std::string reply = std::string(kJunkReplyLine) + "\n";
                    asio::write(peer, asio::buffer(reply), write_ec);
                }
                break;
            }
            case Script::kBigReply:
            {
                std::string request;
                if (readRequestLine(peer, request))
                {
                    writeUntilTheClientGivesUp(peer, bigReplyLine(m_replyFillerBytes));
                }
                break;
            }
        }

        // The request line has been consumed before this point, which matters:
        // closing a socket with unread data still queued sends a reset, and a
        // reset can discard the reply the client is about to read.
        std::error_code ignored;
        std::ignore = peer.close(ignored);
    }

    /// Read one '\n'-terminated request line, giving up as soon as stop() is
    /// requested.
    ///
    /// The socket is switched to non-blocking and polled rather than read
    /// synchronously, because a synchronous read cannot be interrupted: on
    /// Linux, closing a socket from another thread does not reliably wake a
    /// thread already blocked in read() on it. A client that never sent its
    /// request would otherwise park this thread past the end of the test and
    /// turn a failed assertion into a suite-wide timeout.
    [[nodiscard]] bool readRequestLine(asio::ip::tcp::socket &peer, std::string &line)
    {
        std::error_code ec;
        std::ignore = peer.non_blocking(true, ec);
        if (ec)
        {
            return false;
        }

        // Cleared once, not per attempt: data that arrived before a would_block
        // is part of the line, and read_until searches what the buffer already
        // holds before asking the socket for more.
        line.clear();
        bool complete = false;
        while (!isStopping())
        {
            ec.clear();
            asio::read_until(peer, asio::dynamic_buffer(line), '\n', ec);
            if (!ec)
            {
                complete = true;
                break;
            }
            if (asio::error::would_block != ec && asio::error::try_again != ec)
            {
                // EOF or a reset: the peer is gone, so there is no line to read.
                break;
            }
            std::this_thread::sleep_for(kPollInterval);
        }

        // Blocking mode is restored before returning, because the caller writes
        // its script on this same socket next and a synchronous asio write
        // gives up at the first would_block instead of finishing the line.
        // Harmless for the short replies, which fit in the send buffer whole,
        // and wrong for kBigReply, which cannot: there it would leave the
        // client a truncated line and turn a case about the cap into a case
        // about a fixture that never sent a newline.
        std::error_code restore_ec;
        std::ignore = peer.non_blocking(false, restore_ec);
        return complete;
    }

    /// Block the script until stop() is called. Only used by kStall, which
    /// holds the connection open for the whole test on purpose.
    void waitForStop()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_stopRequested.wait(lock, [this]() { return m_stopping; });
    }

    [[nodiscard]] bool isStopping()
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_stopping;
    }

    /// Raise the stop flag and wait for the loop thread to leave.
    ///
    /// No socket is closed here: the loop thread owns them all, and a cross-
    /// thread close is a race on the socket object even where it does happen to
    /// interrupt the call.
    void stop()
    {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_stopRequested.notify_all();

        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

    asio::io_context m_io;
    asio::ip::tcp::acceptor m_acceptor;
    Script m_script;
    std::size_t m_replyFillerBytes{ kBigReplyFillerBytes };
    std::uint16_t m_port{ kAnyFreePort };
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_stopRequested;
    bool m_stopping{ false };
};

// ---------------------------------------------------------------------------
// Mode 1: nothing is listening
// ---------------------------------------------------------------------------
TEST(ControlClient, ConnectionRefusedIsAnErrorNamingTheStartCommand)
{
    const std::uint16_t port = reservedThenReleasedPort();
    ASSERT_NE(kAnyFreePort, port) << "no free port could be reserved and released";

    // Failure must arrive as a value, never as a throw: an exception here would
    // leave the test body, and gtest would report the throw itself rather than
    // the error text this case is about. That text is the deliverable — no
    // daemon running is the expected state on a fresh machine, so the failure
    // has to read as a command to run rather than as an errno to decode, and it
    // has to name the refused connection rather than any other way of failing
    // to reach the peer.
    const ControlClient client(kLoopback, port, kShortTimeoutMs);
    const RpcResult result = client.call("get_status", nlohmann::json::object());

    ASSERT_FALSE(result.ok()) << "connecting to a port nobody listens on must fail";
    expectErrorMessage(result, ErrorCode::kStorageFailure,
                       { "cannot connect", "./run.sh serve",
                         std::string(kLoopback) + ":" + std::to_string(port) });
}

// ---------------------------------------------------------------------------
// Mode 2: connected, but the reply never comes — the case the read timeout
// exists for, and the one that would present as Claude Code freezing
// ---------------------------------------------------------------------------
TEST(ControlClient, AStalledPeerTimesOutInsteadOfHanging)
{
    FakeServer server(FakeServer::Script::kStall);

    const std::optional<RpcResult> result =
        callWithDeadline(server.port(), kShortTimeoutMs, "get_status");

    // The outer deadline is half the assertion: a client that stopped arming
    // its read timeout would still be inside call() here, and this fails by
    // name instead of blocking the suite.
    ASSERT_TRUE(result.has_value())
        << "call() had not returned " << kCallDeadline.count()
        << " ms after dialing a peer that accepted and then stayed silent";
    ASSERT_FALSE(result->ok()) << "a silent peer cannot produce a result";
    expectErrorMessage(*result, ErrorCode::kInternal,
                       { "timed out", std::to_string(kShortTimeoutMs) + " ms", "get_status",
                         server.address() });
}

// ---------------------------------------------------------------------------
// Mode 3: the peer goes away instead of answering
// ---------------------------------------------------------------------------
TEST(ControlClient, APeerThatClosesMidReadFailsInsteadOfHanging)
{
    FakeServer server(FakeServer::Script::kClose);

    const std::optional<RpcResult> result =
        callWithDeadline(server.port(), kShortTimeoutMs, "get_status");

    ASSERT_TRUE(result.has_value())
        << "call() had not returned " << kCallDeadline.count()
        << " ms after the peer closed the connection";
    ASSERT_FALSE(result->ok()) << "a peer that closed cannot produce a result";
    expectErrorMessage(*result, ErrorCode::kInternal, { server.address() });

    // Which step notices the close is a race — a reset can land before the
    // request is written or while its reply is awaited — and both paths are the
    // documented kInternal, so either wording passes while a message that says
    // neither still fails.
    const std::string &message = result->error().message;
    const bool noticed_while_reading =
        std::string::npos != message.find("before finishing its reply");
    const bool noticed_while_writing = std::string::npos != message.find("broke while sending");
    EXPECT_TRUE(noticed_while_reading || noticed_while_writing)
        << "the message must say what the peer did: " << message;
}

// ---------------------------------------------------------------------------
// Mode 5: an answer that is not a JSON-RPC response
// ---------------------------------------------------------------------------
TEST(ControlClient, AMalformedReplyIsReportedWithAnExcerpt)
{
    FakeServer server(FakeServer::Script::kJunkReply);

    const std::optional<RpcResult> result =
        callWithDeadline(server.port(), kShortTimeoutMs, "get_status");

    ASSERT_TRUE(result.has_value())
        << "call() had not returned " << kCallDeadline.count()
        << " ms after a malformed reply was written back";
    ASSERT_FALSE(result->ok()) << "garbage must not be parsed into a result";

    // "The daemon returned garbage" is not actionable; "the daemon returned
    // this" is, which is why the header promises the excerpt — so the excerpt
    // is what the assertion is about, not just the error code.
    expectErrorMessage(*result, ErrorCode::kInternal, { "not JSON", kJunkReplyLine });
}

// ---------------------------------------------------------------------------
// Mode 6: a reply over the byte cap. Three cases, because the cap became a
// caller's choice and all three halves of that matter: the default stayed where
// it was, a raised cap really does read the whole reply, and the bound in the
// error is the one this client was given rather than a constant.
//
// This is the export path's failure mode: export_tasks answers with the whole
// backlog, so its reply grows with the backlog and a fixed cap would have made
// the call fail permanently once it was reached — with no way to page a
// document that has to arrive as one line.
// ---------------------------------------------------------------------------
TEST(ControlClient, TheDefaultCapStillRefusesAReplyOverAMegabyte)
{
    FakeServer server(FakeServer::Script::kBigReply, kBigReplyFillerBytes);

    // Constructed with NO cap argument — the two-argument form the export
    // subcommand itself used before the cap was a parameter. Making the cap a
    // parameter is only safe if the default did not move with it: were it
    // raised globally, a peer that answers with a runaway line could cost every
    // caller of every method that memory.
    const std::optional<RpcResult> result =
        callWithBothDefaults(server.port(), "export_tasks");

    ASSERT_TRUE(result.has_value())
        << "call() had not returned " << kCallDeadline.count()
        << " ms after a reply larger than the default cap was written back";
    ASSERT_FALSE(result->ok()) << "a reply over the default cap must not be accepted";
    expectErrorMessage(*result, ErrorCode::kInternal,
                       { "exceeded", std::to_string(kPinnedDefaultCapBytes) + " bytes" });
}

TEST(ControlClient, ARaisedCapReturnsEveryByteOfAReplyOverAMegabyte)
{
    FakeServer server(FakeServer::Script::kBigReply, kBigReplyFillerBytes);

    const std::optional<RpcResult> result =
        callWithCap(server.port(), kBigReplyTimeoutMs, kRaisedReplyBytes, "export_tasks");

    ASSERT_TRUE(result.has_value())
        << "call() had not returned " << kCallDeadline.count()
        << " ms after a reply larger than a megabyte was written back";
    ASSERT_TRUE(result->ok()) << "a reply below the configured cap must be read whole: "
                              << describe(result->error());

    // Content, not merely "no error": a client that stopped at the old cap and
    // returned what it had would pass an error-only assertion while handing the
    // export a truncated line — which is worse than failing, because that file
    // is what the user commits. Both ends are checked because a truncation can
    // be either, and the length because a reply that lost its middle satisfies
    // both markers.
    const std::string blob = result->value().at("blob").get<std::string>();
    const std::size_t expected_size = kBigReplyFillerBytes +
                                      std::string(kBigReplyBegin).size() +
                                      std::string(kBigReplyEnd).size();
    EXPECT_EQ(expected_size, blob.size()) << "the reply did not arrive intact";
    EXPECT_TRUE(blob.starts_with(kBigReplyBegin)) << "the head of the reply was lost";
    EXPECT_TRUE(blob.ends_with(kBigReplyEnd)) << "the tail of the reply was lost";
}

TEST(ControlClient, AReplyOverTheConfiguredCapIsAnErrorNamingThatCap)
{
    FakeServer server(FakeServer::Script::kBigReply, kSmallBoundFillerBytes);

    const std::optional<RpcResult> result =
        callWithCap(server.port(), kShortTimeoutMs, kTightReplyBytes, "export_tasks");

    ASSERT_TRUE(result.has_value())
        << "call() had not returned " << kCallDeadline.count()
        << " ms after a reply over a small configured cap was written back";
    ASSERT_FALSE(result->ok()) << "a reply over the configured cap must fail, whatever the cap is";
    // The BOUND it was given, not a constant: this message is what a caller
    // reads to learn how far it has to raise the ceiling, so it has to name the
    // number this client actually had.
    expectErrorMessage(*result, ErrorCode::kInternal,
                       { "exceeded", std::to_string(kTightReplyBytes) + " bytes" });
}

// ---------------------------------------------------------------------------
// ping(): the cheap liveness probe the CLI and `mcp` use at startup
// ---------------------------------------------------------------------------
TEST(ControlClient, PingSucceedsAgainstASilentPeerWithoutWaitingForAReply)
{
    FakeServer server(FakeServer::Script::kStall);

    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    const std::optional<bool> alive = pingWithDeadline(server.port(), kPingTimeoutMs);
    const std::chrono::milliseconds elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    ASSERT_TRUE(alive.has_value())
        << "ping() had not returned " << kCallDeadline.count() << " ms";
    // A peer that completes the handshake is alive whatever it does next — the
    // probe answers "is something listening", not "is it answering".
    EXPECT_TRUE(*alive) << "a listener that accepts a connection is up, silent or not";

    // Timing is the second half of the contract. A ping that waited for a reply
    // would sit out the whole client timeout before answering, so one that
    // returns in a small fraction of it can only have stopped at the connect.
    EXPECT_LT(elapsed, std::chrono::milliseconds(kPingTimeoutMs))
        << "ping() waited for a reply instead of returning at the connect";
}

TEST(ControlClient, PingFailsWithoutThrowingWhenNothingIsListening)
{
    const std::uint16_t port = reservedThenReleasedPort();
    ASSERT_NE(kAnyFreePort, port) << "no free port could be reserved and released";

    const std::optional<bool> alive = pingWithDeadline(port, kShortTimeoutMs);

    ASSERT_TRUE(alive.has_value()) << "ping() had not returned " << kCallDeadline.count() << " ms";
    EXPECT_FALSE(*alive) << "nothing is listening on a port that was just released";
}

// ---------------------------------------------------------------------------
// The wire-code mapping the header calls the inverse of the server's table
// ---------------------------------------------------------------------------
TEST(ControlClient, EveryErrorCodeRoundTripsThroughItsWireCode)
{
    // fromRpcErrorCode() and rpcErrorCode() are documented as one mapping
    // written down twice, with a standing instruction to change both halves
    // together. A round trip is the assertion that catches a one-sided edit: it
    // fails whichever half moved, which no test of either half alone can do.
    for (const ErrorCode code :
         { ErrorCode::kInvalidArgument, ErrorCode::kNotFound, ErrorCode::kConflict,
           ErrorCode::kStorageFailure, ErrorCode::kInternal })
    {
        EXPECT_TRUE(code == ControlClient::fromRpcErrorCode(rpcErrorCode(code)))
            << "round trip failed for " << toString(code);
    }

    // A code in the application range that neither half assigns (-32022 is
    // McpServer's unsupported-version) must degrade to kInternal rather than
    // being guessed at: an invented classification is worse than an honest
    // "unexpected", because the caller would act on it.
    EXPECT_TRUE(ErrorCode::kInternal == ControlClient::fromRpcErrorCode(-32022));
}

} // namespace
} // namespace taskpilot
