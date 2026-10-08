// Repl.cpp — see Repl.hpp
//
// The console is a thin shell over the control-socket method catalog, exactly
// the way McpServer is: every command below is one RPC call, and nothing is
// decided here that the daemon also decides (field validation, ranking,
// persistence). There is therefore no second code path to keep in sync — the
// `queue` command and the `get_queue` tool are the same operation, so a method
// that changes meaning cannot be fixed on one frontend and forgotten on the
// other.
//
// Two things do live here rather than in the daemon, and both are edge
// concerns:
//
//   1. The `due` grammar (+3d, today, 2026-10-15). It is a convenience for a
//      human typing at a prompt, so the conversion to epoch seconds happens
//      here, once, and the wire protocol stays pure numeric and unambiguous.
//      The daemon never learns to parse the word "tomorrow".
//   2. Presentation: the aligned table, the score breakdown that explains why
//      the order is what it is, and the overdue marker. A client that wants
//      numbers rather than a rendering asks the daemon for the JSON.
//
// Output goes only to the injected stream, never to stdout directly: the same
// binary also runs the `mcp` subcommand, where stdout is the protocol stream
// and one stray line would corrupt its framing.

#include "control/Repl.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Clock.hpp"
#include "core/Task.hpp"

namespace taskpilot
{
namespace
{

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

/// Whitespace as the scanner understands it. std::isspace is used rather than a
/// hand-written set so a line pasted from elsewhere (a CR left over from CRLF
/// framing, a stray vertical tab) cannot turn into a token.
[[nodiscard]] bool isSpace(char ch)
{
    return 0 != std::isspace(static_cast<unsigned char>(ch));
}

/// Strip leading and trailing whitespace.
[[nodiscard]] std::string trim(const std::string &text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && isSpace(text[begin]))
    {
        ++begin;
    }
    while (end > begin && isSpace(text[end - 1]))
    {
        --end;
    }
    return text.substr(begin, end - begin);
}

/// Strip trailing whitespace only, keeping any left indent. A rendered table
/// row needs this rather than trim(): the last column's padding must not reach
/// the file, but the row's indent must survive.
[[nodiscard]] std::string rstrip(const std::string &text)
{
    std::size_t end = text.size();
    while (end > 0 && isSpace(text[end - 1]))
    {
        --end;
    }
    return text.substr(0, end);
}

/// Format a score for a table cell: one decimal place, which is the precision
/// the ranking is meaningful at. Always via snprintf into a local buffer, so no
/// formatting flag is ever left set on the caller's output stream.
[[nodiscard]] std::string formatScore(double value)
{
    // The buffer is sized generously because a double may need many digits.
    // snprintf truncates rather than overflows, so an absurd value renders as a
    // clipped string instead of corrupting memory.
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.1f", value);
    return std::string{ buffer };
}

/// Format a score-breakdown term compactly: no decimal point when the value is
/// a whole number (importance × weight always is) and one decimal otherwise
/// (the aging term is fractional days). Keeps the score column narrow, which is
/// what makes the breakdown fit next to the total it explains.
[[nodiscard]] std::string formatCompact(double value)
{
    const double rounded = std::round(value);
    if (std::fabs(value - rounded) < 1e-9 && std::fabs(rounded) < 1e15)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%.0f", rounded);
        return std::string{ buffer };
    }
    return formatScore(value);
}

/// Render epoch seconds as a local wall-clock stamp ("2026-10-08 09:00").
///
/// Display only: the wire format stays epoch seconds, and the daemon never sees
/// a formatted date. An instant the platform cannot break down (a corrupted
/// row, an absurd deadline) renders as "?" instead of failing the whole view.
[[nodiscard]] std::string formatTimestamp(std::int64_t epoch_seconds)
{
    const std::time_t instant = static_cast<std::time_t>(epoch_seconds);
    std::tm local{};
    if (nullptr == localtime_r(&instant, &local))
    {
        return "?";
    }

    // Sized for a tm_year the platform is free to report wildly (the format is
    // bounded at 60 bytes for the widest int fields), so the compiler can see
    // that nothing is truncated.
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02d %02d:%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min);
    return std::string{ buffer };
}

/// Render a duration for the uptime line: only the units that are non-zero, so
/// "1h" rather than "0d 1h 0m 0s".
[[nodiscard]] std::string formatDuration(std::int64_t seconds)
{
    if (seconds <= 0)
    {
        return "0s";
    }

    const std::int64_t days = seconds / kSecondsPerDay;
    const std::int64_t hours = (seconds % kSecondsPerDay) / 3600;
    const std::int64_t minutes = (seconds % 3600) / 60;
    const std::int64_t rest = seconds % 60;

    std::string text;
    const auto append = [&text](std::int64_t value, char unit)
    {
        if (!text.empty())
        {
            text += ' ';
        }
        text += std::to_string(value);
        text += unit;
    };

    if (days > 0)
    {
        append(days, 'd');
    }
    if (hours > 0)
    {
        append(hours, 'h');
    }
    if (minutes > 0)
    {
        append(minutes, 'm');
    }
    if (rest > 0 || text.empty())
    {
        append(rest, 's');
    }
    return text;
}

// ---------------------------------------------------------------------------
// Reading replies
//
// A reply is data, not a promise: the console renders whatever the daemon sent
// and must never throw on a field it did not expect (a missing key, a null
// where a string belongs, an integer stored as unsigned). Each reader below
// therefore degrades to the fallback instead of raising, which keeps one
// surprising field from blanking out an entire command.
// ---------------------------------------------------------------------------

[[nodiscard]] std::string readString(const nlohmann::json &object, const char *key,
                                     std::string fallback = std::string{})
{
    if (!object.is_object())
    {
        return fallback;
    }
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string())
    {
        return fallback;
    }
    return it->get<std::string>();
}

[[nodiscard]] std::optional<std::int64_t> readInteger(const nlohmann::json &object,
                                                      const char *key)
{
    if (!object.is_object())
    {
        return std::nullopt;
    }
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_integer())
    {
        return std::nullopt;
    }
    try
    {
        // is_number_integer() also covers unsigned storage, and an unsigned
        // value above INT64_MAX passes that check but does not fit; the
        // conversion reports that by throwing. A malformed reply is not worth
        // an exception escaping a display path, so it degrades to "absent".
        return it->get<std::int64_t>();
    }
    catch (const nlohmann::json::exception &)
    {
        return std::nullopt;
    }
}

/// Render an arbitrary JSON value as one line. Scalars read naturally; an array
/// or object is shown as compact JSON, which is the honest rendering of
/// structure that has no table of its own.
[[nodiscard]] std::string formatValue(const nlohmann::json &value)
{
    if (value.is_string())
    {
        return value.get<std::string>();
    }
    if (value.is_boolean())
    {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_null())
    {
        return "-";
    }
    if (value.is_number())
    {
        // A count stays a count: 13 is not "13.0". The unsigned case is guarded
        // because a value above INT64_MAX fits the JSON type but not the integer
        // it would be read into.
        if (value.is_number_integer())
        {
            try
            {
                return std::to_string(value.get<std::int64_t>());
            }
            catch (const nlohmann::json::exception &)
            {
                return value.dump();
            }
        }
        return formatCompact(value.get<double>());
    }
    return value.dump();
}

