// test_repl.cpp — grammar, dispatch and rendering tests for the console
//
// The console is driven through the same injection point production uses: a
// fake Caller that records what would have gone to the daemon. That makes the
// assertion target the real contract — the method name and the params the
// console builds — rather than the spacing of a printed column, so most cases
// below check what the fake received and only the cases where the text IS the
// feature (errors, the help list, the score breakdown) assert on output.
//
// tokenize() is private in the frozen header, so it is exercised through
// executeLine(), where its one observable effect — the tokens a command
// receives — is assertable.

#include "control/Repl.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Clock.hpp"
#include "core/Result.hpp"

namespace taskpilot
{
namespace
{

/// Stand-in for the ControlClient-backed Caller: records the last call (and
/// every method name, in order) and answers with a canned Result, so "the
/// daemon refused" is as cheap to exercise as "the daemon answered".
class FakeCaller
{
  public:
    FakeCaller()
        : m_caller([this](const std::string &method,
                          const nlohmann::json &params) -> RpcResult
                   {
                       last_method = method;
                       last_params = params;
                       methods.push_back(method);
                       ++calls;
                       return m_reply;
                   })
    {
    }

    // The lambda above captures `this`, so a copy would leave the copy's
    // callback pointing at the original.
    FakeCaller(const FakeCaller &) = delete;
    FakeCaller &operator=(const FakeCaller &) = delete;

    /// Answer the next call with a successful payload.
    void setReply(const nlohmann::json &payload) { m_reply = RpcResult{ payload }; }

    /// Answer the next call with a tool-level failure.
    void setFailure(const Error &failure) { m_reply = RpcResult{ failure }; }

    [[nodiscard]] Repl::Caller &caller() { return m_caller; }

    std::string last_method;
    nlohmann::json last_params;
    std::vector<std::string> methods;
    int calls{ 0 };

  private:
    RpcResult m_reply{ nlohmann::json::object() };
    Repl::Caller m_caller;
};

/// One console session wired to a fake daemon, with its output captured.
///
/// Member order is load-bearing: the caller and the stream must outlive the
/// Repl, which holds references to both.
class Session
{
  public:
    [[nodiscard]] bool line(const std::string &text) { return m_repl.executeLine(text); }
    [[nodiscard]] std::string output() const { return m_out.str(); }
    [[nodiscard]] FakeCaller &fake() { return m_fake; }

