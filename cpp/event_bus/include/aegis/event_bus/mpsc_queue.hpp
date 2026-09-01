#ifndef AEGIS_EVENT_BUS_MPSC_QUEUE_HPP
#define AEGIS_EVENT_BUS_MPSC_QUEUE_HPP

#include "aegis/event_bus/cache_line.hpp"
#include "aegis/event_bus/queue_types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace aegis::event_bus {

template <typename Value, std::size_t Capacity,
          std::size_t MaximumProducerRetries = 64U>
class MpscQueue final {
public:
  static_assert(std::is_trivially_copyable_v<Value>);
  static_assert(Capacity >= 2U && is_power_of_two(Capacity));
  static_assert(MaximumProducerRetries != 0U);

  MpscQueue() noexcept {
    for (std::size_t index = 0U; index < Capacity; ++index) {
      cells_[index].sequence.store(index, std::memory_order_relaxed);
    }
  }

  [[nodiscard]] EnqueueStatus
  enqueue(const Value& value,
          const OverloadPolicy policy = OverloadPolicy::reject_newest) noexcept {
    if (invalidated_.value.load(std::memory_order_acquire) != 0U) {
      return EnqueueStatus::invalidated;
    }
    auto position = enqueue_position_.value.load(std::memory_order_relaxed);
    for (std::size_t retry = 0U; retry < MaximumProducerRetries; ++retry) {
      auto& cell = cells_[position & kIndexMask];
      const auto sequence = cell.sequence.load(std::memory_order_acquire);
      if (sequence == position) {
        if (enqueue_position_.value.compare_exchange_weak(position, position + 1U,
                                                          std::memory_order_relaxed,
                                                          std::memory_order_relaxed)) {
          cell.value = value;
          // Release publishes the payload to the single consumer.
          cell.sequence.store(position + 1U, std::memory_order_release);
          accepted_count_.value.fetch_add(1U, std::memory_order_relaxed);
          const auto consumer = dequeue_position_.value.load(std::memory_order_acquire);
          // A fast consumer can pass this individual producer immediately after
          // publication. A released cell can also become visible just before
          // its cursor update, so keep this sampled metric within capacity.
          const auto observed_lag =
              position + 1U >= consumer ? position + 1U - consumer : 0U;
          update_maximum_lag(observed_lag > static_cast<std::uint64_t>(Capacity)
                                 ? static_cast<std::uint64_t>(Capacity)
                                 : observed_lag);
          return EnqueueStatus::accepted;
        }
        continue;
      }
      // A sequence behind the claimed producer position belongs to the prior
      // ring generation, so the consumer has not released this cell yet.
      if (sequence < position) {
        full_rejections_.value.fetch_add(1U, std::memory_order_relaxed);
        if (policy == OverloadPolicy::fail_closed) {
          invalidate();
          return EnqueueStatus::invalidated;
        }
        return EnqueueStatus::full;
      }
      position = enqueue_position_.value.load(std::memory_order_relaxed);
    }
    contention_rejections_.value.fetch_add(1U, std::memory_order_relaxed);
    return EnqueueStatus::contention;
  }

  [[nodiscard]] DequeueStatus dequeue(Value& output) noexcept {
    if (invalidated_.value.load(std::memory_order_acquire) != 0U) {
      return DequeueStatus::invalidated;
    }
    const auto position = dequeue_position_.value.load(std::memory_order_relaxed);
    auto& cell = cells_[position & kIndexMask];
    const auto sequence = cell.sequence.load(std::memory_order_acquire);
    if (sequence != position + 1U) {
      return enqueue_position_.value.load(std::memory_order_acquire) == position
                 ? DequeueStatus::empty
                 : DequeueStatus::producer_inflight;
    }
    output = cell.value;
    // Release makes this cell reusable by a future producer generation.
    cell.sequence.store(position + Capacity, std::memory_order_release);
    dequeue_position_.value.store(position + 1U, std::memory_order_release);
    consumed_count_.value.fetch_add(1U, std::memory_order_relaxed);
    return DequeueStatus::item;
  }

  void invalidate() noexcept {
    if (invalidated_.value.exchange(1U, std::memory_order_acq_rel) == 0U) {
      invalidation_count_.value.fetch_add(1U, std::memory_order_relaxed);
    }
  }

  [[nodiscard]] QueueMetrics metrics() const noexcept {
    const auto consumer = dequeue_position_.value.load(std::memory_order_acquire);
    const auto producer = enqueue_position_.value.load(std::memory_order_acquire);
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
        .contention_rejection_count =
            contention_rejections_.value.load(std::memory_order_relaxed),
        .invalidation_count = invalidation_count_.value.load(std::memory_order_relaxed),
        .invalidated = invalidated_.value.load(std::memory_order_acquire) != 0U};
  }

  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
  struct alignas(kCacheLineBytes) Cell {
    std::atomic<std::uint64_t> sequence{0U};
    Value value{};
  };

  void update_maximum_lag(const std::uint64_t lag) noexcept {
    auto observed = maximum_lag_.value.load(std::memory_order_relaxed);
    while (observed < lag &&
           !maximum_lag_.value.compare_exchange_weak(
               observed, lag, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
  }

  static constexpr std::uint64_t kIndexMask = Capacity - 1U;
  alignas(kCacheLineBytes) std::array<Cell, Capacity> cells_{};
  PaddedAtomicU64 enqueue_position_;
  PaddedAtomicU64 dequeue_position_;
  PaddedAtomicU64 accepted_count_;
  PaddedAtomicU64 consumed_count_;
  PaddedAtomicU64 full_rejections_;
  PaddedAtomicU64 contention_rejections_;
  PaddedAtomicU64 maximum_lag_;
  PaddedAtomicU64 invalidated_;
  PaddedAtomicU64 invalidation_count_;
};

} // namespace aegis::event_bus

#endif // AEGIS_EVENT_BUS_MPSC_QUEUE_HPP
