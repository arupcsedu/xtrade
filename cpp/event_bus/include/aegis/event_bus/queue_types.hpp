#ifndef AEGIS_EVENT_BUS_QUEUE_TYPES_HPP
#define AEGIS_EVENT_BUS_QUEUE_TYPES_HPP

#include <cstddef>
#include <cstdint>

namespace aegis::event_bus {

enum class OverloadPolicy : std::uint8_t {
  reject_newest = 1,
  fail_closed = 2,
};

enum class EnqueueStatus : std::uint8_t {
  accepted = 1,
  full = 2,
  contention = 3,
  invalidated = 4,
};

enum class DequeueStatus : std::uint8_t {
  item = 1,
  empty = 2,
  producer_inflight = 3,
  invalidated = 4,
};

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct QueueMetrics {
  std::uint64_t producer_sequence{};
  std::uint64_t consumer_sequence{};
  std::uint64_t consumer_lag_events{};
  std::uint64_t maximum_consumer_lag_events{};
  std::uint64_t accepted_count{};
  std::uint64_t consumed_count{};
  std::uint64_t full_rejection_count{};
  std::uint64_t contention_rejection_count{};
  std::uint64_t invalidation_count{};
  bool invalidated{false};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] constexpr bool is_power_of_two(const std::size_t value) noexcept {
  return value != 0U && (value & (value - 1U)) == 0U;
}

} // namespace aegis::event_bus

#endif // AEGIS_EVENT_BUS_QUEUE_TYPES_HPP
