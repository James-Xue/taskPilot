// main.cpp — the taskPilot executable: argument parsing, wiring, signal loop.
//
// One binary carries six subcommands so the daemon and the MCP bridge can
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
//   export   write every task and tombstone as JSONL, to stdout or --out
//   import   merge a JSONL export into the daemon's store, dry run by default
//   version  print the version
//
// Four of the six are thin CLIENTS of the control socket (mcp, cli, export,
// import): they do nothing with the backlog except ask the daemon to do it, so
// the daemon stays its single writer and no second process ever opens the
// database file. Only `serve` and `version` run with no daemon behind them —
// which is why `export` and `import` fail when one is not listening.
//
// Exit codes: 0 success, 1 runtime failure (store, socket, missing daemon,
// failed export write), 2 usage error (unknown command or flag, missing or
// out-of-range port, an empty file to import). The split matters because
// run.sh runs under `set -e`.
//
// STDOUT POLICY: stdout carries a PRODUCT, never a diagnostic. `version`
// prints a version string, `cli` a rendered table, `export` the JSONL itself
// (the subcommand is a data pipe: `taskPilot export > backlog.jsonl`), and
// `import` the merge report a human reads. In `mcp` mode stdout IS the MCP
// protocol stream, where one stray line corrupts the framing and the host
// drops the server with no error the user can act on — so every log line,
// warning, and banner in this file goes to stderr.
//
// This file stays thin on purpose: parsing, wiring, and the signal loop. Every
// rule about tasks, ranking, and persistence lives in the layers below it.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

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

    /// Address the control socket binds to (`serve`) or connects to (every
    /// other subcommand that has a daemon).
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
    kExport,
    kImport,
    /// Named kPrintVersion rather than kVersion to stay clear of the
    /// kVersion string constant, which -Wshadow would flag as a collision.
    kPrintVersion,
};

/// A fully parsed command line.
struct Invocation
{
    Command command{ Command::kServe };
    Config config;

    /// `export --out <path>`: where the JSONL goes. Empty means stdout, which
    /// is the subcommand's default shape — it is a data pipe.
    ///
    /// Deliberately NOT a Config member: Config is the precedence-ordered
    /// configuration (defaults, then environment, then flags), and these
    /// options have no environment variable and no default. Sitting next to
    /// --db they would suggest a source of truth that does not exist.
    std::string export_out_path;

    /// `import --file <path>`: the export to merge. Empty means stdin.
    std::string import_file_path;

