#include "aegis/time/clock_jump.hpp"

namespace aegis::time {

ClockJumpResult
WallClockJumpDetector::observe(const WallClockTimeNs wall_time,
                               const MonotonicTimeNs monotonic_time) noexcept {
  if (!previous_.has_value()) {
    previous_ = Sample{.wall_time = wall_time, .monotonic_time = monotonic_time};
    return {};
  }

  if (monotonic_time <= previous_->monotonic_time) {
    return {.state = ClockJumpState::monotonic_regression,
            .error = TimeError::regression};
  }

  const auto monotonic_delta =
      checked_elapsed(previous_->monotonic_time, monotonic_time);
  if (!monotonic_delta.ok()) {
    return {.state = ClockJumpState::monotonic_regression,
            .error = monotonic_delta.error};
  }
  const auto wall_delta = checked_difference(wall_time, previous_->wall_time);
  if (!wall_delta.ok()) {
    return {.state = ClockJumpState::arithmetic_error, .error = wall_delta.error};
  }
  const auto residual = checked_subtract(wall_delta.value, monotonic_delta.value);
  if (!residual.ok()) {
    return {.state = ClockJumpState::arithmetic_error, .error = residual.error};
  }

  previous_ = Sample{.wall_time = wall_time, .monotonic_time = monotonic_time};
  if (magnitude(residual.value) <= tolerance_ns_) {
    return {.state = ClockJumpState::stable, .residual = residual.value};
  }
  const auto state = residual.value.value() < 0 ? ClockJumpState::backward_jump
                                                : ClockJumpState::forward_jump;
  return {.state = state, .residual = residual.value};
}

void WallClockJumpDetector::reset() noexcept { previous_.reset(); }

} // namespace aegis::time
