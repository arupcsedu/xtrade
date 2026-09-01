#include "aegis/time/clocks.hpp"

#include <chrono>
#include <cstdint>

namespace aegis::time {

TimeResult<MonotonicTimeNs> SystemMonotonicClock::monotonic_now() const noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  if (nanoseconds < 0) {
    return {.error = TimeError::invalid_timestamp};
  }
  return {.value = MonotonicTimeNs{static_cast<std::uint64_t>(nanoseconds)}};
}

TimeResult<WallClockTimeNs> SystemWallClock::wall_now() const noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return {.value = WallClockTimeNs{nanoseconds}};
}

TimeResult<MonotonicTimeNs> TestMonotonicClock::monotonic_now() const noexcept {
  return {.value = current_};
}

void TestMonotonicClock::set(const MonotonicTimeNs value) noexcept { current_ = value; }

TimeError TestMonotonicClock::advance(const DurationNs duration) noexcept {
  if (duration.value() < 0) {
    return TimeError::invalid_duration;
  }
  const auto result = checked_add(current_, duration);
  if (!result.ok()) {
    return result.error;
  }
  current_ = result.value;
  return TimeError::none;
}

TimeResult<WallClockTimeNs> TestWallClock::wall_now() const noexcept {
  return {.value = current_};
}

void TestWallClock::set(const WallClockTimeNs value) noexcept { current_ = value; }

TimeError TestWallClock::advance(const DurationNs duration) noexcept {
  const auto result = checked_add(current_, duration);
  if (!result.ok()) {
    return result.error;
  }
  current_ = result.value;
  return TimeError::none;
}

TimeResult<MonotonicTimeNs> SimulatedClock::monotonic_now() const noexcept {
  return {.value = monotonic_time_};
}

TimeResult<WallClockTimeNs> SimulatedClock::wall_now() const noexcept {
  return {.value = wall_time_};
}

TimeError SimulatedClock::advance(const DurationNs duration) noexcept {
  if (duration.value() < 0) {
    return TimeError::invalid_duration;
  }
  const auto next_wall = checked_add(wall_time_, duration);
  if (!next_wall.ok()) {
    return next_wall.error;
  }
  const auto next_monotonic = checked_add(monotonic_time_, duration);
  if (!next_monotonic.ok()) {
    return next_monotonic.error;
  }
  wall_time_ = next_wall.value;
  monotonic_time_ = next_monotonic.value;
  return TimeError::none;
}

TimeError SimulatedClock::jump_wall(const DurationNs delta) noexcept {
  const auto next_wall = checked_add(wall_time_, delta);
  if (!next_wall.ok()) {
    return next_wall.error;
  }
  wall_time_ = next_wall.value;
  return TimeError::none;
}

void SimulatedClock::set_monotonic(const MonotonicTimeNs value) noexcept {
  monotonic_time_ = value;
}

} // namespace aegis::time
