// main.cpp — the taskPilot executable: argument parsing, wiring, signal loop.
//
// One binary carries four subcommands so the daemon and the MCP bridge can
// never be different versions. That is not cosmetic: `mcp` forwards every
// tools/call to a running `serve` and publishes a tool catalog that is
// compiled into this file's own image, so a mismatched pair would advertise
// methods the daemon cannot dispatch.
//
//   serve    the daemon: opens the store, serves the control socket
//            (the DEFAULT subcommand — run.sh and the MCP registration both
//            invoke the binary with no arguments)
//   mcp      stdio MCP bridge; talks to a running daemon
//   cli      interactive REPL; talks to a running daemon
//   version  print the version
//
// Exit codes: 0 success, 1 runtime failure (store, socket, missing daemon),
// 2 usage error (unknown command or flag, missing or out-of-range port). The
// split matters because run.sh runs under `set -e`.
//
// STDOUT POLICY: stdout carries a PRODUCT, never a diagnostic. `version` is
// the only subcommand that writes there. In `mcp` mode stdout IS the MCP
// protocol stream, where one stray line corrupts the framing and the host
// drops the server with no error the user can act on — so every log line,
// warning, and banner in this file goes to stderr.
//
// This file stays thin on purpose: parsing, wiring, and the signal loop. Every
// rule about tasks, ranking, and persistence lives in the layers below it.

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "control/ControlClient.hpp"
#include "control/JsonRpcServer.hpp"
#include "control/McpServer.hpp"
#include "control/Repl.hpp"
#include "control/Services.hpp"
#include "core/Clock.hpp"
#include "core/Result.hpp"
#include "core/TaskStore.hpp"

namespace taskpilot
{
namespace
{

/// Version reported by `version`, by get_status, and in the MCP serverInfo.
/// Mirrors PROJECT_VERSION in CMakeLists.txt — bump both together.
constexpr const char *kVersion{ "0.1.0" };

/// Human-facing program name. The MCP serverInfo name is deliberately the
/// lowercase "taskpilot" the header defaults to, since it is an identifier a
/// client may key on rather than display text.
constexpr const char *kProgramName{ "taskPilot" };

/// Control socket port when neither the environment nor a flag says otherwise.
constexpr std::uint16_t kDefaultPort{ 8091 };

/// The largest legal TCP port. Named so the range check reads as the protocol
/// rule it is instead of a magic number.
constexpr unsigned long kMaxPort{ 65535UL };

/// Effective configuration.
///
/// Precedence, lowest to highest: the defaults below, then the environment,
/// then the command line — see parseInvocation(). Flags win so that a one-off
/// `--port` in a terminal beats whatever a shell profile exported, which is
/// the order every other CLI the user already runs follows.
struct Config
{
    /// SQLite file. A relative path resolves against the working directory,
    /// which run.sh pins to the repository root.
    std::string db_path{ "data/taskpilot.db" };

    /// TCP port of the control socket.
    std::uint16_t port{ kDefaultPort };

    /// Address the control socket binds to (`serve`) or connects to (`mcp`,
    /// `cli`).
    ///
    /// LOOPBACK BY DEFAULT because the socket has NO AUTHENTICATION. It is a
    /// single-user personal tool, so anything that can reach the port can
    /// read and rewrite the whole backlog; binding the default to a routable
    /// interface would hand that write access to every host on the network,
    /// including the coffee-shop one. --bind is the explicit, deliberate
    /// opt-in to that risk.
    std::string bind_address{ "127.0.0.1" };
};

enum class Command
{
    kServe,
    kMcp,
    kCli,
    /// Named kPrintVersion rather than kVersion to stay clear of the
    /// kVersion string constant, which -Wshadow would flag as a collision.
    kPrintVersion,
};

/// A fully parsed command line.
struct Invocation
{
    Command command{ Command::kServe };
    Config config;

    /// -h/--help was given: print usage for `command` and exit 0.
    bool help{ false };

    /// False means `error` explains the problem: print it plus usage, exit 2.
    bool valid{ false };