/// The array of rows inside a reply.
///
/// get_queue and list_tasks may answer with a bare array or with an object that
/// wraps one; the frozen headers fix the method names and the entry shape but
/// not this container, so both readings are accepted rather than one being
/// assumed and the other turning a valid reply into "unexpected reply shape".
[[nodiscard]] Result<nlohmann::json> rowsOf(const nlohmann::json &reply)
{
    if (reply.is_array())
    {
        return reply;
    }

    if (reply.is_object())
    {
        for (const char *key : { "queue", "tasks", "items", "entries", "results" })
        {
            const auto it = reply.find(key);
            if (it != reply.end() && it->is_array())
            {
                return *it;
            }
        }

        // A single-array-member object is unambiguous whatever the key is, so
        // it is accepted too. Anything more ambiguous than that is reported
        // rather than guessed at.
        if (1 == reply.size())
        {
            for (const auto &item : reply.items())
            {
                if (item.value().is_array())
                {
                    return item.value();
                }
            }
        }
    }

    return Error::invalidArgument("the reply carried no list of tasks");
}

/// The task object inside a reply that may wrap it (`{"task": {...}}`) or be
/// the task itself.
[[nodiscard]] nlohmann::json taskOf(const nlohmann::json &reply)
{
    if (reply.is_object())
    {
        const auto it = reply.find("task");
        if (it != reply.end() && it->is_object())
        {
            return *it;
        }
    }
    return reply;
}

// ---------------------------------------------------------------------------
// Command context and output primitives
// ---------------------------------------------------------------------------

/// What a command handler needs: the RPC channel, the output stream, and the
/// instant sampled for this line. `now` is sampled once per line by
/// executeLine rather than read per use, so a `due=+3d` conversion and the
/// overdue markers in the same command cannot disagree about what "now" means.
struct Console
{
    const Repl::Caller &caller;
    std::ostream &out;
    std::int64_t now;

    /// One RPC, through a single funnel.
    ///
    /// A default-constructed Caller means the console was wired without a
    /// daemon connection — a bug in the caller's setup, not user input — and it
    /// is caught here because the alternative is std::bad_function_call
    /// unwinding the whole session on the first command.
    [[nodiscard]] RpcResult call(const std::string &method, nlohmann::json params) const
    {
        if (!caller)
        {
            return Error::internal("the console has no daemon connection");
        }
        return caller(method, params);
    }
};

void printError(std::ostream &out, const Error &error)
{
    out << "error: " << error.message << '\n';
}

void printError(const Console &console, const Error &error)
{
    printError(console.out, error);
}

/// Print aligned "label: value" lines, padded to the longest label.
void printFields(const Console &console,
                 const std::vector<std::pair<std::string, std::string>> &fields)
{
    std::size_t width = 0;
    for (const auto &field : fields)
    {
        width = std::max(width, field.first.size() + 1);
    }

    for (const auto &field : fields)
    {
        const std::string label = field.first + ':';
        console.out << "  " << label << std::string(width - label.size(), ' ') << ' '
                    << field.second << '\n';
    }
}

struct Column
{
    std::string header;
    bool right_align{ false };
};

/// Print a header row and then the rows, each cell padded to the widest value
/// in its column. Widths are measured from the rendered strings, so a column
/// whose values vary in width — a due date that carries an overdue marker —
/// still lines up.
void printTable(const Console &console, const std::vector<Column> &columns,
                const std::vector<std::vector<std::string>> &rows)
{
    std::vector<std::size_t> widths(columns.size(), 0);
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        widths[i] = columns[i].header.size();
    }
    for (const std::vector<std::string> &row : rows)
    {
        const std::size_t cells = std::min(row.size(), widths.size());
        for (std::size_t i = 0; i < cells; ++i)
        {
            widths[i] = std::max(widths[i], row[i].size());
        }
    }

    const auto writeRow = [&](const std::vector<std::string> &cells)
    {
        std::string line;
        for (std::size_t i = 0; i < columns.size(); ++i)
        {
            const std::string cell = i < cells.size() ? cells[i] : std::string{};
            const std::size_t padding = widths[i] > cell.size() ? widths[i] - cell.size() : 0;
            line += "  ";
            if (columns[i].right_align)
            {
                line += std::string(padding, ' ');
                line += cell;
            }
            else
            {
                line += cell;
                line += std::string(padding, ' ');
            }
        }
        console.out << rstrip(line) << '\n';
    };

    std::vector<std::string> headers;
    headers.reserve(columns.size());
    for (const Column &column : columns)
    {
        headers.push_back(column.header);
    }

    writeRow(headers);
    for (const std::vector<std::string> &row : rows)
    {
        writeRow(row);
    }
}

// ---------------------------------------------------------------------------
// Scalar parsing
// ---------------------------------------------------------------------------

/// Parse a whole number and refuse anything with leftovers, so "7x" does not
/// read as 7. from_chars is used rather than stoi because caller input is an
/// expected failure here: it must come back as an Error, not as an exception
/// unwinding a command loop.
[[nodiscard]] Result<std::int64_t> parseInteger(const std::string &text,
                                                const std::string &what)
{
    std::int64_t value = 0;
    const std::from_chars_result parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (std::errc{} != parsed.ec || parsed.ptr != text.data() + text.size())
    {
        return Error::invalidArgument(what + ": expected a whole number, got '" + text + "'");
    }
    return value;
}

/// Parse a decimal number. Weights are doubles — urgency 0.5 is legal — so this
/// reader exists alongside parseInteger rather than sharing it.
[[nodiscard]] Result<double> parseNumber(const std::string &text, const std::string &what)
{
    double value = 0.0;
    const std::from_chars_result parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (std::errc{} != parsed.ec || parsed.ptr != text.data() + text.size())
    {
        return Error::invalidArgument(what + ": expected a number, got '" + text + "'");
    }
    if (!std::isfinite(value))
    {
        // NaN and infinity would poison every comparison in the ranking. The
        // store rejects them too; refusing here keeps the message about the
        // token the user actually typed.
        return Error::invalidArgument(what + ": expected a finite number, got '" + text + "'");
    }
    return value;
}

/// Split a k=v command argument. Only the first '=' separates, so a value may
/// contain one ("notes=see a=b").
[[nodiscard]] Result<std::pair<std::string, std::string>>
parseKeyValue(const std::string &token, const std::string &what)
{
    const std::size_t separator = token.find('=');
    if (std::string::npos == separator || 0 == separator)
    {
        return Error::invalidArgument(what + ": expected key=value, got '" + token + "'");
    }
    return std::make_pair(token.substr(0, separator), token.substr(separator + 1));
}

/// "a,b,c" -> {"a","b","c"}. Blank elements are dropped rather than sent: the
/// store rejects a blank tag, so "a,,b" would otherwise fail an entire add over
/// one stray comma.
[[nodiscard]] std::vector<std::string> splitTags(const std::string &value)
{
    std::vector<std::string> tags;
    std::size_t begin = 0;
    while (begin <= value.size())
    {
        const std::size_t comma = value.find(',', begin);
        const std::size_t end = std::string::npos == comma ? value.size() : comma;
        const std::string tag = trim(value.substr(begin, end - begin));
        if (!tag.empty())
        {
            tags.push_back(tag);
        }
        if (std::string::npos == comma)
        {
            break;
        }
        begin = comma + 1;
    }
    return tags;
}

