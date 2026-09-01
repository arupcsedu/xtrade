#ifndef AEGIS_EVENT_BUS_SNAPSHOT_STORE_HPP
#define AEGIS_EVENT_BUS_SNAPSHOT_STORE_HPP

#include "aegis/event_bus/cache_line.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace aegis::event_bus {

enum class SnapshotPublishStatus : std::uint8_t {
  published = 1,
  no_free_slot = 2,
  uninitialized = 3,
};

enum class SnapshotReadStatus : std::uint8_t {
  snapshot = 1,
  empty = 2,
  busy = 3,
  corrupt = 4,
  reader_overflow = 5,
  uninitialized = 6,
};

using SnapshotChecksumFunction = std::uint64_t (*)(const void*) noexcept;

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct SnapshotStoreMetrics {
  std::uint64_t published_generation{};
  std::uint64_t publish_count{};
  std::uint64_t read_count{};
  std::uint64_t no_free_slot_count{};
  std::uint64_t corrupt_read_count{};
  std::uint64_t active_readers{};
  std::uint32_t stale_reader_slots{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

template <typename Value, std::size_t SlotCount = 3U>
class ImmutableSnapshotStore final {
public:
  static_assert(std::is_trivially_copyable_v<Value>);
  static_assert(SlotCount >= 3U);

  class ReadHandle final {
  public:
    ReadHandle() noexcept = default;
    ~ReadHandle() { release(); }

    ReadHandle(const ReadHandle&) = delete;
    ReadHandle& operator=(const ReadHandle&) = delete;

    ReadHandle(ReadHandle&& other) noexcept { move_from(other); }

    ReadHandle& operator=(ReadHandle&& other) noexcept {
      if (this != &other) {
        release();
        move_from(other);
      }
      return *this;
    }

    [[nodiscard]] const Value* get() const noexcept { return value_; }
    [[nodiscard]] const Value& operator*() const noexcept { return *value_; }
    [[nodiscard]] const Value* operator->() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept { return value_ != nullptr; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] std::uint64_t published_at_ns() const noexcept {
      return published_at_ns_;
    }
    [[nodiscard]] bool stale(const std::uint64_t now_ns,
                             const std::uint64_t threshold_ns) const noexcept {
      return !valid() || now_ns < acquired_at_ns_ ||
             now_ns - acquired_at_ns_ > threshold_ns;
    }

    void release() noexcept {
      if (store_ != nullptr) {
        store_->release_reader(slot_index_);
      }
      store_ = nullptr;
      value_ = nullptr;
      generation_ = 0U;
      published_at_ns_ = 0U;
      acquired_at_ns_ = 0U;
      slot_index_ = 0U;
    }

  private:
    friend class ImmutableSnapshotStore;

    // These fields are copied from one already-validated slot in one call.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void adopt(ImmutableSnapshotStore& store, const std::size_t slot_index,
               const std::uint64_t generation, const std::uint64_t published_at_ns,
               const std::uint64_t acquired_at_ns) noexcept {
      release();
      store_ = &store;
      slot_index_ = slot_index;
      value_ = &store.slots_[slot_index].value;
      generation_ = generation;
      published_at_ns_ = published_at_ns;
      acquired_at_ns_ = acquired_at_ns;
    }

    void move_from(ReadHandle& other) noexcept {
      store_ = std::exchange(other.store_, nullptr);
      value_ = std::exchange(other.value_, nullptr);
      generation_ = std::exchange(other.generation_, 0U);
      published_at_ns_ = std::exchange(other.published_at_ns_, 0U);
      acquired_at_ns_ = std::exchange(other.acquired_at_ns_, 0U);
      slot_index_ = std::exchange(other.slot_index_, 0U);
    }

    ImmutableSnapshotStore* store_{};
    const Value* value_{};
    std::uint64_t generation_{};
    std::uint64_t published_at_ns_{};
    std::uint64_t acquired_at_ns_{};
    std::size_t slot_index_{};
  };

  ImmutableSnapshotStore() noexcept = default;

  [[nodiscard]] bool initialize(SnapshotChecksumFunction checksum) noexcept {
    if (checksum == nullptr || initialized_.load(std::memory_order_relaxed)) {
      return false;
    }
    checksum_ = checksum;
    active_slot_.store(kNoActiveSlot, std::memory_order_relaxed);
    published_generation_.store(0U, std::memory_order_relaxed);
    for (auto& slot : slots_) {
      slot.sequence.store(0U, std::memory_order_relaxed);
      slot.readers.store(0U, std::memory_order_relaxed);
      slot.oldest_reader_time_ns.store(0U, std::memory_order_relaxed);
      slot.generation = 0U;
      slot.published_at_ns = 0U;
      slot.checksum = 0U;
    }
    initialized_.store(true, std::memory_order_release);
    return true;
  }

  [[nodiscard]] SnapshotPublishStatus
  publish(const Value& value, const std::uint64_t published_at_ns) noexcept {
    if (!initialized_.load(std::memory_order_acquire)) {
      return SnapshotPublishStatus::uninitialized;
    }
    const auto active = active_slot_.load(std::memory_order_acquire);
    const auto start = active == kNoActiveSlot ? 0U : (active + 1U) % SlotCount;
    for (std::size_t offset = 0U; offset < SlotCount; ++offset) {
      const auto candidate = (start + offset) % SlotCount;
      if (candidate == active) {
        continue;
      }
      auto& slot = slots_[candidate];
      if (slot.readers.load(std::memory_order_acquire) != 0U) {
        continue;
      }
      auto sequence = slot.sequence.load(std::memory_order_relaxed);
      if ((sequence & 1U) != 0U ||
          !slot.sequence.compare_exchange_strong(sequence, sequence + 1U,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
        continue;
      }
      if (slot.readers.load(std::memory_order_acquire) != 0U) {
        slot.sequence.store(sequence + 2U, std::memory_order_release);
        continue;
      }
      slot.value = value;
      slot.checksum = checksum_(&slot.value);
      slot.generation =
          published_generation_.fetch_add(1U, std::memory_order_relaxed) + 1U;
      slot.published_at_ns = published_at_ns;
      slot.sequence.store(sequence + 2U, std::memory_order_release);
      // Release publishes the fully constructed immutable slot and its stable
      // even sequence to readers acquiring active_slot_.
      active_slot_.store(candidate, std::memory_order_release);
      publish_count_.fetch_add(1U, std::memory_order_relaxed);
      return SnapshotPublishStatus::published;
    }
    no_free_slot_count_.fetch_add(1U, std::memory_order_relaxed);
    return SnapshotPublishStatus::no_free_slot;
  }

  [[nodiscard]] SnapshotReadStatus acquire(const std::uint64_t acquired_at_ns,
                                           ReadHandle& output) noexcept {
    if (!initialized_.load(std::memory_order_acquire)) {
      return SnapshotReadStatus::uninitialized;
    }
    for (std::size_t retry = 0U; retry < kMaximumReadRetries; ++retry) {
      const auto active = active_slot_.load(std::memory_order_acquire);
      if (active == kNoActiveSlot) {
        return SnapshotReadStatus::empty;
      }
      auto& slot = slots_[active];
      const auto sequence = slot.sequence.load(std::memory_order_acquire);
      if ((sequence & 1U) != 0U) {
        continue;
      }
      const auto previous_readers =
          slot.readers.fetch_add(1U, std::memory_order_acq_rel);
      if (previous_readers == std::numeric_limits<std::uint32_t>::max()) {
        slot.readers.fetch_sub(1U, std::memory_order_release);
        return SnapshotReadStatus::reader_overflow;
      }
      if (previous_readers == 0U) {
        slot.oldest_reader_time_ns.store(acquired_at_ns, std::memory_order_relaxed);
      }
      if (slot.sequence.load(std::memory_order_acquire) != sequence ||
          active_slot_.load(std::memory_order_acquire) != active) {
        release_reader(active);
        continue;
      }
      if (checksum_(&slot.value) != slot.checksum) {
        corrupt_read_count_.fetch_add(1U, std::memory_order_relaxed);
        release_reader(active);
        return SnapshotReadStatus::corrupt;
      }
      output.adopt(*this, active, slot.generation, slot.published_at_ns,
                   acquired_at_ns);
      read_count_.fetch_add(1U, std::memory_order_relaxed);
      return SnapshotReadStatus::snapshot;
    }
    return SnapshotReadStatus::busy;
  }

  [[nodiscard]] SnapshotStoreMetrics
  metrics(const std::uint64_t now_ns,
          const std::uint64_t stale_reader_threshold_ns) const noexcept {
    std::uint64_t active_readers{};
    std::uint32_t stale_slots{};
    for (const auto& slot : slots_) {
      const auto readers = slot.readers.load(std::memory_order_acquire);
      if (readers == 0U) {
        continue;
      }
      active_readers += readers;
      const auto oldest = slot.oldest_reader_time_ns.load(std::memory_order_relaxed);
      if (now_ns < oldest || now_ns - oldest > stale_reader_threshold_ns) {
        ++stale_slots;
      }
    }
    return {.published_generation =
                published_generation_.load(std::memory_order_relaxed),
            .publish_count = publish_count_.load(std::memory_order_relaxed),
            .read_count = read_count_.load(std::memory_order_relaxed),
            .no_free_slot_count = no_free_slot_count_.load(std::memory_order_relaxed),
            .corrupt_read_count = corrupt_read_count_.load(std::memory_order_relaxed),
            .active_readers = active_readers,
            .stale_reader_slots = stale_slots};
  }

private:
  struct alignas(kCacheLineBytes) Slot {
    std::atomic<std::uint64_t> sequence{0U};
    std::atomic<std::uint32_t> readers{0U};
    std::atomic<std::uint64_t> oldest_reader_time_ns{0U};
    std::uint64_t generation{};
    std::uint64_t published_at_ns{};
    std::uint64_t checksum{};
    Value value{};
  };

  void release_reader(const std::size_t slot_index) noexcept {
    auto& slot = slots_[slot_index];
    if (slot.readers.fetch_sub(1U, std::memory_order_release) == 1U) {
      slot.oldest_reader_time_ns.store(0U, std::memory_order_relaxed);
    }
  }

  static constexpr std::size_t kMaximumReadRetries = 8U;
  static constexpr std::size_t kNoActiveSlot = SlotCount;
  alignas(kCacheLineBytes) std::array<Slot, SlotCount> slots_{};
  alignas(kCacheLineBytes) std::atomic<std::size_t> active_slot_{kNoActiveSlot};
  std::atomic<std::uint64_t> published_generation_{0U};
  std::atomic<std::uint64_t> publish_count_{0U};
  std::atomic<std::uint64_t> read_count_{0U};
  std::atomic<std::uint64_t> no_free_slot_count_{0U};
  std::atomic<std::uint64_t> corrupt_read_count_{0U};
  std::atomic<bool> initialized_{false};
  SnapshotChecksumFunction checksum_{};
};

} // namespace aegis::event_bus

#endif // AEGIS_EVENT_BUS_SNAPSHOT_STORE_HPP
