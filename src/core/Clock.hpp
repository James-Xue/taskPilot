#pragma once
// Clock.hpp — injectable time source
//
// Ranking is a function of "now": urgency depends on the distance to a
// deadline, and aging depends on how long a task has waited. If the scoring
// engine read the wall clock directly, every test of it would be a
// time-dependent flake and no test could assert an exact score.
//
// So the clock is a dependency. Production code passes SystemClock; tests
// pass FixedClock and get fully deterministic numbers.

#include <cstdint>

namespace taskpilot
{

/// Abstract epoch-seconds source.
///
/// Thread-safety: implementations must be safe to call from multiple threads
/// concurrently. Both implementations below are.
class Clock
{
  public:
    virtual ~Clock() = default;

    /// Seconds since the Unix epoch, UTC.
    [[nodiscard]] virtual std::int64_t nowEpochSeconds() const = 0;
};

/// Reads the real system clock.
class SystemClock final : public Clock
{
  public:
    [[nodiscard]] std::int64_t nowEpochSeconds() const override;
};

/// A clock frozen at a caller-supplied instant, for deterministic tests.
/// Mutable via setNow() so a test can advance time between assertions.
class FixedClock final : public Clock
{
  public:
    explicit FixedClock(std::int64_t epoch_seconds) : m_now{ epoch_seconds } {}

    [[nodiscard]] std::int64_t nowEpochSeconds() const override { return m_now; }

    void setNow(std::int64_t epoch_seconds) { m_now = epoch_seconds; }

  private:
    std::int64_t m_now;
};

/// Seconds in one day. Named so the scoring code reads as intent rather than
/// as a magic 86400.
inline constexpr std::int64_t kSecondsPerDay{ 86400 };

} // namespace taskpilot
