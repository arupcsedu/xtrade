#ifndef AEGIS_EVENT_BUS_SPSC_RING_HPP
#define AEGIS_EVENT_BUS_SPSC_RING_HPP

#include "aegis/event_bus/cache_line.hpp"
#include "aegis/event_bus/queue_types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace aegis::event_bus {

template <typename Value, std::size_t Capacity> class SpscRing final {
public:
  static_assert(std::is_trivially_copyable_v<Value>);
  static_assert(Capacity >= 2U && is_power_of_two(Capacity));

  [[nodiscard]] EnqueueStatus
  enqueue(const Value& value,
          const OverloadPolicy policy = OverloadPolicy::reject_newest) noexcept {
    if (invalidated_.value.load(std::memory_order_acquire) != 0U) {
      return EnqueueStatus::invalidated;
    }
    const auto tail = producer_sequence_.value.load(std::memory_order_relaxed);
    const auto head = consumer_sequence_.value.load(std::memory_order_acquire);
    if (tail - head >= Capacity) {
      full_rejections_.value.fetch_add(1U, std::memory_order_relaxed);
      if (policy == OverloadPolicy::fail_closed) {
        invalidate();
        return EnqueueStatus::invalidated;
      }
      return EnqueueStatus::full;
    }
    values_[tail & kIndexMask] = value;
    // Release publishes the completed slot write. The consumer's acquire load
    // is the only synchronization edge needed for SPSC payload visibility.
    producer_sequence_.value.store(tail + 1U, std::memory_order_release);
    accepted_count_.value.fetch_add(1U, std::memory_order_relaxed);
    update_maximum_lag(tail + 1U - head);
    return EnqueueStatus::accepted;
  }

  [[nodiscard]] DequeueStatus dequeue(Value& output) noexcept {
    if (invalidated_.value.load(std::memory_order_acquire) != 0U) {
      return DequeueStatus::invalidated;
    }
    const auto head = consumer_sequence_.value.load(std::memory_order_relaxed);
    const auto tail = producer_sequence_.value.load(std::memory_order_acquire);
    if (head == tail) {
      return DequeueStatus::empty;
    }
    output = values_[head & kIndexMask];
    // Release makes the slot reusable only after the payload copy completes.
    consumer_sequence_.value.store(head + 1U, std::memory_order_release);
    consumed_count_.value.fetch_add(1U, std::memory_order_relaxed);
    return DequeueStatus::item;
  }

  void invalidate() noexcept {
    if (invalidated_.value.exchange(1U, std::memory_order_acq_rel) == 0U) {
      invalidation_count_.value.fetch_add(1U, std::memory_order_relaxed);
    }
  }

  // Quiescent-only restart operation. Concurrent reset is deliberately not
  // supported because it would erase ownership and sequence evidence.
  void reset_quiescent() noexcept {
    consumer_sequence_.value.store(0U, std::memory_order_relaxed);
    producer_sequence_.value.store(0U, std::memory_order_relaxed);
    accepted_count_.value.store(0U, std::memory_order_relaxed);
    consumed_count_.value.store(0U, std::memory_order_relaxed);
    full_rejections_.value.store(0U, std::memory_order_relaxed);
    maximum_lag_.value.store(0U, std::memory_order_relaxed);
    invalidation_count_.value.store(0U, std::memory_order_relaxed);
    invalidated_.value.store(0U, std::memory_order_release);
  }

  [[nodiscard]] QueueMetrics metrics() const noexcept {
    const auto consumer = consumer_sequence_.value.load(std::memory_order_acquire);
    const auto producer = producer_sequence_.value.load(std::memory_order_acquire);
    const auto sampled_lag = producer - consumer;
    return {
        .producer_sequence = producer,
        .consumer_sequence = consumer,
        // A lock-free metrics snapshot is approximate under movement. Clamp a
        // consumer-first/producer-second sample to the physical capacity.
        .consumer_lag_events = sampled_lag > static_cast<std::uint64_t>(Capacity)
                                   ? static_cast<std::uint64_t>(Capacity)
                                   : sampled_lag,
        .maximum_consumer_lag_events =
            maximum_lag_.value.load(std::memory_order_relaxed),
        .accepted_count = accepted_count_.value.load(std::memory_order_relaxed),
        .consumed_count = consumed_count_.value.load(std::memory_order_relaxed),
        .full_rejection_count = full_rejections_.value.load(std::memory_order_relaxed),
        .invalidation_count = invalidation_count_.value.load(std::memory_order_relaxed),
        .invalidated = invalidated_.value.load(std::memory_order_acquire) != 0U};
  }

  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
  void update_maximum_lag(const std::uint64_t lag) noexcept {
    auto observed = maximum_lag_.value.load(std::memory_order_relaxed);
    while (observed < lag &&
           !maximum_lag_.value.compare_exchange_weak(
               observed, lag, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
  }

  static constexpr std::uint64_t kIndexMask = Capacity - 1U;
  alignas(kCacheLineBytes) std::array<Value, Capacity> values_{};
  PaddedAtomicU64 producer_sequence_;
  PaddedAtomicU64 consumer_sequence_;
  PaddedAtomicU64 accepted_count_;
  PaddedAtomicU64 consumed_count_;
  PaddedAtomicU64 full_rejections_;
  PaddedAtomicU64 maximum_lag_;
  PaddedAtomicU64 invalidated_;
  PaddedAtomicU64 invalidation_count_;
};

} // namespace aegis::event_bus

#endif // AEGIS_EVENT_BUS_SPSC_RING_HPP