    /// `import --apply`: true performs the merge, false only reports what it
    /// would do. The default is the DRY RUN — see runImport for why the
    /// destructive form is the one that must be asked for.
    bool apply{ false };

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
    if ("export" == text)
    {
        out = Command::kExport;
        return true;
    }
    if ("import" == text)
    {
        out = Command::kImport;
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
/// asked about one subcommand, and a wall of text for all six buries the
/// answer.
void printUsage(std::ostream &out, Command command, bool general_only)
{
    out << "usage: " << kProgramName << " <serve|mcp|cli|export|import|version> [options]\n"
        << "\n"
        << "  serve     run the daemon: opens the task store and serves the control socket\n"
        << "            (the default when no command is given)\n"
        << "  mcp       run the stdio MCP bridge; forwards every call to a running daemon\n"
        << "  cli       attach an interactive console to a running daemon\n"
        << "  export    write every task and tombstone as JSONL (stdout, or --out <path>)\n"
        << "  import    merge a JSONL export into the daemon (DRY RUN unless --apply)\n"
        << "  version   print the version and exit\n"
        << "\n"
        << "options:\n"
        << "  --db <path>       task database (serve only)      [TASKPILOT_DB]\n"
        << "                    default: data/taskpilot.db, created if missing\n"
        << "  --port <n>        control socket port, 1..65535   [TASKPILOT_PORT]\n"
        << "                    default: " << kDefaultPort << "\n"
        << "  --bind <addr>     bind (serve) or connect (mcp, cli, export, import) address\n"
        << "                    [TASKPILOT_BIND]  default: 127.0.0.1\n"
        << "  --out <path>      export destination (export only; default: stdout)\n"
        << "  --file <path>     export to merge, replacing stdin (import only)\n"
        << "  --apply           perform the merge instead of reporting it (import only)\n"
        << "  -h, --help        show this help\n";

    if (general_only)
    {
        return;
    }

    out << "\nexamples:\n"
        << "  ./run.sh serve                     # daemon on 127.0.0.1:" << kDefaultPort << "\n"
        << "  ./run.sh serve --port 9000 --db /tmp/backlog.db\n"
        << "  ./run.sh mcp                       # what Claude Code spawns\n"
        << "  ./run.sh cli                       # human REPL against the daemon\n"
        << "  ./run.sh export --out ../taskPilot-data/backlog.jsonl\n"
        << "  ./run.sh import --file ../taskPilot-data/backlog.jsonl --apply\n";

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
    else if (Command::kExport == command)
    {
        out << "\nexport needs a running daemon (./run.sh serve). It writes the whole backlog,\n"
            << "one record per line, to stdout — or to --out, which also prints a summary on\n"
            << "stderr. stdout is the data, so redirect it, and keep the result in the\n"
            << "private data repository rather than this public one.\n";
    }
    else if (Command::kImport == command)
    {
        out << "\nimport needs a running daemon (./run.sh serve) and MERGES the export read from\n"
            << "--file, or from stdin, into it. IT IS A DRY RUN BY DEFAULT: without --apply it\n"
            << "reports what would change and writes nothing. A merge can overwrite or delete\n"
            << "tasks, so the form that writes has to be asked for explicitly.\n";
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

        // --apply is the one flag that takes no value: its presence IS the
        // value, so it is settled before the value-taking options below and
        // never consumes the next argument (which would silently eat the
        // subcommand's own operand).
        if ("--apply" == flag)
        {
            if (Command::kImport != invocation.command)
            {
                invocation.error = "--apply is only valid for import";
                return invocation;
            }
            invocation.apply = true;
            continue;
        }

        // Every remaining option takes a value. --db, --port and --bind are
        // accepted whatever the subcommand is, as they always have been:
        // refusing them now would turn command lines that work today into
        // usage errors for no gain. --out and --file are new, so they are
        // scoped to the single subcommand that can act on them, and typing one
        // at the wrong subcommand says so instead of being dropped.
        if ("--db" != flag && "--port" != flag && "--bind" != flag && "--out" != flag
            && "--file" != flag)
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
        else if ("--bind" == flag)
        {
            invocation.config.bind_address = value;
        }
        else if ("--out" == flag)
        {
            if (Command::kExport != invocation.command)
            {
                invocation.error = "--out is only valid for export";
                return invocation;
            }
            invocation.export_out_path = value;
        }
        else
        {
            if (Command::kImport != invocation.command)
            {
                invocation.error = "--file is only valid for import";
                return invocation;
            }
            invocation.import_file_path = value;
        }
    }

    invocation.valid = true;
    return invocation;
}

/// Create the parent directory of `path` when it does not exist yet.
///
/// Used for both files this program creates: the SQLite database and the
/// `export --out` destination. Neither writes the directory above the file —
/// SQLite fails with a bare "unable to open database file" and std::ofstream
/// simply fails to open — and the default locations (data/taskpilot.db,
/// export/…) sit in directories a fresh clone does not have.
[[nodiscard]] bool ensureParentDirectory(const std::string &path, std::string &error)
{
    // ":memory:" and any URI-style path has no filesystem parent.
    if (path.empty() || ':' == path.front())
    {
        return true;
    }

    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
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

/// Report a failed control-socket call, in the form every client subcommand
/// uses.
///
/// The client's own message is surfaced verbatim: when nothing is listening it
/// already names the command that starts a daemon, and when a merge fails it
/// names the offending line. Replacing either with a generic "the call failed"
/// would send the user looking in the wrong place.
void reportRpcFailure(const Error &error)
{
    std::cerr << kProgramName << ": " << error.message << "\n";
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

/// `export` — write every task and tombstone as JSONL, through the daemon.
///
/// REQUIRES A RUNNING DAEMON, as `cli` and `mcp` do and as `version` does not.
/// That is the intended shape rather than a limitation: everything that
/// touches the backlog goes through the daemon, which is its single writer, so
/// the file on disk always has exactly one process whose view of it is
/// authoritative. Opening the database from this short-lived process instead
/// would be at best a lock conflict with the daemon that holds it open for its
/// whole life, and at worst a write into an inode the daemon has already
/// replaced — a failure mode that leaves two processes believing different
/// files are the backlog.
[[nodiscard]] int runExport(const Config &config, const std::string &out_path)
{
    // No ping() first: a missing daemon is reported by the call itself, with
    // the command that fixes it, and a probe would only open a window in which
    // the daemon can exit between the probe and the call.
    const ControlClient client(config.bind_address, config.port);

    const RpcResult response = client.call("export_tasks", nlohmann::json::object());
    if (!response.ok())
    {
        reportRpcFailure(response.error());
        return 1;
    }

    const nlohmann::json &result = response.value();
    const auto jsonl = result.find("jsonl");
    if (result.end() == jsonl || !jsonl->is_string())
    {
        // A reply without the payload means the thing on this port speaks a
        // different export protocol — an older binary, say. Saying so beats
        // printing nothing, which would be indistinguishable from an empty
        // backlog, and worse, would be written to the user's file as one.
        std::cerr << kProgramName << ": the daemon returned no export; is a different "
                  << kProgramName << " listening on " << config.bind_address << ":"
                  << config.port << "?\n";
        return 1;
    }

    const std::string lines = jsonl->get<std::string>();

    // The summary's record count. The daemon sends its own tally; counting the
    // newlines is the fallback for a reply that lacks it and is exact for this
    // format, because every record is one newline-terminated line.
    std::size_t records{ 0 };
    if (const auto count = result.find("count");
        result.end() != count && count->is_number_integer())
    {
        records = static_cast<std::size_t>(count->get<std::int64_t>());
    }
    else
    {
        records = static_cast<std::size_t>(std::count(lines.begin(), lines.end(), '\n'));
    }

    if (out_path.empty())
    {
        // The JSONL is this subcommand's product, so it goes to stdout byte for
        // byte — `taskPilot export > backlog.jsonl` and
        // `taskPilot export --out backlog.jsonl` must produce identical files,
        // which is why nothing else (not even a "done" line) shares this
        // stream.
        std::cout << lines;
        return 0;
    }

    std::string directory_error;
    if (!ensureParentDirectory(out_path, directory_error))
    {
        std::cerr << kProgramName << ": " << directory_error << "\n";
        return 1;
    }

    // Truncate rather than append: an export is a COMPLETE snapshot, so
    // whatever bytes are already in the file are stale by definition — and a
    // second copy of the same uids would break the one-line-per-uid property
    // the format and its merge both depend on.
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        std::cerr << kProgramName << ": cannot write " << out_path << ": " << std::strerror(errno)
                  << "\n";
        return 1;
    }

    out << lines;
    out.close();
    if (out.fail())
    {
        // A full disk or an I/O error part-way through. The file exists and
        // holds a prefix of the export, so it is removed: a snapshot that is
        // merely SHORT is the one artefact a later `git add` would happily
        // commit and the next machine would happily merge, and a missing file
        // is a far more obvious failure than a truncated one.
        std::error_code removal_failure;
        std::filesystem::remove(out_path, removal_failure);
        std::cerr << kProgramName << ": cannot write " << out_path
                  << ": the export was not written completely, and the partial file was removed\n";
        return 1;
    }

    // The summary is a diagnostic and stdout is carrying the data only when
    // --out is absent, so with --out the count and the path go to stderr.
    std::cerr << kProgramName << ": wrote " << records << " records to " << out_path << "\n";
    return 0;
}

// ---------------------------------------------------------------------------
// Merge report rendering
// ---------------------------------------------------------------------------

/// One line of the printed merge report.
struct MergeRow
{
    const char *counter_key; ///< Wire key of the count.
    const char *titles_key;  ///< Wire key of the example titles, or nullptr.
    const char *label;       ///< How the row is named when printed.
    const char *marker;      ///< One-character prefix for an example title.
};

/// The rows of a report, in the order a reader wants them: what arrived, what
/// was overwritten, what was removed, what came back, and what the local copy
/// already had.
///
/// The keys are the store's own MergeReport field names, because that struct
/// is what the daemon serializes; the labels are separate because they are how
/// a person reads the same thing. The dry run and the applied merge print
/// identical blocks, so the report a user approved is one they can compare the
/// real run against line by line.
constexpr MergeRow kMergeRows[]{
    { "inserted", "inserted_titles", "inserted", "+" },
    { "updated", "updated_titles", "updated", "~" },
    { "deleted", "deleted_titles", "deleted", "-" },
    { "resurrected", "resurrected_titles", "resurrected", "+" },
    { "skipped", nullptr, "skipped", "" },
};

/// Read one counter out of a merge report. nullopt when the document holds no
/// integer under that key, which the caller reports as an unreadable report
/// rather than as a count of zero: "nothing changed" and "I cannot read the
/// answer" must never look alike, and this is a report about overwriting work.
[[nodiscard]] std::optional<std::int64_t> reportCounter(const nlohmann::json &report,
                                                        const char *key)
{
    const auto entry = report.find(key);
    if (report.end() == entry || !entry->is_number_integer())
    {
        return std::nullopt;
    }

    return entry->get<std::int64_t>();
}

/// The example titles of one report row. Entries that are not strings are
/// dropped rather than rejected: one malformed element must not cost the reader
/// the rest of the report, which is the only account of what a merge did.
[[nodiscard]] std::vector<std::string> reportTitles(const nlohmann::json &report,
                                                    const char *key)
{
    std::vector<std::string> titles;
    if (nullptr == key)
    {
        return titles;
    }

    const auto list = report.find(key);
    if (report.end() == list || !list->is_array())
    {
        return titles;
    }

    for (const nlohmann::json &entry : *list)
    {
        if (entry.is_string())
        {
            titles.push_back(entry.get<std::string>());
        }
    }

    return titles;
}

/// Print a merge report for a human.
///
/// `applied` only changes the heading: a dry run and its matching real merge
/// carry the same numbers, which is what makes the dry run evidence about the
/// run that follows it rather than a separate story.
void printMergeReport(std::ostream &out, const nlohmann::json &report, bool applied)
{
    out << (applied ? "merge applied:" : "merge (dry run):") << "\n";

    bool recognized{ false };
    for (const MergeRow &row : kMergeRows)
    {
        const std::optional<std::int64_t> counter = reportCounter(report, row.counter_key);
        if (!counter.has_value())
        {
            continue;
        }
        recognized = true;

        out << "  " << row.label << ": " << *counter << "\n";

        const std::vector<std::string> titles = reportTitles(report, row.titles_key);
        for (const std::string &title : titles)
        {
            out << "    " << row.marker << " " << title << "\n";
        }

        // The store keeps the counts exact but caps the example lists, so the
        // gap has to be visible: a list that quietly stopped inside a first
        // sync of a large file would otherwise read as the whole story.
        if (nullptr != row.titles_key && titles.size() < static_cast<std::size_t>(*counter))
        {
            out << "    (" << (*counter - static_cast<std::int64_t>(titles.size()))
                << " more not listed)\n";
        }
    }

    if (!recognized)
    {
        // The reply is not a report this client knows how to read. Printing it
        // whole is the honest fallback — it still tells the reader what
        // changed, whereas a fabricated "0 inserted" would send them looking in
        // the wrong place.
        out << report.dump(2) << "\n";
    }
}

/// True when a report says the merge changed nothing (or would change
/// nothing); nullopt when the counters cannot be read at all, so the caller
/// falls back to advice that is safe whichever the answer is.
///
/// "skipped" is deliberately excluded from the sum: the local copy already
/// winning a record is precisely what a no-op sync looks like, and counting it
/// would make every clean merge look like work.
[[nodiscard]] std::optional<bool> reportIsClean(const nlohmann::json &report)
{
    constexpr const char *kChangeKeys[]{ "inserted", "updated", "deleted", "resurrected" };

    bool recognized{ false };
    bool changed{ false };
    for (const char *key : kChangeKeys)
    {
        const std::optional<std::int64_t> counter = reportCounter(report, key);
        if (!counter.has_value())
        {
            continue;
        }

        recognized = true;
        changed = changed || 0 < *counter;
    }

    if (!recognized)
    {
        return std::nullopt;
    }

    return !changed;
}

/// `import` — merge a JSONL export into the daemon's store.
///
/// DRY RUN BY DEFAULT, deliberately: a merge can overwrite or delete local
/// work, and omitting a flag is the quietest way to ask for anything, so the
/// form that writes must be the one that has to be requested. The dry run is
/// not a preview feature bolted on — it takes the same decisions and produces
/// the same report the real merge will, so approving it is approving an
/// outcome rather than a guess.
///
/// Like `export`, and for the same single-writer reason, this goes through the
/// daemon instead of opening the database itself.
[[nodiscard]] int runImport(const Config &config, const std::string &file_path, bool apply)
{
    std::string jsonl;
    if (file_path.empty())
    {
        jsonl.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
        if (std::cin.bad())
        {
            std::cerr << kProgramName << ": cannot read the export from stdin\n";
            return 1;
        }
    }
    else
    {
        std::ifstream in(file_path, std::ios::binary);
        if (!in.is_open())
        {
            std::cerr << kProgramName << ": cannot read " << file_path << ": "
                      << std::strerror(errno) << "\n";
            return 1;
        }

        jsonl.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (in.bad())
        {
            std::cerr << kProgramName << ": cannot read " << file_path << "\n";
            return 1;
        }
    }

    // An empty input is a usage error, not a clean merge of zero records: it
    // means the file is empty or the redirection was forgotten, and a merge is
    // additive unless a tombstone says otherwise, so reporting "no changes"
    // would let a pipeline that moved nothing look exactly like a sync that
    // had nothing to move.
    if (std::string::npos == jsonl.find_first_not_of(" \t\r\n\v\f"))
    {
        std::cerr << kProgramName << ": nothing to import: expected the JSONL that"
                  << " `taskPilot export` writes\n";
        return 2;
    }

    // dry_run is sent in both directions rather than left to the daemon's
    // default. The daemon's default is also "dry run", and saying so here means
    // this client's behaviour cannot be changed by that default moving.
    const ControlClient client(config.bind_address, config.port);
    const nlohmann::json params{ { "jsonl", jsonl }, { "dry_run", !apply } };
    const RpcResult response = client.call("import_tasks", params);
    if (!response.ok())
    {
        // For a malformed export this is where the line number arrives, so the
        // message is surfaced verbatim: it is the only pointer to the record
        // that stopped the merge.
        reportRpcFailure(response.error());
        return 1;
    }

    // The report IS this subcommand's product — it is what a human reads to
    // decide whether the merge it describes is the one they wanted — so it goes
    // to stdout.
    printMergeReport(std::cout, response.value(), apply);
    if (!apply)
    {
        if (reportIsClean(response.value()).value_or(false))
        {
            // The common case, and worth stating plainly: the two sides agree,
            // so there is nothing for --apply to do and telling the user to
            // re-run would only send them round the loop again.
            std::cout << "nothing to do: this backlog already matches the export.\n";
        }
        else
        {
            std::cout << "nothing was written: this was a dry run. Re-run with --apply to"
                      << " perform the merge.\n";
        }
    }
    return 0;
}

/// `version` — print the version and exit.
[[nodiscard]] int runVersion()
{
    // stdout carries it because a version string is a product (scripts and MCP
    // configs parse it), not a diagnostic.
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
    case taskpilot::Command::kExport:
        return taskpilot::runExport(invocation.config, invocation.export_out_path);
    case taskpilot::Command::kImport:
        return taskpilot::runImport(invocation.config, invocation.import_file_path,
                                    invocation.apply);
    case taskpilot::Command::kPrintVersion:
        return taskpilot::runVersion();
    }

    // Unreachable: every enumerator is handled above. A trailing failure is
    // deliberately preferred to a `default:` label, which would silently
    // swallow a newly added subcommand instead of letting -Wswitch flag it.
    std::cerr << taskpilot::kProgramName << ": unhandled command\n";
    return 1;
}