// ---------------------------------------------------------------------------
// Deadline grammar
//
// This is the one place a human date becomes an epoch second. Everything past
// this file speaks integers, so the console can be opinionated about the local
// time zone without a stored task, the daemon, or an MCP client ever becoming
// ambiguous about what a deadline means.
// ---------------------------------------------------------------------------

struct CalendarDate
{
    int year{ 0 };
    int month{ 0 };
    int day{ 0 };
};

/// The forms parseDue accepts, stated once so every rejection can quote them.
constexpr const char *kDueForms{
    "+3d, +6h, +30m, today, tomorrow or YYYY-MM-DD, or '-' for no deadline"
};

/// An offset beyond this is a typo, not a deadline. Bounding it also keeps the
/// seconds conversion below clear of signed overflow.
constexpr std::int64_t kMaxOffsetSeconds{ 100 * 365 * kSecondsPerDay };

[[nodiscard]] bool isLeapYear(int year)
{
    return (0 == year % 4 && 0 != year % 100) || 0 == year % 400;
}

[[nodiscard]] int daysInMonth(int year, int month)
{
    // February is the only month whose length depends on the year; the rest are
    // table lookups.
    if (2 == month)
    {
        return isLeapYear(year) ? 29 : 28;
    }
    switch (month)
    {
        case 1:
        case 3:
        case 5:
        case 7:
        case 8:
        case 10:
        case 12:
            return 31;
        default:
            return 30;
    }
}

[[nodiscard]] bool isDigits(const std::string &text, std::size_t begin, std::size_t end)
{
    if (end > text.size() || begin >= end)
    {
        return false;
    }
    for (std::size_t i = begin; i < end; ++i)
    {
        if (0 == std::isdigit(static_cast<unsigned char>(text[i])))
        {
            return false;
        }
    }
    return true;
}

/// Read a short run of digits that isDigits() has already validated. Four
/// digits at most, so this cannot overflow.
[[nodiscard]] int digitsToInt(const std::string &text, std::size_t begin, std::size_t end)
{
    int value = 0;
    for (std::size_t i = begin; i < end; ++i)
    {
        value = value * 10 + (text[i] - '0');
    }
    return value;
}

/// The one rejection shape for a deadline: what was typed, optionally why it
/// was rejected, and the full list of accepted forms. A user should never have
/// to consult help to fix a typo.
[[nodiscard]] Error dueFormatError(const std::string &text,
                                   const std::string &detail = std::string{})
{
    std::string message = "invalid due date '" + text + "'";
    if (!detail.empty())
    {
        message += " (" + detail + ")";
    }
    message += ": expected ";
    message += kDueForms;
    return Error::invalidArgument(std::move(message));
}

/// Local midnight of the day `epoch_seconds` falls on, shifted by `day_offset`
/// days.
///
/// Two details make this correct rather than merely plausible:
///   1. The shift is applied to tm_mday, not to the epoch. A local day is not
///      always 86400 seconds long, so "tomorrow" computed as now + 86400 drifts
///      by an hour across a DST change; mktime normalises day 32 into the next
///      month for us.
///   2. tm_isdst is set to -1 so the library derives the offset in force at the
///      target midnight instead of inheriting the one in force now. On a
///      transition day those differ, and inheriting would put midnight an hour
///      off.
[[nodiscard]] Result<std::int64_t> localMidnightFrom(std::int64_t epoch_seconds,
                                                     int day_offset)
{
    const std::time_t instant = static_cast<std::time_t>(epoch_seconds);
    std::tm local{};
    if (nullptr == localtime_r(&instant, &local))
    {
        return Error::invalidArgument(
            "the local time zone could not be resolved for that instant");
    }

    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    local.tm_mday += day_offset;

    const std::time_t midnight = std::mktime(&local);
    if (static_cast<std::time_t>(-1) == midnight)
    {
        return Error::invalidArgument(
            "that date is outside the range the local calendar library can represent");
    }
    return static_cast<std::int64_t>(midnight);
}

/// Local midnight of an explicit calendar date.
[[nodiscard]] Result<std::int64_t> localMidnightOn(int year, int month, int day)
{
    std::tm local{};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_isdst = -1;

    const std::time_t midnight = std::mktime(&local);
    if (static_cast<std::time_t>(-1) == midnight)
    {
        return Error::invalidArgument(
            "that date is outside the range the local calendar library can represent");
    }

    // Belt to the range checks' braces. mktime NORMALISES out-of-range fields
    // (Feb 30 becomes Mar 2) and a time-zone change can make a wall-clock time
    // not exist at all, so the instant is read back and compared with the
    // calendar day that was asked for. This is what turns a silently shifted
    // date into a visible error; the comparison uses the requested year, month
    // and day because mktime overwrites the tm it normalised.
    std::tm verified{};
    if (nullptr == localtime_r(&midnight, &verified))
    {
        return Error::invalidArgument(
            "that date is outside the range the local calendar library can represent");
    }
    if (verified.tm_year + 1900 != year || verified.tm_mon + 1 != month
        || verified.tm_mday != day)
    {
        return Error::invalidArgument("that date does not exist in the local time zone");
    }
    return static_cast<std::int64_t>(midnight);
}

/// Parse exactly "YYYY-MM-DD". Positional rather than lenient because the
/// accepted set is one shape: a parser that also took "2026-1-5" would have to
/// guess whether the first number was the month.
[[nodiscard]] Result<CalendarDate> parseCalendarDate(const std::string &text)
{
    const bool shaped = 10 == text.size() && '-' == text[4] && '-' == text[7]
        && isDigits(text, 0, 4) && isDigits(text, 5, 7) && isDigits(text, 8, 10);
    if (!shaped)
    {
        return dueFormatError(text, "an absolute date must be YYYY-MM-DD");
    }

    CalendarDate date;
    date.year = digitsToInt(text, 0, 4);
    date.month = digitsToInt(text, 5, 7);
    date.day = digitsToInt(text, 8, 10);

    if (date.month < 1 || date.month > 12)
    {
        return dueFormatError(text, "the month must be 01-12");
    }
    if (date.day < 1 || date.day > daysInMonth(date.year, date.month))
    {
        return dueFormatError(text, "that day does not exist in that month");
    }
    return date;
}

