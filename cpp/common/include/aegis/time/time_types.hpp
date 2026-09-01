#ifndef AEGIS_TIME_TIME_TYPES_HPP
#define AEGIS_TIME_TIME_TYPES_HPP

#include <compare>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace aegis::time {

enum class TimeError : std::uint8_t {
  none = 0,
  overflow,
  underflow,
  regression,
  invalid_duration,
  invalid_timestamp,
};

class DurationNs {
public:
  constexpr DurationNs() noexcept = default;
  constexpr explicit DurationNs(const std::int64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::int64_t value() const noexcept { return value_; }

  auto operator<=>(const DurationNs&) const = default;

private:
  std::int64_t value_{};
};

template <typename Tag, typename Representation> class TimestampNs {
  static_assert(std::is_integral_v<Representation>);
  static_assert(sizeof(Representation) == sizeof(std::uint64_t));

public:
  using representation_type = Representation;

  constexpr TimestampNs() noexcept = default;
  constexpr explicit TimestampNs(const Representation value) noexcept : value_(value) {}

  [[nodiscard]] constexpr Representation value() const noexcept { return value_; }

  auto operator<=>(const TimestampNs&) const = default;

private:
  Representation value_{};
};

struct WallClockTimeTag;
struct MonotonicTimeTag;
struct ExchangeTimeTag;
struct HardwareReceiveTimeTag;

using WallClockTimeNs = TimestampNs<WallClockTimeTag, std::int64_t>;
using MonotonicTimeNs = TimestampNs<MonotonicTimeTag, std::uint64_t>;
using ExchangeTimeNs = TimestampNs<ExchangeTimeTag, std::int64_t>;
using HardwareReceiveTimeNs = TimestampNs<HardwareReceiveTimeTag, std::int64_t>;

// Public members preserve aggregate initialization and a branch-free result
// representation for the execution path.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes,
// readability-redundant-member-init)
template <typename Value> struct TimeResult {
  Value value{};
  TimeError error{TimeError::none};

  [[nodiscard]] constexpr bool ok() const noexcept { return error == TimeError::none; }
};
// NOLINTEND(misc-non-private-member-variables-in-classes,
// readability-redundant-member-init)

namespace detail {

[[nodiscard]] constexpr TimeResult<std::int64_t>
checked_add_signed(const std::int64_t left, const std::int64_t right) noexcept {
  constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
  constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
  if (right > 0 && left > maximum - right) {
    return {.error = TimeError::overflow};
  }
  if (right < 0 && left < minimum - right) {
    return {.error = TimeError::underflow};
  }
  return {.value = left + right};
}

[[nodiscard]] constexpr TimeResult<std::int64_t>
checked_subtract_signed(const std::int64_t left, const std::int64_t right) noexcept {
  constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
  constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
  if (right > 0 && left < minimum + right) {
    return {.error = TimeError::underflow};
  }
  if (right < 0 && left > maximum + right) {
    return {.error = TimeError::overflow};
  }
  return {.value = left - right};
}

[[nodiscard]] constexpr std::uint64_t
signed_magnitude(const std::int64_t value) noexcept {
  if (value >= 0) {
    return static_cast<std::uint64_t>(value);
  }
  return static_cast<std::uint64_t>(-(value + 1)) + 1U;
}

} // namespace detail

[[nodiscard]] constexpr std::uint64_t magnitude(const DurationNs value) noexcept {
  return detail::signed_magnitude(value.value());
}

[[nodiscard]] constexpr TimeResult<DurationNs>
checked_add(const DurationNs left, const DurationNs right) noexcept {
  const auto result = detail::checked_add_signed(left.value(), right.value());
  return {.value = DurationNs{result.value}, .error = result.error};
}

[[nodiscard]] constexpr TimeResult<DurationNs>
checked_subtract(const DurationNs left, const DurationNs right) noexcept {
  const auto result = detail::checked_subtract_signed(left.value(), right.value());
  return {.value = DurationNs{result.value}, .error = result.error};
}

template <typename Tag, typename Representation>
[[nodiscard]] constexpr TimeResult<TimestampNs<Tag, Representation>>
checked_add(const TimestampNs<Tag, Representation> timestamp,
            const DurationNs duration) noexcept {
  if constexpr (std::is_signed_v<Representation>) {
    const auto result = detail::checked_add_signed(timestamp.value(), duration.value());
    return {.value = TimestampNs<Tag, Representation>{result.value},
            .error = result.error};
  } else {
    const auto raw = timestamp.value();
    if (duration.value() >= 0) {
      const auto increment = static_cast<std::uint64_t>(duration.value());
      if (raw > std::numeric_limits<std::uint64_t>::max() - increment) {
        return {.error = TimeError::overflow};
      }
      return {.value = TimestampNs<Tag, Representation>{raw + increment}};
    }
    const auto decrement = detail::signed_magnitude(duration.value());
    if (raw < decrement) {
      return {.error = TimeError::underflow};
    }
    return {.value = TimestampNs<Tag, Representation>{raw - decrement}};
  }
}

template <typename Tag, typename Representation>
[[nodiscard]] constexpr TimeResult<TimestampNs<Tag, Representation>>
checked_subtract(const TimestampNs<Tag, Representation> timestamp,
                 const DurationNs duration) noexcept {
  if constexpr (std::is_signed_v<Representation>) {
    const auto result =
        detail::checked_subtract_signed(timestamp.value(), duration.value());
    return {.value = TimestampNs<Tag, Representation>{result.value},
            .error = result.error};
  } else {
    const auto raw = timestamp.value();
    if (duration.value() >= 0) {
      const auto decrement = static_cast<std::uint64_t>(duration.value());
      if (raw < decrement) {
        return {.error = TimeError::underflow};
      }
      return {.value = TimestampNs<Tag, Representation>{raw - decrement}};
    }
    const auto increment = detail::signed_magnitude(duration.value());
    if (raw > std::numeric_limits<std::uint64_t>::max() - increment) {
      return {.error = TimeError::overflow};
    }
    return {.value = TimestampNs<Tag, Representation>{raw + increment}};
  }
}

template <typename Tag, typename Representation>
[[nodiscard]] constexpr TimeResult<DurationNs>
checked_elapsed(const TimestampNs<Tag, Representation> start,
                const TimestampNs<Tag, Representation> end) noexcept {
  if (end < start) {
    return {.error = TimeError::regression};
  }
  if constexpr (std::is_signed_v<Representation>) {
    const auto result = detail::checked_subtract_signed(end.value(), start.value());
    return {.value = DurationNs{result.value}, .error = result.error};
  } else {
    const auto elapsed = end.value() - start.value();
    if (elapsed >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return {.error = TimeError::overflow};
    }
    return {.value = DurationNs{static_cast<std::int64_t>(elapsed)}};
  }
}

template <typename Tag>
[[nodiscard]] constexpr TimeResult<DurationNs>
checked_difference(const TimestampNs<Tag, std::int64_t> left,
                   const TimestampNs<Tag, std::int64_t> right) noexcept {
  const auto result = detail::checked_subtract_signed(left.value(), right.value());
  return {.value = DurationNs{result.value}, .error = result.error};
}

static_assert(sizeof(DurationNs) == 8);
static_assert(sizeof(WallClockTimeNs) == 8);
static_assert(sizeof(MonotonicTimeNs) == 8);
static_assert(std::is_trivially_copyable_v<DurationNs>);
static_assert(std::is_trivially_copyable_v<MonotonicTimeNs>);
static_assert(!std::is_same_v<WallClockTimeNs, ExchangeTimeNs>);
static_assert(!std::is_convertible_v<WallClockTimeNs, MonotonicTimeNs>);

} // namespace aegis::time

#endif // AEGIS_TIME_TIME_TYPES_HPP