  private:
    FakeCaller m_fake;
    std::ostringstream m_out;
    Repl m_repl{ m_fake.caller(), m_out };
};

/// A fixed "now" for every case that does not need the wall clock:
/// 2026-10-08 12:00:00 UTC as a plain number. Nothing below assumes a time
/// zone, so any instant would do; a real date just makes failures readable.
constexpr std::int64_t kNow{ 1791460800 };

/// Local midnight of the day containing `epoch_seconds`, shifted by
/// `day_offset`.
///
/// Computed here the way the implementation must compute it — break the instant
/// down in LOCAL time, zero the clock fields, let mktime resolve the offset —
/// because asserting an absolute epoch would bake this machine's time zone into
/// the test and fail on a laptop that moved.
std::int64_t localMidnight(std::int64_t epoch_seconds, int day_offset)
{
    const std::time_t instant = static_cast<std::time_t>(epoch_seconds);
    std::tm local{};
    if (nullptr == localtime_r(&instant, &local))
    {
        ADD_FAILURE() << "localtime_r failed for " << epoch_seconds;
        return 0;
    }
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    local.tm_mday += day_offset;
    return static_cast<std::int64_t>(std::mktime(&local));
}

/// Local midnight of an explicit calendar date, by the same reasoning as
/// localMidnight.
std::int64_t localMidnightOf(int year, int month, int day)
{
    std::tm local{};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    return static_cast<std::int64_t>(std::mktime(&local));
}

/// Render an instant the way the console is required to.
///
/// This one duplicates the implementation's format on purpose: unlike a date
/// PARSER, whose contract is "any correct answer", the requirement here IS the
/// rendered string, so the expected text has to be stated somewhere in the
/// test.
std::string stampOf(std::int64_t epoch_seconds)
{
    const std::time_t instant = static_cast<std::time_t>(epoch_seconds);
    std::tm local{};
    if (nullptr == localtime_r(&instant, &local))
    {
        return "?";
    }
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02d %02d:%02d", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);
    return std::string{ buffer };
}

/// A ranked-queue entry, shaped the way PriorityEngine::toJson documents it.
nlohmann::json rankedTask(std::int64_t id, const std::string &title, int importance,
                          const nlohmann::json &due_at, int blocks, double score)
{
    return nlohmann::json{
        { "id", id },
        { "title", title },
        { "status", "open" },
        { "importance", importance },
        { "due_at", due_at },
        { "blocks", blocks },
        { "score", score },
        { "score_parts",
          nlohmann::json{ { "importance", 25.0 },
                          { "urgency", 30.0 },
                          { "age", 6.0 },
                          { "blocks", 16.0 } } },
    };
}

// ---------------------------------------------------------------------------
// tokenize(), observed through the command it feeds
// ---------------------------------------------------------------------------

TEST(ReplTokenize, PlainTokensSplitOnWhitespace)
{
    Session session;
    EXPECT_TRUE(session.line("queue 7"));
    EXPECT_EQ("get_queue", session.fake().last_method);
    EXPECT_EQ(7, session.fake().last_params["limit"].get<int>());
}

TEST(ReplTokenize, QuotedTitleWithSpacesArrivesAsOneToken)
{
    Session session;
    EXPECT_TRUE(session.line("add \"fix the WS bug\" imp=5"));
    ASSERT_EQ(1, session.fake().calls);
    EXPECT_EQ("fix the WS bug", session.fake().last_params["title"].get<std::string>());
}

TEST(ReplTokenize, SurroundingAndRepeatedWhitespaceIsIgnored)
{
    Session session;
    EXPECT_TRUE(session.line("   add    milk   \t "));
    ASSERT_EQ(1, session.fake().calls);
    EXPECT_EQ("milk", session.fake().last_params["title"].get<std::string>());
}

TEST(ReplTokenize, ACommentIsStrippedBeforeTokenizing)
{
    Session session;
    EXPECT_TRUE(session.line("queue 3 # only the top three"));
    ASSERT_EQ(1, session.fake().calls);
    EXPECT_EQ(3, session.fake().last_params["limit"].get<int>());
    // The comment left no argument behind: "three" would otherwise have been
    // read as a second argument of its own.
    EXPECT_EQ(1u, session.fake().last_params.size());
}

TEST(ReplTokenize, HashInsideQuotesIsTextNotAComment)
{
    Session session;
    EXPECT_TRUE(session.line("add \"reopen #42 after the fix\""));
    ASSERT_EQ(1, session.fake().calls);
    EXPECT_EQ("reopen #42 after the fix",
              session.fake().last_params["title"].get<std::string>());
}

TEST(ReplTokenize, UnterminatedQuoteIsAnErrorAndEndsNothing)
{
    Session session;
    EXPECT_TRUE(session.line("add \"fix the WS bug"));
    // Nothing half-parsed reached the daemon.
    EXPECT_EQ(0, session.fake().calls);
    EXPECT_NE(std::string::npos, session.output().find("error:"));

    // And the session survives it: a console that exits on a typo is useless
    // for exploring.
    EXPECT_TRUE(session.line("queue"));
    EXPECT_EQ(1, session.fake().calls);
}

TEST(ReplTokenize, BlankAndCommentOnlyLinesDoNothing)
{
    Session session;
    EXPECT_TRUE(session.line(""));
    EXPECT_TRUE(session.line("    "));
    EXPECT_TRUE(session.line("# a note to self"));
    EXPECT_EQ(0, session.fake().calls);
    EXPECT_TRUE(session.output().empty());
}

// ---------------------------------------------------------------------------
// parseDue()
// ---------------------------------------------------------------------------

TEST(ReplParseDue, EmptyAndDashMeanNoDeadline)
{
    const std::vector<std::string> none{ "", "-", "   " };
    for (const std::string &text : none)
    {
        const Result<std::optional<std::int64_t>> due = Repl::parseDue(text, kNow);
        ASSERT_TRUE(due.ok()) << text;
        EXPECT_FALSE(due.value().has_value()) << text;
    }
}

TEST(ReplParseDue, RelativeOffsetsAreAddedToNow)
{
    struct Case
    {
        const char *text;
        std::int64_t offset;
    };
    const Case cases[]{
        { "+3d", 3 * kSecondsPerDay },
        { "+6h", 6 * 3600 },
        { "+30m", 30 * 60 },
        { "+90m", 90 * 60 },
        { "+0d", 0 },
        { "+100d", 100 * kSecondsPerDay },
    };

    for (const Case &one : cases)
    {
        const Result<std::optional<std::int64_t>> due = Repl::parseDue(one.text, kNow);
        ASSERT_TRUE(due.ok()) << one.text;
        ASSERT_TRUE(due.value().has_value()) << one.text;
        EXPECT_EQ(kNow + one.offset, *due.value()) << one.text;
    }
}

TEST(ReplParseDue, TodayAndTomorrowLandOnLocalMidnight)
{
    // The expectation is derived with the same arithmetic the implementation
    // must use, so this asserts the day boundary rather than this machine's
    // offset — see the comment on localMidnight.
    const Result<std::optional<std::int64_t>> today = Repl::parseDue("today", kNow);
    ASSERT_TRUE(today.ok());
    ASSERT_TRUE(today.value().has_value());
    EXPECT_EQ(localMidnight(kNow, 0), *today.value());

    const Result<std::optional<std::int64_t>> tomorrow = Repl::parseDue("tomorrow", kNow);
    ASSERT_TRUE(tomorrow.ok());
    ASSERT_TRUE(tomorrow.value().has_value());
    EXPECT_EQ(localMidnight(kNow, 1), *tomorrow.value());

    // Zone-independent properties, so a wrong expectation helper cannot make a
    // wrong implementation look right: today's midnight has already happened,
    // and it happened within the last day (a DST day is 23 or 25 hours long).
    EXPECT_LE(*today.value(), kNow);
    EXPECT_LE(kNow - *today.value(), 25 * 3600);
    EXPECT_GT(*tomorrow.value(), kNow);
}

TEST(ReplParseDue, AbsoluteDateIsTheLocalMidnightOfThatDay)
{
    const Result<std::optional<std::int64_t>> due = Repl::parseDue("2027-03-01", kNow);
    ASSERT_TRUE(due.ok());
    ASSERT_TRUE(due.value().has_value());
    EXPECT_EQ(localMidnightOf(2027, 3, 1), *due.value());

    // A leap day is a real day, and the parser has to know that.
    const Result<std::optional<std::int64_t>> leap = Repl::parseDue("2028-02-29", kNow);
    ASSERT_TRUE(leap.ok());
    ASSERT_TRUE(leap.value().has_value());
    EXPECT_EQ(localMidnightOf(2028, 2, 29), *leap.value());
}

TEST(ReplParseDue, RejectsMalformedInputAndNamesTheAcceptedForms)
{
    const std::vector<std::string> malformed{
        "+3x",          // unknown unit
        "+",            // no number, no unit
        "+3",           // no unit
        "+d",           // no number
        "3d",           // missing the '+'
        "next tuesday", // not a form at all
        "todayy",       // near miss
        "2026-13-45",   // no such month
        "2026-02-30",   // no such day
        "2027-02-29",   // 2027 is not a leap year
        "2026-1-5",     // the documented form is zero-padded
        "2026/10/15",   // wrong separator
        "2026-10-15T00:00", // trailing time
    };

    for (const std::string &text : malformed)
    {
        const Result<std::optional<std::int64_t>> due = Repl::parseDue(text, kNow);
        ASSERT_FALSE(due.ok()) << text;
        EXPECT_EQ(ErrorCode::kInvalidArgument, due.error().code) << text;
        // The message lists what IS accepted, so a typo needs no help lookup.
        EXPECT_NE(std::string::npos, due.error().message.find("YYYY-MM-DD")) << text;
        EXPECT_NE(std::string::npos, due.error().message.find("tomorrow")) << text;
    }
}

TEST(ReplParseDue, AnAbsurdOffsetIsRejectedRatherThanOverflowing)
{
    const std::vector<std::string> too_large{
        "+100001d",               // beyond the supported span
        "+99999999999999999999d", // beyond int64 while scanning
    };
    for (const std::string &text : too_large)
    {
        const Result<std::optional<std::int64_t>> due = Repl::parseDue(text, kNow);
        EXPECT_FALSE(due.ok()) << text;
    }
}

// ---------------------------------------------------------------------------
// Dispatch: what each command sends
// ---------------------------------------------------------------------------

TEST(ReplCommands, QueueDefaultsToTenAndHonoursAnExplicitLimit)
{
    {
        Session session;
        EXPECT_TRUE(session.line("queue"));
        EXPECT_EQ("get_queue", session.fake().last_method);
        EXPECT_EQ(10, session.fake().last_params["limit"].get<int>());
    }
    {
        Session session;
        EXPECT_TRUE(session.line("queue 25"));
        EXPECT_EQ(25, session.fake().last_params["limit"].get<int>());
    }
    {
        // 0 is the API's documented spelling of "the whole queue".
        Session session;
        EXPECT_TRUE(session.line("queue 0"));
        EXPECT_EQ(0, session.fake().last_params["limit"].get<int>());
    }
    {
        Session session;
        EXPECT_TRUE(session.line("queue -1"));
        EXPECT_EQ(0, session.fake().calls);
        EXPECT_NE(std::string::npos, session.output().find("error:"));
    }
    {
        Session session;
        EXPECT_TRUE(session.line("queue three"));
        EXPECT_EQ(0, session.fake().calls);
        EXPECT_NE(std::string::npos, session.output().find("error:"));
    }
}

TEST(ReplCommands, ListSendsTheStatusFilterItWasGiven)
{
    {
        Session session;
        EXPECT_TRUE(session.line("ls"));
        EXPECT_EQ("list_tasks", session.fake().last_method);
        // No filter is invented for a bare ls.
        EXPECT_TRUE(session.fake().last_params.empty());
    }
    {
        Session session;
        EXPECT_TRUE(session.line("ls in_progress"));
        EXPECT_EQ("list_tasks", session.fake().last_method);
        EXPECT_EQ("in_progress", session.fake().last_params["status"].get<std::string>());
    }
    {
        Session session;
        EXPECT_TRUE(session.line("ls bogus"));
        EXPECT_EQ(0, session.fake().calls);
        EXPECT_NE(std::string::npos, session.output().find("bogus"));
    }
}

TEST(ReplCommands, AddBuildsTheParamsFromKeyValuePairs)
{
    Session session;
    const std::int64_t before = SystemClock{}.nowEpochSeconds();
    EXPECT_TRUE(session.line("add \"fix the WS bug\" imp=5 due=+2d blocks=3 tags=ws,bug "
                             "notes=\"flaky since Tuesday\""));
    const std::int64_t after = SystemClock{}.nowEpochSeconds();

    ASSERT_EQ(1, session.fake().calls);
    EXPECT_EQ("add_task", session.fake().last_method);

    const nlohmann::json &params = session.fake().last_params;
    EXPECT_EQ("fix the WS bug", params["title"].get<std::string>());
    EXPECT_EQ(5, params["importance"].get<int>());
    EXPECT_EQ(3, params["blocks"].get<int>());
    EXPECT_EQ(std::vector<std::string>({ "ws", "bug" }),
              params["tags"].get<std::vector<std::string>>());
    EXPECT_EQ("flaky since Tuesday", params["notes"].get<std::string>());

    // The relative deadline is resolved against the console's own clock, which
    // the test cannot read, so it is bracketed by the instants around the call
    // rather than pinned. (The absolute form is pinned exactly below.)
    const std::int64_t due = params["due_at"].get<std::int64_t>();
    EXPECT_GE(due, before + 2 * kSecondsPerDay);
    EXPECT_LE(due, after + 2 * kSecondsPerDay);
}

TEST(ReplCommands, AddOmitsTheDeadlineWhenAskedForNone)
{
    Session session;
    EXPECT_TRUE(session.line("add milk due=-"));
    ASSERT_EQ(1, session.fake().calls);
    // Only the title: "no deadline" is the ABSENCE of due_at, not a zero.
    EXPECT_EQ(1u, session.fake().last_params.size());
    EXPECT_EQ("milk", session.fake().last_params["title"].get<std::string>());
}

TEST(ReplCommands, AddPinsAnAbsoluteDeadlineToLocalMidnight)
{
    Session session;
    EXPECT_TRUE(session.line("add taxes due=2030-06-01"));
    ASSERT_EQ(1, session.fake().calls);
    EXPECT_EQ(localMidnightOf(2030, 6, 1),
              session.fake().last_params["due_at"].get<std::int64_t>());
}

TEST(ReplCommands, AddSendsOnlyTheTitleWhenNoKeysAreGiven)
{
    Session session;
    EXPECT_TRUE(session.line("add milk"));
    ASSERT_EQ(1, session.fake().calls);
    // Defaults belong to the daemon; the console must not bake in its own idea
    // of them.
    EXPECT_EQ(1u, session.fake().last_params.size());
    EXPECT_EQ("milk", session.fake().last_params["title"].get<std::string>());
}

TEST(ReplCommands, ABareSecondWordIsNotJoinedIntoTheTitle)
{
    // Tokens come from the line, so a title with spaces has to be quoted.
    // Gluing "milk" onto "buy" would be a guessing game: is the next word part
    // of the title or a key? Refusing it, and saying nothing was created, is
    // the only answer that cannot silently mean something the user did not ask
    // for.
    Session session;
    EXPECT_TRUE(session.line("add buy milk"));
    EXPECT_EQ(0, session.fake().calls);
    EXPECT_NE(std::string::npos, session.output().find("error:"));
}

TEST(ReplCommands, AddRejectsBadKeysValuesAndSyntaxWithoutCalling)
{
    const std::vector<std::string> bad_commands{
        "add",                   // no title at all
        "add \"t\" colour=red",  // unknown key
        "add \"t\" imp=9",       // out of range
        "add \"t\" imp=0",
        "add \"t\" imp=abc",     // not a number
        "add \"t\" blocks=-1",
        "add \"t\" due=next tuesday",
        "add \"t\" imp",         // not a key=value pair
        "add \"t\" imp=5 imp=3", // duplicate key
        "add \"t\" title=other", // the title is positional, not a key
    };

    for (const std::string &command : bad_commands)
    {
        Session session;
        EXPECT_TRUE(session.line(command)) << command;
        EXPECT_EQ(0, session.fake().calls) << command;
        EXPECT_NE(std::string::npos, session.output().find("error:")) << command;
    }
}

TEST(ReplCommands, IdCommandsSendTheIdAndTheRightMethod)
{
    struct Case
    {
        const char *line;
        const char *method;
    };
    const Case cases[]{
        { "show 7", "get_task" },
        { "done 7", "complete_task" },
        { "reopen 7", "reopen_task" },
        { "rm 7", "delete_task" },
    };

    for (const Case &one : cases)
    {
        Session session;
        EXPECT_TRUE(session.line(one.line));
        EXPECT_EQ(one.method, session.fake().last_method) << one.line;
        EXPECT_EQ(7, session.fake().last_params["id"].get<int>()) << one.line;
    }
}

TEST(ReplCommands, IdCommandsRejectAMissingOrNonNumericId)
{
    const std::vector<std::string> bad_commands{ "show", "show seven", "done abc", "rm" };
    for (const std::string &command : bad_commands)
    {
        Session session;
        EXPECT_TRUE(session.line(command)) << command;
        EXPECT_EQ(0, session.fake().calls) << command;
        EXPECT_NE(std::string::npos, session.output().find("error:")) << command;
    }
}

TEST(ReplCommands, WeightsReadsWithoutArgumentsAndWritesWithThem)
{
    {
        Session session;
        EXPECT_TRUE(session.line("weights"));
        EXPECT_EQ("get_weights", session.fake().last_method);
        EXPECT_TRUE(session.fake().last_params.empty());
        EXPECT_NE(std::string::npos, session.output().find("score ="));
    }
    {
        Session session;
        EXPECT_TRUE(session.line("weights urgency=0.5 age=10"));
        EXPECT_EQ("set_weights", session.fake().last_method);
        EXPECT_DOUBLE_EQ(0.5, session.fake().last_params["urgency"].get<double>());
        EXPECT_DOUBLE_EQ(10.0, session.fake().last_params["age"].get<double>());
        // An integral weight keeps the integer shape the catalog advertises; a
        // fractional one stays a float rather than being refused. Both reach the
        // same double field on the other side.
        EXPECT_TRUE(session.fake().last_params["age"].is_number_integer());
        EXPECT_TRUE(session.fake().last_params["urgency"].is_number_float());
    }
}

TEST(ReplCommands, WeightsRejectsUnknownKeysAndUnparseableValues)
{
    const std::vector<std::string> bad_commands{
        "weights bogus=1",
        "weights urgency=x",
        "weights urgency=",
        "weights =1",
        "weights urgency=1 urgency=2",
    };

    for (const std::string &command : bad_commands)
    {
        Session session;
        EXPECT_TRUE(session.line(command)) << command;
        EXPECT_EQ(0, session.fake().calls) << command;
        EXPECT_NE(std::string::npos, session.output().find("error:")) << command;
    }
}

TEST(ReplCommands, SimpleQueriesCallTheirMethodsWithNoParams)
{
    struct Case
    {
        const char *line;
        const char *method;
    };
    const Case cases[]{
        { "stats", "get_stats" },
        { "status", "get_status" },
        { "methods", "describe_methods" },
    };

    for (const Case &one : cases)
    {
        Session session;
        EXPECT_TRUE(session.line(one.line));
        EXPECT_EQ(one.method, session.fake().last_method) << one.line;
        EXPECT_TRUE(session.fake().last_params.empty()) << one.line;
    }
}

TEST(ReplCommands, UnknownCommandKeepsTheSessionAlive)
{
    Session session;
    EXPECT_TRUE(session.line("frobnicate"));
    EXPECT_EQ(0, session.fake().calls);
    EXPECT_NE(std::string::npos, session.output().find("error:"));
    EXPECT_NE(std::string::npos, session.output().find("frobnicate"));

    EXPECT_TRUE(session.line("queue"));
    EXPECT_EQ(1, session.fake().calls);
}

TEST(ReplCommands, FailedRpcPrintsTheDaemonMessageAndKeepsTheSessionAlive)
{
    Session session;
    session.fake().setFailure(Error::notFound("no task with id 7"));

    EXPECT_TRUE(session.line("done 7"));
    EXPECT_NE(std::string::npos, session.output().find("error: no task with id 7"));

    // The failure came from the daemon, not from the console, and it must not
    // take the session with it.
    EXPECT_TRUE(session.line("queue"));
    EXPECT_EQ(2, session.fake().calls);
}

TEST(ReplCommands, AMissingDaemonConnectionIsAnErrorNotACrash)
{
    // A console built without a caller is a wiring bug, and it must surface as
    // a message rather than as std::bad_function_call taking the session down.
    std::ostringstream out;
    Repl repl{ Repl::Caller{}, out };

    EXPECT_TRUE(repl.executeLine("queue"));
    EXPECT_NE(std::string::npos, out.str().find("error:"));
}

TEST(ReplCommands, QuitAndExitEndTheSession)
{
    Session session;
    EXPECT_FALSE(session.line("exit"));
    EXPECT_FALSE(session.line("quit"));
    EXPECT_FALSE(session.line("quit # done for today"));
    EXPECT_EQ(0, session.fake().calls);
}

// ---------------------------------------------------------------------------
// run()
// ---------------------------------------------------------------------------

TEST(ReplRun, ReadsUntilQuitAndThenStops)
{
    FakeCaller fake;
    std::ostringstream out;
    Repl repl{ fake.caller(), out };

    std::istringstream input{ "queue 1\nquit\nqueue 2\n" };
    repl.run(input);

    // The line after quit was never executed.
    ASSERT_EQ(1, fake.calls);
    EXPECT_EQ("get_queue", fake.methods.front());
}

TEST(ReplRun, EndsAtEofWithoutAQuit)
{
    FakeCaller fake;
    std::ostringstream out;
    Repl repl{ fake.caller(), out };

    std::istringstream input{ "queue 1\n" };
    repl.run(input);

    EXPECT_EQ(1, fake.calls);
}

TEST(ReplRun, ToleratesCrlfAndBlankLines)
{
    FakeCaller fake;
    std::ostringstream out;
    Repl repl{ fake.caller(), out };

    std::istringstream input{ "\r\nqueue 4\r\n   \n" };
    repl.run(input);

    ASSERT_EQ(1, fake.calls);
    EXPECT_EQ(4, fake.last_params["limit"].get<int>());
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

TEST(ReplFormatting, QueueTableShowsTheBreakdownAndMarksOverdue)
{
    const std::int64_t overdue_due = 1; // 1970: overdue against any clock
    const std::int64_t future_due = SystemClock{}.nowEpochSeconds() + 3 * kSecondsPerDay;

    Session session;
    session.fake().setReply(nlohmann::json::array({
        rankedTask(7, "fix the WS bug", 5, overdue_due, 2, 77.0),
        rankedTask(8, "write the report", 4, future_due, 0, 47.7),
        rankedTask(9, "tidy the store", 3, nullptr, 0, 41.6),
    }));

    EXPECT_TRUE(session.line("queue 3"));
    const std::string out = session.output();

    EXPECT_NE(std::string::npos, out.find("fix the WS bug"));
    EXPECT_NE(std::string::npos, out.find("write the report"));
    EXPECT_NE(std::string::npos, out.find("tidy the store"));

    // The header names every column.
    EXPECT_NE(std::string::npos, out.find("score"));
    EXPECT_NE(std::string::npos, out.find("title"));

    // The total, and the breakdown that is the whole point of the view: the
    // number alone says nothing about why the order is what it is.
    EXPECT_NE(std::string::npos, out.find("77.0"));
    EXPECT_NE(std::string::npos, out.find("i25 u30 a6 b16"));
    EXPECT_NE(std::string::npos, out.find("score parts:"));

    // Due dates are human, and only the past one carries the marker.
    EXPECT_NE(std::string::npos, out.find(stampOf(overdue_due)));
    EXPECT_NE(std::string::npos, out.find("! " + stampOf(overdue_due)));
    EXPECT_NE(std::string::npos, out.find(stampOf(future_due)));
    EXPECT_EQ(std::string::npos, out.find("! " + stampOf(future_due)));
    EXPECT_NE(std::string::npos, out.find("! = overdue"));
}

TEST(ReplFormatting, ListRendersPlainTasksWithoutInventingScores)
{
    Session session;
    session.fake().setReply(nlohmann::json::array({
        nlohmann::json{ { "id", 4 },
                        { "title", "reply to mail" },
                        { "importance", 2 },
                        { "due_at", nullptr },
                        { "blocks", 0 } },
    }));

    EXPECT_TRUE(session.line("ls"));
    EXPECT_EQ("list_tasks", session.fake().last_method);

    const std::string out = session.output();
    EXPECT_NE(std::string::npos, out.find("reply to mail"));
    // Nothing ranked came back, so there is nothing to explain: no breakdown
    // legend, and no overdue legend either.
    EXPECT_EQ(std::string::npos, out.find("score parts:"));
    EXPECT_EQ(std::string::npos, out.find("! = overdue"));
}

TEST(ReplFormatting, QueueAcceptsAListWrappedInAnObject)
{
    Session session;
    session.fake().setReply(nlohmann::json{
        { "queue", nlohmann::json::array({ rankedTask(1, "wrapped entry", 1, nullptr, 0, 1.0) }) },
    });

    EXPECT_TRUE(session.line("queue"));
    EXPECT_NE(std::string::npos, session.output().find("wrapped entry"));
}

TEST(ReplFormatting, AnUnusableReplyIsAnErrorNotACrash)
{
    Session session;
    session.fake().setReply(nlohmann::json{ { "unexpected", true } });

    EXPECT_TRUE(session.line("queue"));
    EXPECT_NE(std::string::npos, session.output().find("error:"));
}

TEST(ReplFormatting, ShowRendersEveryFieldIncludingTheScore)
{
    Session session;
    session.fake().setReply(nlohmann::json{
        { "id", 3 },
        { "title", "tidy the store" },
        { "status", "in_progress" },
        { "importance", 4 },
        { "due_at", 1 },
        { "blocks", 1 },
        { "tags", nlohmann::json::array({ "data", "chore" }) },
        { "created_at", 1791000000 },
        { "updated_at", 1791400000 },
        { "completed_at", nullptr },
        { "notes", "vacuum the sqlite file" },
        { "score", 30.0 },
        { "score_parts",
          nlohmann::json{ { "importance", 20.0 },
                          { "urgency", 0.0 },
                          { "age", 10.0 },
                          { "blocks", 0.0 } } },
    });

    EXPECT_TRUE(session.line("show 3"));
    EXPECT_EQ("get_task", session.fake().last_method);
    EXPECT_EQ(3, session.fake().last_params["id"].get<int>());

    const std::string out = session.output();
    EXPECT_NE(std::string::npos, out.find("tidy the store"));
    EXPECT_NE(std::string::npos, out.find("in_progress"));
    EXPECT_NE(std::string::npos, out.find("data, chore"));
    EXPECT_NE(std::string::npos, out.find("vacuum the sqlite file"));
    // The total, then the four terms that produced it.
    EXPECT_NE(std::string::npos, out.find("30.0 (i20 u0 a10 b0)"));
    EXPECT_NE(std::string::npos, out.find("this task is overdue"));
}

TEST(ReplFormatting, StatsAndStatusRenderTalliesAndHumanTimes)
{
    {
        Session session;
        session.fake().setReply(nlohmann::json{ { "open", 3 },
                                                { "in_progress", 1 },
                                                { "done", 9 },
                                                { "archived", 0 },
                                                { "overdue", 2 },
                                                { "due_within_24h", 1 },
                                                { "now", kNow },
                                                // A field this console has never
                                                // heard of: it must still be
                                                // shown, not dropped.
                                                { "top_title", "fix the WS bug" } });
        EXPECT_TRUE(session.line("stats"));
        const std::string out = session.output();
        EXPECT_NE(std::string::npos, out.find("open:"));
        EXPECT_NE(std::string::npos, out.find("overdue:"));
        EXPECT_NE(std::string::npos, out.find("due_within_24h:"));
        EXPECT_NE(std::string::npos, out.find(stampOf(kNow)));
        EXPECT_NE(std::string::npos, out.find("top_title:"));
        EXPECT_NE(std::string::npos, out.find("fix the WS bug"));
    }
    {
        Session session;
        session.fake().setReply(nlohmann::json{ { "version", "0.1.0" },
                                                { "uptime_seconds", 3661 },
                                                { "db_path", "/tmp/tasks.db" },
                                                { "method_count", 13 },
                                                { "now", kNow },
                                                { "open", 1 } });
        EXPECT_TRUE(session.line("status"));
        const std::string out = session.output();
        EXPECT_NE(std::string::npos, out.find("0.1.0"));
        // Durations and instants are rendered, not dumped raw.
        EXPECT_NE(std::string::npos, out.find("1h 1m 1s"));
        EXPECT_EQ(std::string::npos, out.find("3661"));
        EXPECT_NE(std::string::npos, out.find("/tmp/tasks.db"));
        EXPECT_NE(std::string::npos, out.find("13"));
        EXPECT_NE(std::string::npos, out.find(stampOf(kNow)));
        // The tallies merged into the same reply are shown too.
        EXPECT_NE(std::string::npos, out.find("open:"));
    }
}

TEST(ReplFormatting, WeightsPrintTheFormulaTheyFeed)
{
    Session session;
    session.fake().setReply(nlohmann::json{ { "importance", 5.0 },
                                            { "urgency", 30.0 },
                                            { "age", 2.5 },
                                            { "blocks", 8.0 },
                                            { "urgency_horizon_days", 7.0 } });

    EXPECT_TRUE(session.line("weights"));
    const std::string out = session.output();
    EXPECT_NE(std::string::npos, out.find("importance"));
    EXPECT_NE(std::string::npos, out.find("2.5")); // fractional weights survive
    EXPECT_NE(std::string::npos, out.find("score ="));
    EXPECT_NE(std::string::npos, out.find("urgency ramps over 7 days"));
}

TEST(ReplFormatting, MethodsListsNamesDescriptionsAndParams)
{
    Session session;
    session.fake().setReply(nlohmann::json::array({ nlohmann::json{
        { "name", "add_task" },
        { "description", "Create a task and return it." },
        { "params_schema",
          nlohmann::json{ { "type", "object" },
                          { "properties",
                            nlohmann::json{ { "title", nlohmann::json::object() } } },
                          { "required", nlohmann::json::array({ "title" }) } } },
    } }));

    EXPECT_TRUE(session.line("methods"));
    EXPECT_EQ("describe_methods", session.fake().last_method);

    const std::string out = session.output();
    EXPECT_NE(std::string::npos, out.find("add_task"));
    EXPECT_NE(std::string::npos, out.find("Create a task and return it."));
    EXPECT_NE(std::string::npos, out.find("params: title"));
    EXPECT_NE(std::string::npos, out.find("(required: title)"));
}

// ---------------------------------------------------------------------------
// help
// ---------------------------------------------------------------------------

TEST(ReplHelp, ListsEveryCommandAndNeedsNoDaemon)
{
    const std::vector<std::string> commands{
        "queue", "ls", "add",   "show",  "done", "reopen", "rm",
        "weights", "stats", "status", "methods", "help", "quit", "exit",
    };

    const std::string text = Repl::helpText();
    for (const std::string &command : commands)
    {
        EXPECT_NE(std::string::npos, text.find(command)) << command;
    }

    Session session;
    EXPECT_TRUE(session.line("help"));
    const std::string printed = session.output();
    for (const std::string &command : commands)
    {
        EXPECT_NE(std::string::npos, printed.find(command)) << command;
    }

    // help is answered locally: it has to work when the daemon is down, which
    // is exactly when a user needs to read it.
    EXPECT_EQ(0, session.fake().calls);
}

TEST(ReplHelp, DocumentsTheGrammarItAccepts)
{
    // Spots where the help and the parser must agree, checked against the
    // parser itself rather than against a copied list.
    const std::string text = Repl::helpText();
    EXPECT_NE(std::string::npos, text.find("+3d"));
    EXPECT_NE(std::string::npos, text.find("YYYY-MM-DD"));
    EXPECT_NE(std::string::npos, text.find("in_progress"));

    // A bare "in_progress" is legal input; a status the help never mentions is
    // not, so the two texts describe one vocabulary.
    EXPECT_TRUE(Repl::parseDue("+3d", kNow).ok());
    EXPECT_TRUE(taskStatusFromString("in_progress").has_value());
}

} // namespace
} // namespace taskpilot