/// Parse "+<digits><unit>" and add it to `now`.
[[nodiscard]] Result<std::int64_t> relativeDeadline(const std::string &text,
                                                    std::int64_t now)
{
    // Scan the shape up front and report the missing piece by name, rather than
    // handing a malformed token to a library that would only say "invalid".
    std::int64_t magnitude = 0;
    std::size_t index = 1;
    std::size_t digits = 0;
    while (index < text.size() && 0 != std::isdigit(static_cast<unsigned char>(text[index])))
    {
        const std::int64_t digit = text[index] - '0';
        if (magnitude > (std::numeric_limits<std::int64_t>::max() - digit) / 10)
        {
            return dueFormatError(text, "the offset is too large");
        }
        magnitude = magnitude * 10 + digit;
        ++digits;
        ++index;
    }

    if (0 == digits)
    {
        return dueFormatError(text, "a number is required after '+'");
    }
    if (index >= text.size())
    {
        return dueFormatError(text, "a unit is required (d, h or m)");
    }
    if (index + 1 != text.size())
    {
        return dueFormatError(text, "unexpected trailing characters");
    }

    std::int64_t unit_seconds = 0;
    switch (text[index])
    {
        case 'd':
            unit_seconds = kSecondsPerDay;
            break;
        case 'h':
            unit_seconds = 3600;
            break;
        case 'm':
            unit_seconds = 60;
            break;
        default:
            return dueFormatError(text,
                                  std::string{ "unknown unit '" } + text[index]
                                      + "'; use d, h or m");
    }

    // Bounded before multiplying so the multiply cannot overflow, and bounded
    // again before adding so the sum cannot either. Both are impossible for a
    // sane offset; they exist so a fat-fingered "+999999999999d" is an error
    // message rather than undefined behaviour.
    if (magnitude > kMaxOffsetSeconds / unit_seconds)
    {
        return dueFormatError(text, "the offset is too large");
    }
    const std::int64_t offset = magnitude * unit_seconds;
    if (now > std::numeric_limits<std::int64_t>::max() - offset)
    {
        return dueFormatError(text, "the deadline is outside the representable range");
    }
    return now + offset;
}

// ---------------------------------------------------------------------------
// Task rendering
// ---------------------------------------------------------------------------

/// What a rendered row said, collected so the caller can print a legend once
/// for the whole table instead of repeating it per row.
struct RowNotes
{
    bool has_breakdown{ false };
    bool has_overdue{ false };
};

/// The columns queue and ls share. "imp" is the importance column, kept short
/// because the score column — which carries the breakdown that explains the
/// order — needs the width more than a header does.
[[nodiscard]] std::vector<Column> taskColumns()
{
    // The score column is left-aligned even though it holds a number: its cells
    // are a total plus a parenthetical of varying length, so right-aligning them
    // would line up the closing bracket and leave the totals scattered.
    return {
        Column{ "#", true },
        Column{ "score", false },
        Column{ "id", true },
        Column{ "imp", true },
        Column{ "due", false },
        Column{ "blocks", true },
        Column{ "title", false },
    };
}

/// "i25 u30 a6 b16" — the four weighted terms in the order the formula is
/// written (see docs/scoring.md). Empty when the reply carries no usable
/// breakdown, so a plain task list renders an empty cell rather than a row of
/// zeros, which would be a claim the console cannot support.
[[nodiscard]] std::string breakdownCell(const nlohmann::json &entry)
{
    const auto it = entry.is_object() ? entry.find("score_parts") : entry.end();
    if (it == entry.end() || !it->is_object())
    {
        return std::string{};
    }
    // All four terms or none: a partial breakdown would silently understate the
    // score it is supposed to explain.
    for (const char *key : { "importance", "urgency", "age", "blocks" })
    {
        const auto part = it->find(key);
        if (part == it->end() || !part->is_number())
        {
            return std::string{};
        }
    }

    return "i" + formatCompact((*it)["importance"].get<double>())
        + " u" + formatCompact((*it)["urgency"].get<double>())
        + " a" + formatCompact((*it)["age"].get<double>())
        + " b" + formatCompact((*it)["blocks"].get<double>());
}

/// The due cell: a human date, prefixed with "! " once the deadline has passed.
/// A prefix rather than a suffix so the marker cannot be read as part of the
/// date, and text rather than colour so it survives a pipe or a captured log.
[[nodiscard]] std::string dueCell(const nlohmann::json &entry, std::int64_t now,
                                  bool &overdue)
{
    const std::optional<std::int64_t> due = readInteger(entry, "due_at");
    if (!due.has_value())
    {
        return "-";
    }

    overdue = *due < now;
    const std::string stamp = formatTimestamp(*due);
    return overdue ? "! " + stamp : stamp;
}

/// One table row: rank, score (with its breakdown when the reply has one), id,
/// importance, due, blocks, title.
[[nodiscard]] std::vector<std::string> taskCells(const nlohmann::json &entry,
                                                 std::int64_t now, std::size_t index,
                                                 RowNotes &notes)
{
    const auto integerCell = [&entry](const char *key) -> std::string
    {
        const std::optional<std::int64_t> value = readInteger(entry, key);
        return value.has_value() ? std::to_string(*value) : std::string{ "-" };
    };

    std::vector<std::string> cells;
    cells.reserve(7);

    // The rank is the row's position, which is the ranked order get_queue
    // returned; no separate field is needed for it.
    cells.push_back(std::to_string(index + 1));

    // Score and its evidence share a cell: the total is what the row is ordered
    // by, and the terms in parentheses are WHY it landed there. The total alone
    // would hide the one thing the console knows that the user's memory of the
    // task does not.
    const auto score = entry.is_object() ? entry.find("score") : entry.end();
    if (score != entry.end() && score->is_number())
    {
        std::string cell = formatScore(score->get<double>());
        const std::string breakdown = breakdownCell(entry);
        if (!breakdown.empty())
        {
            cell += " (" + breakdown + ")";
            notes.has_breakdown = true;
        }
        cells.push_back(std::move(cell));
    }
    else
    {
        // list_tasks may return plain tasks with no ranking attached.
        cells.emplace_back("-");
    }

    cells.push_back(integerCell("id"));
    cells.push_back(integerCell("importance"));

    bool overdue = false;
    cells.push_back(dueCell(entry, now, overdue));
    if (overdue)
    {
        notes.has_overdue = true;
    }

    cells.push_back(integerCell("blocks"));
    cells.push_back(readString(entry, "title", "-"));
    return cells;
}

/// Legend lines, printed only when some row actually carries what they explain.
/// Without the first, the letters in the score column are guesswork; without
/// the second, the "!" is.
void printLegends(const Console &console, const RowNotes &notes)
{
    if (notes.has_breakdown)
    {
        console.out << "  score parts: i=importance, u=urgency, a=age, b=blocks\n";
    }
    if (notes.has_overdue)
    {
        console.out << "  ! = overdue\n";
    }
}

/// Render the rows of a task-list reply. Shared by queue and ls so the two
/// views cannot drift into showing different columns of the same data.
void printTaskRows(const Console &console, const RpcResult &reply, const char *empty_message)
{
    if (!reply.ok())
    {
        printError(console, reply.error());
        return;
    }

    const Result<nlohmann::json> rows = rowsOf(reply.value());
    if (!rows.ok())
    {
        printError(console, rows.error());
        return;
    }

    RowNotes notes;
    std::vector<std::vector<std::string>> table;
    std::size_t index = 0;
    for (const nlohmann::json &entry : rows.value())
    {
        table.push_back(taskCells(entry, console.now, index, notes));
        ++index;
    }

    if (table.empty())
    {
        console.out << empty_message << '\n';
        return;
    }

    printTable(console, taskColumns(), table);
    printLegends(console, notes);
}

/// One line reporting what a mutating command did, built from the task the
/// daemon sent back, so the console never claims more than the daemon
/// confirmed. A reply without an id (or without a title, as delete's is) still
/// prints what it does carry.
void printTaskLine(const Console &console, const char *verb, const nlohmann::json &reply)
{
    const nlohmann::json task = taskOf(reply);
    console.out << verb;

    const std::optional<std::int64_t> id = readInteger(task, "id");
    if (id.has_value())
    {
        console.out << ' ' << *id;
    }

    const std::string title = readString(task, "title");
    if (!title.empty())
    {
        console.out << ": " << title;
    }
    console.out << '\n';
}