    std::string error;
};

/// Set by the signal handler below and polled by the serve wait loop.
///
/// Global because a signal handler cannot receive user data. Relaxed ordering
/// is sufficient and correct here: the flag carries no data alongside it — it
/// only ever says "somebody asked us to stop" — so no other memory has to be
/// made visible through it.
std::atomic<bool> g_stop_requested{ false };

/// SIGINT/SIGTERM handler.
///
/// It does exactly ONE thing — set the flag — because a handler can run at any
/// point on any thread: allocating, locking, logging, or writing to a stream
/// from inside it can deadlock against the very code it interrupted. The real
/// shutdown (joining the accept and session threads, closing the sockets)
/// happens on the main thread, which notices the flag within one poll
/// interval. A relaxed store to a lock-free atomic is async-signal-safe on
/// every platform taskPilot targets.
extern "C" void handleStopSignal(int /*signal_number*/)
{
    g_stop_requested.store(true, std::memory_order_relaxed);
}

/// Read an environment variable, treating an EMPTY value as unset: an empty
/// TASKPILOT_DB is overwhelmingly more likely a shell variable that expanded
/// to nothing than a request for a nameless database.
[[nodiscard]] std::optional<std::string> environmentValue(const char *name)
{
    const char *raw = std::getenv(name);
    if (nullptr == raw)
    {
        return std::nullopt;
    }

    const std::string value(raw);
    if (value.empty())
    {
        return std::nullopt;
    }

    return value;
}

/// Parse a decimal port number into `out`.
///
/// Parsing into an UNSIGNED type is the validation: from_chars then rejects a
/// leading '-' and any non-digit character for free, which is exactly the
/// "a non-numeric port is a usage error" rule, and `result.ptr != last`
/// rejects trailing junk ("8091abc", "80 91"). The range check happens on the
/// wide type so the narrowing cast is provably safe.
[[nodiscard]] bool parsePortNumber(const std::string &text, std::uint16_t &out)
{
    unsigned long value = 0;
    const char *first = text.data();
    const char *last = first + text.size();

    const std::from_chars_result parsed = std::from_chars(first, last, value);
    if (std::errc{} != parsed.ec || parsed.ptr != last)
    {
        return false;
    }

    // Port 0 is legal for a BIND (the OS picks a free port, which is how the
    // integration test avoids a fixed port) but not for a client to connect
    // to, and "connect to port 0" is never what a user meant to type.
    if (value < 1UL || value > kMaxPort)
    {
        return false;
    }

    out = static_cast<std::uint16_t>(value);
    return true;
}

/// Map the subcommand word onto its enum. Returns false for an unknown word so
/// the caller reports it as a usage error instead of guessing at a default.
[[nodiscard]] bool parseCommand(const std::string &text, Command &out)
{
    if ("serve" == text)
    {
        out = Command::kServe;
        return true;
    }
    if ("mcp" == text)
    {
        out = Command::kMcp;
        return true;
    }
    if ("cli" == text)
    {
        out = Command::kCli;
        return true;
    }
    if ("version" == text)
    {
        out = Command::kPrintVersion;
        return true;
    }

    return false;
}

/// Overall usage, then the paragraph for `command` specifically — the caller
/// asked about one subcommand, and a wall of text for all four buries the
/// answer.
void printUsage(std::ostream &out, Command command, bool general_only)
{
    out << "usage: " << kProgramName << " <serve|mcp|cli|version> [options]\n"
        << "\n"
        << "  serve     run the daemon: opens the task store and serves the control socket\n"
        << "            (the default when no command is given)\n"
        << "  mcp       run the stdio MCP bridge; forwards every call to a running daemon\n"
        << "  cli       attach an interactive console to a running daemon\n"
        << "  version   print the version and exit\n"
        << "\n"
        << "options:\n"
        << "  --db <path>       task database (serve only)      [TASKPILOT_DB]\n"
        << "                    default: data/taskpilot.db, created if missing\n"
        << "  --port <n>        control socket port, 1..65535   [TASKPILOT_PORT]\n"
        << "                    default: " << kDefaultPort << "\n"
        << "  --bind <addr>     bind (serve) or connect (mcp, cli) address\n"
        << "                    [TASKPILOT_BIND]  default: 127.0.0.1\n"
        << "  -h, --help        show this help\n";

    if (general_only)
    {
        return;
    }

    out << "\nexamples:\n"
        << "  ./run.sh serve                     # daemon on 127.0.0.1:" << kDefaultPort << "\n"
        << "  ./run.sh serve --port 9000 --db /tmp/backlog.db\n"
        << "  ./run.sh mcp                       # what Claude Code spawns\n"
        << "  ./run.sh cli                       # human REPL against the daemon\n";

    if (Command::kServe == command)
    {
        out << "\nserve prints its listening address and database path on stderr and runs in\n"
            << "the foreground until SIGINT (Ctrl-C) or SIGTERM.\n";
    }
    else if (Command::kCli == command)
    {
        out << "\ncli exits non-zero when no daemon is listening; start one with ./run.sh serve.\n";
    }
    else if (Command::kMcp == command)
    {
        out << "\nmcp speaks the MCP protocol on stdin/stdout and logs to stderr only.\n";
    }
}

/// Parse the whole command line.
///
/// Order of work:
/// 1. Seed the configuration from the environment.
/// 2. Read the subcommand word (argv[1]), defaulting to `serve`.
/// 3. Let the flags override whatever the environment supplied.
///
/// A malformed value from EITHER source is reported, never silently replaced
/// by the default: a daemon quietly listening on the wrong port is far harder
/// to diagnose than one that refused to start.
[[nodiscard]] Invocation parseInvocation(int argc, char **argv)
{
    Invocation invocation;

    invocation.config.db_path = environmentValue("TASKPILOT_DB").value_or(Config{}.db_path);
    invocation.config.bind_address = environmentValue("TASKPILOT_BIND").value_or(Config{}.bind_address);

    if (const std::optional<std::string> port_text = environmentValue("TASKPILOT_PORT");
        port_text.has_value())
    {
        if (!parsePortNumber(*port_text, invocation.config.port))
        {
            invocation.error = "TASKPILOT_PORT is not a port number (1..65535): " + *port_text;
            return invocation;
        }
    }

    int index = 1;
    if (argc > 1)
    {
        const std::string first(argv[1]);
        if ("-h" == first || "--help" == first)
        {
            invocation.help = true;
            invocation.valid = true;
            return invocation;
        }
        if (!parseCommand(first, invocation.command))
        {
            invocation.error = "unknown command: " + first;
            return invocation;
        }
        index = 2;
    }

    for (; index < argc; ++index)
    {
        const std::string flag(argv[index]);

        if ("-h" == flag || "--help" == flag)
        {
            invocation.help = true;
            invocation.valid = true;
            return invocation;
        }

        if ("--db" != flag && "--port" != flag && "--bind" != flag)
        {
            invocation.error = "unknown option: " + flag;
            return invocation;
        }

        // A flag whose value is missing is a usage error, not a reason to fall
        // back to the default: the user believed they had overridden it.
        if (index + 1 >= argc)
        {
            invocation.error = "missing value for " + flag;
            return invocation;
        }

        const std::string value(argv[index + 1]);
        ++index;

        if ("--port" == flag)
        {
            if (!parsePortNumber(value, invocation.config.port))
            {
                invocation.error = "not a port number (1..65535): " + value;
                return invocation;
            }
            continue;
        }

        if (value.empty())
        {
            invocation.error = "empty value for " + flag;
            return invocation;
        }

        if ("--db" == flag)
        {
            invocation.config.db_path = value;
        }
        else
        {
            invocation.config.bind_address = value;
        }
    }

    invocation.valid = true;
    return invocation;
}

/// Create the parent directory of `db_path` when it does not exist yet.
///
/// SQLite creates the database FILE but never the directory above it, and the
/// default path (data/taskpilot.db) lives in a directory a fresh clone does
/// not have — so without this step the very first `./run.sh serve` fails with
/// a bare "unable to open database file".
[[nodiscard]] bool ensureParentDirectory(const std::string &db_path, std::string &error)
{
    // ":memory:" and any URI-style path has no filesystem parent.
    if (db_path.empty() || ':' == db_path.front())
    {
        return true;
    }

    const std::filesystem::path parent = std::filesystem::path(db_path).parent_path();
    if (parent.empty())
    {
        // A bare filename means the current directory, which exists.
        return true;
    }

    std::error_code failure;
    std::filesystem::create_directories(parent, failure);
    if (failure)
    {
        error = "cannot create directory " + parent.string() + ": " + failure.message();
        return false;
    }

    return true;
}

/// `serve` — run the daemon until a stop signal arrives.
[[nodiscard]] int runServe(const Config &config)
{
    std::string directory_error;
    if (!ensureParentDirectory(config.db_path, directory_error))
    {
        std::cerr << kProgramName << ": " << directory_error << "\n";
        return 1;
    }

    Result<std::unique_ptr<TaskStore>> opened = TaskStore::open(config.db_path);
    if (!opened.ok())
    {
        std::cerr << kProgramName << ": cannot open " << config.db_path << ": "
                  << opened.error().message << "\n";
        return 1;
    }

    // Declaration order is lifetime: the store outlives the services and the
    // server, which both hold references into it. A dangling store here would
    // surface as a crash inside a connection thread, not as a startup error.
    const std::unique_ptr<TaskStore> store = std::move(opened).value();

    // The real clock: the daemon ranks against wall time. Tests that need a
    // fixed instant construct Services themselves with a FixedClock.
    SystemClock clock;
    Services services(*store, clock, kVersion);

    JsonRpcServer server(config.bind_address, config.port, services.registry(), kVersion);

    // Handlers go in BEFORE start() so a Ctrl-C during startup is handled
    // cleanly rather than killing the process between bind and listen.
    g_stop_requested.store(false, std::memory_order_relaxed);
    if (SIG_ERR == std::signal(SIGINT, handleStopSignal)
        || SIG_ERR == std::signal(SIGTERM, handleStopSignal))
    {
        // Worth saying out loud: with no handler installed, the wait loop below
        // has no way out and the only remaining way to stop this process is
        // SIGKILL, which skips the clean shutdown entirely.
        std::cerr << kProgramName << ": warning: cannot install the stop-signal handlers;"
                  << " this process will have to be killed with SIGKILL\n";
    }

    const Status started = server.start();
    if (!started.ok())
    {
        // The message from start() already names the address, since "port
        // taken" is overwhelmingly the case where a daemon is already up.
        std::cerr << kProgramName << ": " << started.error().message << "\n";
        std::cerr << kProgramName << ": if a daemon is already running, attach to it with"
                  << " ./run.sh cli instead of starting a second one\n";
        return 1;
    }

    // Banner on STDERR: stdout belongs to products, not to logs (see the
    // stdout policy at the top of this file).
    std::cerr << kProgramName << " " << kVersion << " listening on " << config.bind_address << ":"
              << server.port() << " (db: " << store->path() << ")\n";
    std::cerr << kProgramName << ": press Ctrl-C to stop\n";

    // Wait loop. Polling a flag is the whole shutdown protocol because the
    // handler must not do real work; 200 ms is short enough to feel immediate
    // and long enough to cost nothing while idle.
    while (!g_stop_requested.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cerr << kProgramName << ": stopping\n";
    server.stop();
    return 0;
}

/// `mcp` — stdio MCP bridge over the control socket.
[[nodiscard]] int runMcp(const Config &config)
{
    const ControlClient client(config.bind_address, config.port);

    // Every tools/call is forwarded to the daemon, which stays the single
    // writer. The bridge owning no store is what makes a restarted `mcp`
    // process impossible to diverge from the daemon's view.
    McpServer::Backend backend = [&client](const std::string &method, const nlohmann::json &params)
    { return client.call(method, params); };

    McpServer server(std::move(backend), ServerInfo{ "taskpilot", kVersion });

    // Liveness probe. When the daemon is missing we say so on stderr and RUN
    // ANYWAY: tools/list is built from the compiled-in method catalog, so the
    // client still discovers every tool and sees the real error on the call.
    // Exiting here instead would present the user with an empty tool list and
    // no explanation at all.
    if (!client.ping())
    {
        std::cerr << kProgramName << ": warning: no daemon is listening on " << config.bind_address
                  << ":" << config.port << "\n";
        std::cerr << kProgramName << ": start one with ./run.sh serve - tools/list still works, but"
                  << " every tools/call will fail until the daemon is up\n";
    }

    server.run(std::cin, std::cout);
    return 0;
}

/// `cli` — interactive console against a running daemon.
[[nodiscard]] int runCli(const Config &config)
{
    const ControlClient client(config.bind_address, config.port);

    // A REPL without a daemon has nothing to talk to, so this is the one place
    // the missing daemon is fatal: fail now with the command that fixes it,
    // rather than presenting a prompt where every command errors.
    if (!client.ping())
    {
        std::cerr << kProgramName << ": no daemon is listening on " << config.bind_address << ":"
                  << config.port << "\n";
        std::cerr << kProgramName << ": start one with ./run.sh serve, then run this again\n";
        return 1;
    }

    Repl::Caller caller = [&client](const std::string &method, const nlohmann::json &params)
    { return client.call(method, params); };

    // The REPL's table IS this subcommand's product, so it belongs on stdout —
    // that is what makes `taskPilot cli < commands.txt | less` usable, and this
    // process never speaks the MCP protocol.
    Repl repl(std::move(caller), std::cout);
    repl.run(std::cin);
    return 0;
}

/// `version` — print the version and exit.
[[nodiscard]] int runVersion()
{
    // The one documented use of stdout: a version string is a machine-readable
    // product (scripts and MCP configs parse it), not a diagnostic.
    std::cout << kProgramName << " " << kVersion << "\n";
    return 0;
}

} // namespace
} // namespace taskpilot

int main(int argc, char **argv)
{
    const taskpilot::Invocation invocation = taskpilot::parseInvocation(argc, argv);

    if (invocation.help)
    {
        taskpilot::printUsage(std::cout, invocation.command, false);
        return 0;
    }

    if (!invocation.valid)
    {
        std::cerr << taskpilot::kProgramName << ": " << invocation.error << "\n";
        taskpilot::printUsage(std::cerr, invocation.command, true);
        return 2;
    }

    switch (invocation.command)
    {
    case taskpilot::Command::kServe:
        return taskpilot::runServe(invocation.config);
    case taskpilot::Command::kMcp:
        return taskpilot::runMcp(invocation.config);
    case taskpilot::Command::kCli:
        return taskpilot::runCli(invocation.config);
    case taskpilot::Command::kPrintVersion:
        return taskpilot::runVersion();
    }

    // Unreachable: every enumerator is handled above. A trailing failure is
    // deliberately preferred to a `default:` label, which would silently
    // swallow a newly added subcommand instead of letting -Wswitch flag it.
    std::cerr << taskpilot::kProgramName << ": unhandled command\n";
    return 1;
}
