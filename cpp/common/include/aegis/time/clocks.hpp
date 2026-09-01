#ifndef AEGIS_TIME_CLOCKS_HPP
#define AEGIS_TIME_CLOCKS_HPP

#include "aegis/time/time_types.hpp"

namespace aegis::time {

class MonotonicClock {
public:
  virtual ~MonotonicClock() = default;

  [[nodiscard]] virtual TimeResult<MonotonicTimeNs> monotonic_now() const noexcept = 0;
};

class WallClock {
public:
  virtual ~WallClock() = default;

  [[nodiscard]] virtual TimeResult<WallClockTimeNs> wall_now() const noexcept = 0;
};

// These adapters are the only part of the library that reads an operating-system
// clock. Calling a named read method is always explicit; state-machine and
// latency code never reads a clock implicitly.
class SystemMonotonicClock final : public MonotonicClock {
public:
  [[nodiscard]] TimeResult<MonotonicTimeNs> monotonic_now() const noexcept override;
};

class SystemWallClock final : public WallClock {
public:
  [[nodiscard]] TimeResult<WallClockTimeNs> wall_now() const noexcept override;
};

class TestMonotonicClock final : public MonotonicClock {
public:
  constexpr explicit TestMonotonicClock(
      const MonotonicTimeNs initial = MonotonicTimeNs{}) noexcept
      : current_(initial) {}

  [[nodiscard]] TimeResult<MonotonicTimeNs> monotonic_now() const noexcept override;
  void set(MonotonicTimeNs value) noexcept;
  [[nodiscard]] TimeError advance(DurationNs duration) noexcept;

private:
  MonotonicTimeNs current_;
};

class TestWallClock final : public WallClock {
public:
  constexpr explicit TestWallClock(
      const WallClockTimeNs initial = WallClockTimeNs{}) noexcept
      : current_(initial) {}

  [[nodiscard]] TimeResult<WallClockTimeNs> wall_now() const noexcept override;
  void set(WallClockTimeNs value) noexcept;
  [[nodiscard]] TimeError advance(DurationNs duration) noexcept;

private:
  WallClockTimeNs current_;
};

class SimulatedClock final : public MonotonicClock, public WallClock {
public:
  constexpr SimulatedClock(const WallClockTimeNs wall_time,
                           const MonotonicTimeNs monotonic_time) noexcept
      : wall_time_(wall_time), monotonic_time_(monotonic_time) {}

  [[nodiscard]] TimeResult<MonotonicTimeNs> monotonic_now() const noexcept override;
  [[nodiscard]] TimeResult<WallClockTimeNs> wall_now() const noexcept override;

  [[nodiscard]] TimeError advance(DurationNs duration) noexcept;
  [[nodiscard]] TimeError jump_wall(DurationNs delta) noexcept;
  void set_monotonic(MonotonicTimeNs value) noexcept;

private:
  WallClockTimeNs wall_time_;
  MonotonicTimeNs monotonic_time_;
};

} // namespace aegis::time

#endif // AEGIS_TIME_CLOCKS_HPP