/// The full view of one task, score breakdown included: this is the command for
/// "why is this where it is?".
void printTaskDetail(const Console &console, const nlohmann::json &reply)
{
    const nlohmann::json task = taskOf(reply);

    const auto integer = [&task](const char *key) -> std::string
    {
        const std::optional<std::int64_t> value = readInteger(task, key);
        return value.has_value() ? std::to_string(*value) : std::string{ "-" };
    };
    const auto stamp = [&task](const char *key) -> std::string
    {
        const std::optional<std::int64_t> value = readInteger(task, key);
        return value.has_value() ? formatTimestamp(*value) : std::string{ "-" };
    };

    std::string tags;
    const auto stored = task.is_object() ? task.find("tags") : task.end();
    if (stored != task.end() && stored->is_array())
    {
        for (const nlohmann::json &tag : *stored)
        {
            if (!tag.is_string())
            {
                continue;
            }
            if (!tags.empty())
            {
                tags += ", ";
            }
            tags += tag.get<std::string>();
        }
    }
    if (tags.empty())
    {
        tags = "-";
    }

    bool overdue = false;
    const std::string due = dueCell(task, console.now, overdue);

    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back("id", integer("id"));
    fields.emplace_back("title", readString(task, "title", "-"));
    fields.emplace_back("status", readString(task, "status", "-"));
    fields.emplace_back("importance", integer("importance"));
    fields.emplace_back("due", due);
    fields.emplace_back("blocks", integer("blocks"));
    fields.emplace_back("tags", tags);
    fields.emplace_back("created", stamp("created_at"));
    fields.emplace_back("updated", stamp("updated_at"));
    fields.emplace_back("completed", stamp("completed_at"));

    const auto score = task.is_object() ? task.find("score") : task.end();
    if (score != task.end() && score->is_number())
    {
        std::string cell = formatScore(score->get<double>());
        const std::string breakdown = breakdownCell(task);
        if (!breakdown.empty())
        {
            cell += " (" + breakdown + ")";
        }
        fields.emplace_back("score", std::move(cell));
    }

    fields.emplace_back("notes", readString(task, "notes", "-"));

    printFields(console, fields);
    if (overdue)
    {
        console.out << "  ! this task is overdue\n";
    }
}

// ---------------------------------------------------------------------------
// Report rendering (stats, status, weights)
// ---------------------------------------------------------------------------

/// The tallies get_stats and get_status are documented to carry, listed once so
/// both views print them in the same order.
constexpr const char *kTallyKeys[]{
    "open", "in_progress", "done", "archived", "overdue", "due_within_24h"
};

[[nodiscard]] bool isTallyKey(const std::string &key)
{
    for (const char *known : kTallyKeys)
    {
        if (key == known)
        {
            return true;
        }
    }
    return false;
}

/// Append a reply object as labelled rows: the tallies first, in a fixed order,
/// then every other key the daemon sent.
///
/// `already_shown` names keys the caller rendered itself — get_status spreads
/// its identity fields into the same object as the tallies — so nothing appears
/// twice and nothing is dropped. Dropping unknown fields would be the tempting
/// shortcut, and it teaches a user to trust a partial picture.
void appendReportFields(const nlohmann::json &object,
                        const std::vector<std::string> &already_shown,
                        std::vector<std::pair<std::string, std::string>> &fields)
{
    if (!object.is_object())
    {
        return;
    }

    const auto shown = [&already_shown](const std::string &key)
    {
        return isTallyKey(key)
            || std::find(already_shown.begin(), already_shown.end(), key) != already_shown.end();
    };

    for (const char *key : kTallyKeys)
    {
        const auto it = object.find(key);
        if (it != object.end())
        {
            fields.emplace_back(key, formatValue(*it));
        }
    }
    for (const auto &item : object.items())
    {
        if (shown(item.key()))
        {
            continue;
        }
        fields.emplace_back(item.key(), formatValue(item.value()));
    }
}

/// Render the ranking weights, plus the formula they feed. The formula line is
/// what makes the numbers actionable: "urgency 30" means nothing until it is
/// written next to the term it multiplies.
void printWeights(const Console &console, const nlohmann::json &reply)
{
    // get_weights answers with the weights object itself; an object wrapping it
    // under "weights" is accepted too, so a future envelope cannot turn a valid
    // reply into a useless dump.
    const nlohmann::json weights =
        (reply.is_object() && reply.contains("weights") && reply["weights"].is_object())
            ? reply["weights"]
            : reply;

    if (!weights.is_object())
    {
        console.out << weights.dump() << '\n';
        return;
    }

    const auto weight = [&weights](const char *key) -> std::string
    {
        const auto it = weights.find(key);
        if (it == weights.end() || !it->is_number())
        {
            return "?";
        }
        return formatCompact(it->get<double>());
    };

    std::vector<std::pair<std::string, std::string>> fields;
    for (const char *key : { "importance", "urgency", "age", "blocks", "urgency_horizon_days" })
    {
        const auto it = weights.find(key);
        if (it != weights.end() && it->is_number())
        {
            fields.emplace_back(key, formatCompact(it->get<double>()));
        }
    }
    printFields(console, fields);

    console.out << "  score = importance*" << weight("importance") << " + urgency*"
                << weight("urgency") << " + age*" << weight("age") << " + blocks*"
                << weight("blocks") << "   (urgency ramps over "
                << weight("urgency_horizon_days") << " days)\n";
}

// ---------------------------------------------------------------------------
// Commands
//
// Each handler is one method call plus a rendering of what came back. Nothing
// re-implements a rule the daemon owns: transitions, conflict checks and the
// full validation of a write are the store's job and arrive here as errors,
// which are printed verbatim.
// ---------------------------------------------------------------------------

/// queue [n] — the ranked queue, best first.
void runQueue(const Console &console, const std::vector<std::string> &args)
{
    // The documented default. A screenful, not the backlog: the console's job
    // is to say what to do next.
    std::int64_t limit = 10;
    if (!args.empty())
    {
        const Result<std::int64_t> parsed = parseInteger(args.front(), "queue: the limit");
        if (!parsed.ok())
        {
            printError(console, parsed.error());
            return;
        }
        if (parsed.value() < 0)
        {
            printError(console, Error::invalidArgument(
                                   "queue: the limit must not be negative, got '" + args.front()
                                       + "'"));
            return;
        }
        limit = parsed.value();
    }

    printTaskRows(console,
                  console.call("get_queue", nlohmann::json{ { "limit", limit } }),
                  "the queue is empty: nothing is open or in progress");
}

/// ls [status] — browse, rather than rank.
void runList(const Console &console, const std::vector<std::string> &args)
{
    nlohmann::json params = nlohmann::json::object();
    if (!args.empty())
    {
        // The vocabulary comes from the core parser rather than from a list
        // repeated here, so the console cannot offer a status the wire does not
        // know.
        const std::optional<TaskStatus> status = taskStatusFromString(args.front());
        if (!status.has_value())
        {
            printError(console, Error::invalidArgument(
                                   "ls: unknown status '" + args.front()
                                       + "'; expected one of: open, in_progress, done, archived"));
            return;
        }
        params["status"] = toString(*status);
    }

    printTaskRows(console, console.call("list_tasks", params), "no tasks match");
}

