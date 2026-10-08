// Clock.cpp — the one real-clock implementation
//
// Every other time-dependent part of taskPilot takes a Clock&, so this file
// is the single place where the process touches the operating system clock.
// That is the whole value of the abstraction: if a test ever produces a
// time-dependent result, the bug is either here or in a caller that ignored
// its injected clock.

#include "core/Clock.hpp"

#include <chrono>
#include <cstdint>

namespace taskpilot
{

// Seconds since the Unix epoch, UTC.
//
// Why system_clock and not steady_clock:
//   1. steady_clock has an unspecified origin (usually boot). Its values are
//      meaningless outside one process lifetime, and these timestamps are
//      written to SQLite and compared against deadlines a human typed in.
//   2. system_clock is the only clock the standard pins to the Unix epoch.
//      It can jump (NTP, manual change), but for "what day is it" semantics
//      that is the correct behaviour — a deadline is wall-clock, not uptime.
//
// The conversion, step by step:
//   1. Sample the clock once. Calling now() twice could straddle a second
//      boundary and make two values read from one call disagree.
//   2. duration_cast to seconds. system_clock's period is nanoseconds on
//      Linux and is platform-defined in general, so reading .count() of the
//      raw duration would return a platform-dependent unit — exactly the
//      unit ambiguity the Task header forbids.
//   3. static_cast to int64. seconds::rep is not required by the standard to
//      be 64-bit (it is `long` in libstdc++), and the project-wide wire type
//      is int64, so the narrowing is made explicit rather than implicit.
//
// Truncation, not rounding, is intentional: duration_cast toward zero reports
// the second that is currently in progress, which is how every timestamp a
// human won't read but a comparison must be exact about behaves.
std::int64_t SystemClock::nowEpochSeconds() const
{
    const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    const std::chrono::seconds since_epoch =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
    return static_cast<std::int64_t>(since_epoch.count());
}

} // namespace taskpilot
