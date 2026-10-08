#pragma once
// Repl.hpp — line-oriented console for the daemon
//
// The MCP bridge is how an LLM drives taskPilot; this is how a human does.
// It is a thin shell over the same control-socket methods, so anything the
// REPL can do, MCP can do and vice versa — there is no second code path to
// keep in sync.
//
// Grammar (one command per line; `#` starts a comment):
//
//   queue [n]              top n of the ranked queue (default 10)
//   ls [status]            list tasks, optionally filtered by status
//   add <title> [k=v ...]  create a task; keys: imp, due, blocks, tags, notes
//   show <id>              one task
//   done <id>              mark complete
//   reopen <id>            back to open
//   rm <id>                delete
//   weights [k=v ...]      show, or update, the ranking weights
//   stats                  counts and tallies
//   status                 daemon version, uptime, db path
//   methods                the method catalog (what MCP exposes)
//   help                   this list
//   quit | exit            leave
//
// `due` accepts a relative offset (`+3d`, `+6h`, `today`) or an absolute
// `YYYY-MM-DD`, which is what a person actually types; the conversion to the
// epoch seconds the API wants happens here, at the edge, so the wire protocol
// stays numeric and unambiguous.

#include <cstdint>
#include <functional>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "control/ControlClient.hpp"
#include "control/Services.hpp"

namespace taskpilot
{

/// Interactive command loop.
///
/// Split out from main.cpp so the parser is unit-testable without a socket:
/// executeLine() takes an injected caller, exactly like McpServer takes an
/// injected backend.
class Repl
{
  public:
    /// Performs one control-socket call. Injected for tests.
    using Caller = std::function<RpcResult(const std::string &method,
                                           const nlohmann::json &params)>;

    /// `caller` performs the RPC; `out` receives human-readable results.
    Repl(Caller caller, std::ostream &out);

    /// Read from `in` until EOF or quit/exit. Returns when the input closes.
    void run(std::istream &in);

    /// Execute one command line and write its output.
    ///
    /// Returns false when the line asked to leave. A command that fails
    /// prints `error: <message>` and returns true — one bad command must not
    /// end the session, which would make the REPL useless for exploring.
    bool executeLine(const std::string &line);

    /// The help text, also used by the `help` command.
    [[nodiscard]] static const char *helpText();

    /// Parse a deadline expression into epoch seconds. Exposed for testing.
    ///
    /// Accepted forms, all resolved against `now`:
    ///   +3d / +6h / +30m   relative offset
    ///   today / tomorrow   midnight boundaries
    ///   2026-10-15         absolute date, interpreted at 00:00 local time
    /// Empty or `-` means "no deadline" and yields nullopt.
    [[nodiscard]] static Result<std::optional<std::int64_t>>
    parseDue(const std::string &text, std::int64_t now);

  private:
    /// Split a line into whitespace-separated tokens, honouring double quotes
    /// so a title may contain spaces:  add "fix the WS bug" imp=5
    [[nodiscard]] static std::vector<std::string> tokenize(const std::string &line);

    Caller m_caller;
    std::ostream &m_out;
};

} // namespace taskpilot