/// add <title> [imp=1..5] [due=...] [blocks=n] [tags=a,b] [notes=...]
void runAdd(const Console &console, const std::vector<std::string> &args)
{
    if (args.empty())
    {
        printError(console, Error::invalidArgument(
                               "add: a title is required - add <title> [imp=1..5] [due=+3d] "
                               "[blocks=n] [tags=a,b] [notes=\"...\"]"));
        return;
    }

    // The title may arrive quoted (tokenize honours quotes), so a title with
    // spaces is one argument here, not several.
    nlohmann::json params{ { "title", args.front() } };
    std::vector<std::string> seen;

    for (std::size_t i = 1; i < args.size(); ++i)
    {
        const Result<std::pair<std::string, std::string>> pair = parseKeyValue(args[i], "add");
        if (!pair.ok())
        {
            printError(console, pair.error());
            return;
        }
        const std::string key = pair.value().first;
        const std::string value = pair.value().second;

        const bool known = "imp" == key || "due" == key || "blocks" == key || "tags" == key
            || "notes" == key;
        if (!known)
        {
            printError(console, Error::invalidArgument(
                                   "add: unknown key '" + key
                                       + "'; expected one of: imp, due, blocks, tags, notes"));
            return;
        }

        // A repeated key is a typo, and letting the last one silently win is
        // exactly the kind of quiet precedence rule that makes a command line
        // untrustworthy.
        if (std::find(seen.begin(), seen.end(), key) != seen.end())
        {
            printError(console, Error::invalidArgument("add: duplicate key '" + key + "'"));
            return;
        }
        seen.push_back(key);

        // Validating imp and blocks here duplicates the store's rules on
        // purpose: the console knows which FLAG the user typed, so it can say
        // "imp must be between 1 and 5" instead of the daemon's "parameter
        // 'importance' is out of range for a 32-bit integer".
        if ("imp" == key)
        {
            const Result<std::int64_t> parsed = parseInteger(value, "add: imp");
            if (!parsed.ok())
            {
                printError(console, parsed.error());
                return;
            }
            if (parsed.value() < 1 || parsed.value() > 5)
            {
                printError(console, Error::invalidArgument(
                                       "add: imp must be between 1 and 5, got '" + value + "'"));
                return;
            }
            params["importance"] = parsed.value();
        }
        else if ("due" == key)
        {
            const Result<std::optional<std::int64_t>> due = Repl::parseDue(value, console.now);
            if (!due.ok())
            {
                printError(console, due.error());
                return;
            }
            if (due.value().has_value())
            {
                // "-" and "" mean "no deadline", which the API expresses by
                // simply omitting the parameter rather than by a null sentinel.
                params["due_at"] = *due.value();
            }
        }
        else if ("blocks" == key)
        {
            const Result<std::int64_t> parsed = parseInteger(value, "add: blocks");
            if (!parsed.ok())
            {
                printError(console, parsed.error());
                return;
            }
            if (parsed.value() < 0)
            {
                printError(console, Error::invalidArgument(
                                       "add: blocks must not be negative, got '" + value + "'"));
                return;
            }
            params["blocks"] = parsed.value();
        }
        else if ("tags" == key)
        {
            params["tags"] = splitTags(value);
        }
        else
        {
            params["notes"] = value;
        }
    }

    const RpcResult reply = console.call("add_task", params);
    if (!reply.ok())
    {
        printError(console, reply.error());
        return;
    }
    printTaskLine(console, "added", reply.value());
}

/// show <id> — one task, with the breakdown of its score.
void runShow(const Console &console, const std::vector<std::string> &args)
{
    if (args.empty())
    {
        printError(console, Error::invalidArgument("show: a task id is required - show <id>"));
        return;
    }

    const Result<std::int64_t> id = parseInteger(args.front(), "show: id");
    if (!id.ok())
    {
        printError(console, id.error());
        return;
    }

    const RpcResult reply = console.call("get_task", nlohmann::json{ { "id", id.value() } });
    if (!reply.ok())
    {
        printError(console, reply.error());
        return;
    }
    printTaskDetail(console, reply.value());
}

/// done / reopen / rm — one id in, the updated task (or a deletion receipt)
/// back. Sharing one handler keeps the three commands from drifting apart in
/// how they parse an id or report a failure.
void runIdCommand(const Console &console, const std::vector<std::string> &args,
                  const std::string &command, const std::string &method,
                  const std::string &verb)
{
    if (args.empty())
    {
        printError(console, Error::invalidArgument(command + ": a task id is required - " + command
                                                   + " <id>"));
        return;
    }

    const Result<std::int64_t> id = parseInteger(args.front(), command + ": id");
    if (!id.ok())
    {
        printError(console, id.error());
        return;
    }

    const RpcResult reply = console.call(method, nlohmann::json{ { "id", id.value() } });
    if (!reply.ok())
    {
        printError(console, reply.error());
        return;
    }
    printTaskLine(console, verb.c_str(), reply.value());
}

/// weights [key=value ...] — show, or retune, the ranking weights.
void runWeights(const Console &console, const std::vector<std::string> &args)
{
    if (args.empty())
    {
        const RpcResult reply = console.call("get_weights", nlohmann::json::object());
        if (!reply.ok())
        {
            printError(console, reply.error());
            return;
        }
        printWeights(console, reply.value());
        return;
    }

    nlohmann::json params = nlohmann::json::object();
    for (const std::string &token : args)
    {
        const Result<std::pair<std::string, std::string>> pair = parseKeyValue(token, "weights");
        if (!pair.ok())
        {
            printError(console, pair.error());
            return;
        }
        const std::string key = pair.value().first;

        const bool known = "importance" == key || "urgency" == key || "age" == key
            || "blocks" == key || "urgency_horizon_days" == key;
        if (!known)
        {
            printError(console, Error::invalidArgument(
                                   "weights: unknown key '" + key
                                       + "'; expected one of: importance, urgency, age, blocks, "
                                         "urgency_horizon_days"));
            return;
        }
        if (params.contains(key))
        {
            printError(console, Error::invalidArgument("weights: duplicate key '" + key + "'"));
            return;
        }

        // Only the number is checked here. Which values are LEGAL — finite,
        // non-negative, horizon above zero — is the store's rule and is
        // enforced once, there; a second copy here would be one more place to
        // forget when the rule changes.
        const Result<double> value = parseNumber(pair.value().second, "weights: " + key);
        if (!value.ok())
        {
            printError(console, value.error());
            return;
        }

        // An integral weight is sent as an integer, which is how the catalog
        // advertises these fields and how the defaults are stored; a fractional
        // one stays a float so urgency 0.5 remains expressible.
        if (std::fabs(value.value()) < 9e15 && std::floor(value.value()) == value.value())
        {
            params[key] = static_cast<std::int64_t>(value.value());
        }
        else
        {
            params[key] = value.value();
        }
    }

    const RpcResult reply = console.call("set_weights", params);
    if (!reply.ok())
    {
        printError(console, reply.error());
        return;
    }
    printWeights(console, reply.value());
}

