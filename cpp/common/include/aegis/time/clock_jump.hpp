#ifndef AEGIS_TIME_CLOCK_JUMP_HPP
#define AEGIS_TIME_CLOCK_JUMP_HPP

#include "aegis/time/time_types.hpp"

#include <cstdint>
#include <optional>

namespace aegis::time {

enum class ClockJumpState : std::uint8_t {
  first_sample = 0,
  stable,
  forward_jump,
  backward_jump,
  monotonic_regression,
  arithmetic_error,
};

// NOLINTBEGIN(misc-non-private-member-variables-in-classes,
// readability-redundant-member-init)
struct ClockJumpResult {
  ClockJumpState state{ClockJumpState::first_sample};
  DurationNs residual{};
  TimeError error{TimeError::none};
};
// NOLINTEND(misc-non-private-member-variables-in-classes,
// readability-redundant-member-init)

class WallClockJumpDetector {
public:
  explicit constexpr WallClockJumpDetector(const std::uint64_t tolerance_ns) noexcept
      : tolerance_ns_(tolerance_ns) {}

  [[nodiscard]] ClockJumpResult observe(WallClockTimeNs wall_time,
                                        MonotonicTimeNs monotonic_time) noexcept;
  void reset() noexcept;

private:
  struct Sample {
    WallClockTimeNs wall_time;
    MonotonicTimeNs monotonic_time;
  };

  std::optional<Sample> previous_;
  std::uint64_t tolerance_ns_{};
};

} // namespace aegis::time

#endif // AEGIS_TIME_CLOCK_JUMP_HPP
