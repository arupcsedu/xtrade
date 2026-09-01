#include "aegis/event_bus/mpsc_queue.hpp"
#include "aegis/event_bus/snapshot_store.hpp"
#include "aegis/event_bus/spsc_ring.hpp"
#include "aegis/event_bus/symbol_state.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <memory>

namespace bus = aegis::event_bus;

namespace {

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct SymbolValue {
  std::uint64_t sequence{};
  std::uint64_t complement{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] std::uint64_t symbol_checksum(const void* value) noexcept {
  const auto& state = *static_cast<const SymbolValue*>(value);
  return state.sequence ^ std::rotl(state.complement, 17U) ^ 0xA5A5'5A5A'1122'3344ULL;
}

// GoogleTest assertion macros inflate the reported cognitive complexity.
// NOLINTBEGIN(readability-function-cognitive-complexity)
TEST(SpscRingTest, ReportsEmptyFullLagAndWraparound) {
  bus::SpscRing<std::uint64_t, 4U> queue;
  std::uint64_t output{};
  EXPECT_EQ(queue.dequeue(output), bus::DequeueStatus::empty);
  for (std::uint64_t value = 1U; value <= 4U; ++value) {
    EXPECT_EQ(queue.enqueue(value), bus::EnqueueStatus::accepted);
  }
  EXPECT_EQ(queue.enqueue(5U), bus::EnqueueStatus::full);
  EXPECT_EQ(queue.metrics().consumer_lag_events, 4U);
  for (std::uint64_t expected = 1U; expected <= 2U; ++expected) {
    ASSERT_EQ(queue.dequeue(output), bus::DequeueStatus::item);
    EXPECT_EQ(output, expected);
  }
  EXPECT_EQ(queue.enqueue(5U), bus::EnqueueStatus::accepted);
  EXPECT_EQ(queue.enqueue(6U), bus::EnqueueStatus::accepted);
  for (std::uint64_t expected = 3U; expected <= 6U; ++expected) {
    ASSERT_EQ(queue.dequeue(output), bus::DequeueStatus::item);
    EXPECT_EQ(output, expected);
  }
  const auto metrics = queue.metrics();
  EXPECT_EQ(metrics.producer_sequence, 6U);
  EXPECT_EQ(metrics.consumer_sequence, 6U);
  EXPECT_EQ(metrics.maximum_consumer_lag_events, 4U);
  EXPECT_EQ(metrics.full_rejection_count, 1U);
}

TEST(SpscRingTest, FailClosedOverloadCannotBeSilentlyDrained) {
  bus::SpscRing<std::uint64_t, 2U> queue;
  EXPECT_EQ(queue.enqueue(1U), bus::EnqueueStatus::accepted);
  EXPECT_EQ(queue.enqueue(2U), bus::EnqueueStatus::accepted);
  EXPECT_EQ(queue.enqueue(3U, bus::OverloadPolicy::fail_closed),
            bus::EnqueueStatus::invalidated);
  std::uint64_t output{};
  EXPECT_EQ(queue.dequeue(output), bus::DequeueStatus::invalidated);
  EXPECT_TRUE(queue.metrics().invalidated);
  EXPECT_EQ(queue.metrics().invalidation_count, 1U);
  queue.reset_quiescent();
  EXPECT_EQ(queue.enqueue(4U), bus::EnqueueStatus::accepted);
}

TEST(MpscQueueTest, ReportsEmptyFullAndWraparound) {
  bus::MpscQueue<std::uint64_t, 4U> queue;
  std::uint64_t output{};
  EXPECT_EQ(queue.dequeue(output), bus::DequeueStatus::empty);
  for (std::uint64_t value = 1U; value <= 4U; ++value) {
    EXPECT_EQ(queue.enqueue(value), bus::EnqueueStatus::accepted);
  }
  EXPECT_EQ(queue.enqueue(5U), bus::EnqueueStatus::full);
  for (std::uint64_t expected = 1U; expected <= 4U; ++expected) {
    ASSERT_EQ(queue.dequeue(output), bus::DequeueStatus::item);
    EXPECT_EQ(output, expected);
  }
  for (std::uint64_t value = 5U; value <= 8U; ++value) {
    EXPECT_EQ(queue.enqueue(value), bus::EnqueueStatus::accepted);
  }
  for (std::uint64_t expected = 5U; expected <= 8U; ++expected) {
    ASSERT_EQ(queue.dequeue(output), bus::DequeueStatus::item);
    EXPECT_EQ(output, expected);
  }
  EXPECT_EQ(queue.metrics().maximum_consumer_lag_events, 4U);
}

TEST(MpscQueueTest, FailClosedOverloadInvalidatesBothSides) {
  bus::MpscQueue<std::uint64_t, 2U> queue;
  EXPECT_EQ(queue.enqueue(1U), bus::EnqueueStatus::accepted);
  EXPECT_EQ(queue.enqueue(2U), bus::EnqueueStatus::accepted);
  EXPECT_EQ(queue.enqueue(3U, bus::OverloadPolicy::fail_closed),
            bus::EnqueueStatus::invalidated);
  std::uint64_t output{};
  EXPECT_EQ(queue.dequeue(output), bus::DequeueStatus::invalidated);
  EXPECT_TRUE(queue.metrics().invalidated);
  EXPECT_EQ(queue.metrics().full_rejection_count, 1U);
}

TEST(ImmutableSnapshotStoreTest, PinsImmutableVersionsAndDetectsStaleReaders) {
  using Store = bus::ImmutableSnapshotStore<SymbolValue, 3U>;
  auto store = std::make_unique<Store>();
  ASSERT_TRUE(store->initialize(symbol_checksum));
  EXPECT_EQ(store->publish({1U, ~std::uint64_t{1U}}, 100U),
            bus::SnapshotPublishStatus::published);
  Store::ReadHandle first;
  ASSERT_EQ(store->acquire(110U, first), bus::SnapshotReadStatus::snapshot);
  EXPECT_EQ(first->sequence, 1U);
  EXPECT_EQ(store->publish({2U, ~std::uint64_t{2U}}, 200U),
            bus::SnapshotPublishStatus::published);
  Store::ReadHandle second;
  ASSERT_EQ(store->acquire(210U, second), bus::SnapshotReadStatus::snapshot);
  EXPECT_EQ(second->sequence, 2U);
  EXPECT_EQ(first->sequence, 1U);
  EXPECT_TRUE(first.stale(1'000U, 100U));
  EXPECT_EQ(store->metrics(1'000U, 100U).stale_reader_slots, 2U);

  EXPECT_EQ(store->publish({3U, ~std::uint64_t{3U}}, 300U),
            bus::SnapshotPublishStatus::published);
  EXPECT_EQ(store->publish({4U, ~std::uint64_t{4U}}, 400U),
            bus::SnapshotPublishStatus::no_free_slot);
  first.release();
  EXPECT_EQ(store->publish({4U, ~std::uint64_t{4U}}, 400U),
            bus::SnapshotPublishStatus::published);
}

TEST(RcuSymbolStateTest, UsesBoundedRegistrationAndIndependentSnapshots) {
  bus::RcuSymbolState<SymbolValue, 2U> states;
  constexpr aegis::common::InstrumentId first_id{10U, 1U};
  constexpr aegis::common::InstrumentId second_id{10U, 2U};
  constexpr aegis::common::InstrumentId third_id{10U, 3U};
  EXPECT_EQ(states.register_symbol(first_id, symbol_checksum),
            bus::SymbolStateStatus::registered);
  EXPECT_EQ(states.register_symbol(first_id, symbol_checksum),
            bus::SymbolStateStatus::duplicate_symbol);
  EXPECT_EQ(states.register_symbol(second_id, symbol_checksum),
            bus::SymbolStateStatus::registered);
  EXPECT_EQ(states.register_symbol(third_id, symbol_checksum),
            bus::SymbolStateStatus::capacity_exhausted);
  ASSERT_EQ(states.publish(first_id, {7U, ~std::uint64_t{7U}}, 10U),
            bus::SnapshotPublishStatus::published);
  decltype(states)::ReadHandle handle;
  ASSERT_EQ(states.acquire(first_id, 11U, handle), bus::SnapshotReadStatus::snapshot);
  EXPECT_EQ(handle->sequence, 7U);
  EXPECT_EQ(states.acquire(third_id, 11U, handle),
            bus::SnapshotReadStatus::uninitialized);
}
// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