/// stats — the backlog tallies.
void runStats(const Console &console, const std::vector<std::string> &args)
{
    static_cast<void>(args); // Takes no parameters.

    const RpcResult reply = console.call("get_stats", nlohmann::json::object());
    if (!reply.ok())
    {
        printError(console, reply.error());
        return;
    }
    if (!reply.value().is_object())
    {
        console.out << reply.value().dump() << '\n';
        return;
    }

    std::vector<std::pair<std::string, std::string>> fields;
    std::vector<std::string> shown;

    // get_stats carries the instant the daemon sampled along with the counts,
    // and rendering it as a wall-clock time is what keeps this command's "now"
    // the same kind of thing as status's.
    const std::optional<std::int64_t> now = readInteger(reply.value(), "now");
    if (now.has_value())
    {
        fields.emplace_back("now", formatTimestamp(*now));
        shown.emplace_back("now");
    }

    appendReportFields(reply.value(), shown, fields);
    if (fields.empty())
    {
        console.out << "the daemon reported no tallies\n";
        return;
    }
    printFields(console, fields);
}

/// status — the daemon's identity, then the tallies it merges into the same
/// reply.
void runStatus(const Console &console, const std::vector<std::string> &args)
{
    static_cast<void>(args); // Takes no parameters.

    const RpcResult reply = console.call("get_status", nlohmann::json::object());
    if (!reply.ok())
    {
        printError(console, reply.error());
        return;
    }
    const nlohmann::json &status = reply.value();
    if (!status.is_object())
    {
        console.out << status.dump() << '\n';
        return;
    }

    std::vector<std::pair<std::string, std::string>> fields;
    std::vector<std::string> shown;

    const std::string version = readString(status, "version");
    if (!version.empty())
    {
        fields.emplace_back("version", version);
        shown.emplace_back("version");
    }

    // Durations and instants are rendered, not dumped: "3600" is a number,
    // "1h" is the answer to the question being asked.
    const std::optional<std::int64_t> uptime = readInteger(status, "uptime_seconds");
    if (uptime.has_value())
    {
        fields.emplace_back("uptime", formatDuration(*uptime));
        shown.emplace_back("uptime_seconds");
    }

    const std::string path = readString(status, "db_path");
    if (!path.empty())
    {
        fields.emplace_back("db", path);
        shown.emplace_back("db_path");
    }

    const std::optional<std::int64_t> methods = readInteger(status, "method_count");
    if (methods.has_value())
    {
        fields.emplace_back("methods", std::to_string(*methods));
        shown.emplace_back("method_count");
    }

    const std::optional<std::int64_t> now = readInteger(status, "now");
    if (now.has_value())
    {
        fields.emplace_back("now", formatTimestamp(*now));
        shown.emplace_back("now");
    }

    appendReportFields(status, shown, fields);
    printFields(console, fields);
}

/// methods — the method catalog, which is also what MCP publishes as tools.
void runMethods(const Console &console, const std::vector<std::string> &args)
{
    static_cast<void>(args); // Takes no parameters.

    const RpcResult reply = console.call("describe_methods", nlohmann::json::object());
    if (!reply.ok())
    {
        printError(console, reply.error());
        return;
    }
    const nlohmann::json &catalog = reply.value();
    if (!catalog.is_array())
    {
        console.out << catalog.dump(2) << '\n';
        return;
    }

    for (const nlohmann::json &spec : catalog)
    {
        console.out << readString(spec, "name", "(unnamed method)") << '\n';

        const std::string description = readString(spec, "description");
        if (!description.empty())
        {
            console.out << "    " << description << '\n';
        }

        const auto schema = spec.is_object() ? spec.find("params_schema") : spec.end();
        if (schema == spec.end() || !schema->is_object())
        {
            continue;
        }
        const auto properties = schema->find("properties");
        if (properties == schema->end() || !properties->is_object())
        {
            continue;
        }

        // Parameter names in the reply's own key order. That order comes from
        // the JSON object rather than from this loop, so it is stable from run
        // to run and the console never invents an ordering of its own.
        std::string names;
        for (const auto &property : properties->items())
        {
            if (!names.empty())
            {
                names += ", ";
            }
            names += property.key();
        }
        console.out << "    params: " << (names.empty() ? std::string{ "(none)" } : names);

        const auto required = schema->find("required");
        if (required != schema->end() && required->is_array() && !required->empty())
        {
            std::string required_names;
            for (const nlohmann::json &name : *required)
            {
                if (!name.is_string())
                {
                    continue;
                }
                if (!required_names.empty())
                {
                    required_names += ", ";
                }
                required_names += name.get<std::string>();
            }
            if (!required_names.empty())
            {
                console.out << "  (required: " << required_names << ')';
            }
        }
        console.out << '\n';
    }
}

// ---------------------------------------------------------------------------
// The line scanner
// ---------------------------------------------------------------------------

/// What scanning one line produced.
///
/// A struct rather than a bare vector because the scanner has two things to
/// say: the tokens, and whether the line ended inside an open quote. The frozen
/// tokenize() signature returns only the vector, so the flag travels beside it
/// and is turned into a kInvalidArgument by executeLine — the caller that is
/// able to report it.
struct ScannedLine
{
    std::vector<std::string> tokens;
    bool unterminated_quote{ false };
};

/// Split one line into tokens, honouring double quotes and stripping comments.
///
/// Rules, applied left to right:
///   1. Whitespace outside quotes separates tokens.
///   2. '#' outside quotes starts a comment that runs to the end of the line,
///      so a scripted session can be annotated inline.
///   3. A double quote toggles "inside quotes". The quote characters are not
///      kept, and everything between a pair belongs to one token, which is what
///      lets a title contain spaces:  add "fix the WS bug"
///   4. A quote toggles mid-token as well (notes="a b"), which is how one
///      argument carries a value with spaces — the reading a shell user already
///      expects, and the reason notes needs no escaping beyond the quotes.
///   5. A line that ends inside quotes still yields the text it had collected,
///      so a typo cannot silently swallow the rest of the line, and the flag is
///      set so the caller can refuse the whole command rather than act on half
///      of it.
[[nodiscard]] ScannedLine splitTokens(const std::string &line)
{
    ScannedLine scanned;
    std::string current;
    bool in_token = false;
    bool in_quotes = false;

    for (const char ch : line)
    {
        if (in_quotes)
        {
            if ('"' == ch)
            {
                in_quotes = false;
            }
            else
            {
                current += ch;
            }
            continue;
        }

        if ('"' == ch)
        {
            in_quotes = true;
            // A quote starts a token even when it is empty: add "" has an empty
            // title, which the store then rejects, and that is a better outcome
            // than the argument vanishing.
            in_token = true;
            continue;
        }
        if ('#' == ch)
        {
            break;
        }
        if (isSpace(ch))
        {
            if (in_token)
            {
                scanned.tokens.push_back(current);
                current.clear();
                in_token = false;
            }
            continue;
        }

        current += ch;
        in_token = true;
    }

    if (in_token)
    {
        scanned.tokens.push_back(current);
    }
    scanned.unterminated_quote = in_quotes;
    return scanned;
}

} // namespace

// ---------------------------------------------------------------------------
// Repl
// ---------------------------------------------------------------------------

Repl::Repl(Caller caller, std::ostream &out)
    : m_caller{ std::move(caller) }, m_out{ out }
{
}

const char *Repl::helpText()
{
    // One literal, so the `help` command cannot drift from the grammar it
    // documents: everything here is the command table executeLine dispatches.
    return "taskPilot console - one command per line, '#' starts a comment\n"
           "\n"
           "  queue [n]              top n of the ranked queue (default 10)\n"
           "  ls [status]            list tasks, optionally filtered by status\n"
           "  add <title> [k=v ...]  create a task; keys: imp, due, blocks, tags, notes\n"
           "  show <id>              show one task in full\n"
           "  done <id>              mark a task complete\n"
           "  reopen <id>            put a completed task back in the queue\n"
           "  rm <id>                delete a task permanently\n"
           "  weights [k=v ...]      show, or update, the ranking weights\n"
           "  stats                  counts and tallies\n"
           "  status                 daemon version, uptime, db path\n"
           "  methods                the method catalog (what MCP exposes)\n"
           "  help                   this list\n"
           "  quit | exit            leave\n"
           "\n"
           "  Quote a title that contains spaces:  add \"fix the WS bug\" imp=5 due=+2d\n"
           "  due accepts:  +3d  +6h  +30m  today  tomorrow  YYYY-MM-DD  - (no deadline)\n"
           "  status is one of:  open  in_progress  done  archived\n"
           "  weights keys:  importance  urgency  blocks  age  urgency_horizon_days\n";
}

void Repl::run(std::istream &in)
{
    // No prompt is written: prompts and banners belong to the caller, and this
    // loop's output may be a pipe or a captured log where a prompt would be
    // noise. The loop ends on quit/exit (executeLine returns false) or on EOF,
    // which is the normal close of a piped session.
    std::string line;
    while (std::getline(in, line))
    {
        // Tolerate CRLF framing: a stray CR would otherwise ride along in the
        // last token of every line.
        if (!line.empty() && '\r' == line.back())
        {
            line.pop_back();
        }
        const bool keep_going = executeLine(line);
        // Flushed per line: the stream may be block-buffered (a pipe, a log),
        // where an unflushed console looks hung rather than quiet.
        m_out.flush();
        if (!keep_going)
        {
            return;
        }
    }
}

bool Repl::executeLine(const std::string &line)
{
    // 1. Scan the line. The one failure the scan can report — a line that ends
    //    inside an open quote — becomes a kInvalidArgument here, at the point
    //    where it can be reported, rather than a crash or a silently truncated
    //    command.
    const ScannedLine scanned = splitTokens(line);
    if (scanned.unterminated_quote)
    {
        printError(m_out, Error::invalidArgument(
                              "unbalanced double quote: close the quote or remove it"));
        return true;
    }

    // 2. Blank lines and comment-only lines do nothing at all — not even an
    //    "unknown command".
    if (scanned.tokens.empty())
    {
        return true;
    }

    const std::string &command = scanned.tokens.front();
    const std::vector<std::string> args(scanned.tokens.begin() + 1, scanned.tokens.end());

    // 3. Sample "now" once for the whole line: a command that both parses a
    //    relative deadline and marks an overdue row has to measure both against
    //    one instant. This is a display/input-side instant only — the daemon
    //    samples its own for ranking, and the two are never mixed.
    const Console console{ m_caller, m_out, SystemClock{}.nowEpochSeconds() };

    // 4. Dispatch. Each branch below is one control-socket method (see the file
    //    header): the console is a shell over the same catalog MCP publishes, so
    //    it can only ever do what the daemon can do, and no branch re-implements
    //    a rule the daemon owns.
    if ("quit" == command || "exit" == command)
    {
        return false;
    }
    if ("help" == command)
    {
        m_out << helpText();
        return true;
    }
    if ("queue" == command)
    {
        runQueue(console, args);
        return true;
    }
    if ("ls" == command)
    {
        runList(console, args);
        return true;
    }
    if ("add" == command)
    {
        runAdd(console, args);
        return true;
    }
    if ("show" == command)
    {
        runShow(console, args);
        return true;
    }
    if ("done" == command)
    {
        runIdCommand(console, args, "done", "complete_task", "completed");
        return true;
    }
    if ("reopen" == command)
    {
        runIdCommand(console, args, "reopen", "reopen_task", "reopened");
        return true;
    }
    if ("rm" == command)
    {
        runIdCommand(console, args, "rm", "delete_task", "deleted");
        return true;
    }
    if ("weights" == command)
    {
        runWeights(console, args);
        return true;
    }
    if ("stats" == command)
    {
        runStats(console, args);
        return true;
    }
    if ("status" == command)
    {
        runStatus(console, args);
        return true;
    }
    if ("methods" == command)
    {
        runMethods(console, args);
        return true;
    }

    // 5. Anything else is the caller's typo, and the session survives it: a
    //    console that exits on a bad command is useless for exploring.
    printError(m_out,
               Error::invalidArgument("unknown command '" + command
                                      + "'; type 'help' for the command list"));
    return true;
}

std::vector<std::string> Repl::tokenize(const std::string &line)
{
    // The header-declared view of the scan, for use where a failure cannot be
    // reported. The unterminated-quote flag is deliberately dropped here —
    // executeLine calls the scanner directly, because it needs the flag and
    // because a second scanning pass just to fetch a bool would be a second
    // place for the rules above to be got wrong.
    return splitTokens(line).tokens;
}

Result<std::optional<std::int64_t>> Repl::parseDue(const std::string &text, std::int64_t now)
{
    // This is the whole reason the console exists as more than a thin client:
    // the human-to-epoch conversion happens here and only here. The wire
    // protocol carries a plain integer, so no two clients have to agree on what
    // "tomorrow" means and the daemon never learns about local calendars.

    // Adapts an instant to a deadline, so the two different failures below (no
    // deadline, and an unusable one) cannot be confused.
    const auto asDeadline = [](const Result<std::int64_t> &instant)
        -> Result<std::optional<std::int64_t>>
    {
        if (!instant.ok())
        {
            return instant.error();
        }
        return std::optional<std::int64_t>{ instant.value() };
    };

    const std::string trimmed = trim(text);

    // 1. Empty or "-" means "no deadline". The API expresses that by an absent
    //    due_at rather than by a magic instant, so nullopt is what the caller
    //    passes through by leaving the parameter out.
    if (trimmed.empty() || "-" == trimmed)
    {
        return std::optional<std::int64_t>{};
    }

    // 2. Relative offsets.
    if ('+' == trimmed.front())
    {
        return asDeadline(relativeDeadline(trimmed, now));
    }

    // 3. Midnight boundaries. They resolve in LOCAL time because that is the
    //    day boundary the person typing at the prompt means; a UTC midnight
    //    would be yesterday afternoon for half the world.
    if ("today" == trimmed)
    {
        return asDeadline(localMidnightFrom(now, 0));
    }
    if ("tomorrow" == trimmed)
    {
        return asDeadline(localMidnightFrom(now, 1));
    }

    // 4. An absolute date, at local midnight for the same reason: "due
    //    2026-10-15" means the start of that day, which is what makes it due
    //    first thing that morning rather than at the end of it.
    const Result<CalendarDate> date = parseCalendarDate(trimmed);
    if (!date.ok())
    {
        return date.error();
    }
    return asDeadline(localMidnightOn(date.value().year, date.value().month, date.value().day));
}

} // namespace taskpilot
