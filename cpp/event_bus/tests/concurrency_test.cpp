#include "aegis/event_bus/mpsc_queue.hpp"
#include "aegis/event_bus/snapshot_store.hpp"
#include "aegis/event_bus/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace bus = aegis::event_bus;

namespace {

inline constexpr std::uint64_t kSpscMessages = 500'000U;
inline constexpr std::uint64_t kMessagesPerProducer = 75'000U;
inline constexpr std::size_t kProducerCount = 4U;
inline constexpr std::uint64_t kSnapshotPublications = 100'000U;

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct ConcurrentMessage {
  std::uint32_t producer{};
  std::uint32_t sequence{};
};

struct ConcurrentSnapshot {
  std::uint64_t sequence{};
  std::uint64_t complement{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] std::uint64_t snapshot_checksum(const void* value) noexcept {
  const auto& snapshot = *static_cast<const ConcurrentSnapshot*>(value);
  return snapshot.sequence ^ std::rotl(snapshot.complement, 29U) ^
         0x5A5A'A5A5'4433'2211ULL;
}

// Sustained tests use fixed counts and no timing assertions. Seed/order evidence
// is the monotonic per-producer sequence itself.
// GoogleTest assertion macros inflate the reported cognitive complexity.
// NOLINTBEGIN(readability-function-cognitive-complexity)
TEST(EventBusConcurrencyTest, SustainedSpscRacePreservesEverySequence) {
  auto queue = std::make_unique<bus::SpscRing<std::uint64_t, 1'024U>>();
  std::atomic<bool> failed{false};
  std::thread producer([&] {
    for (std::uint64_t value = 1U; value <= kSpscMessages; ++value) {
      while (queue->enqueue(value) == bus::EnqueueStatus::full) {
        std::this_thread::yield();
      }
    }
  });
  std::thread consumer([&] {
    for (std::uint64_t expected = 1U; expected <= kSpscMessages; ++expected) {
      std::uint64_t value{};
      while (queue->dequeue(value) == bus::DequeueStatus::empty) {
        std::this_thread::yield();
      }
      if (value != expected) {
        failed.store(true, std::memory_order_relaxed);
      }
    }
  });
  producer.join();
  consumer.join();
  EXPECT_FALSE(failed.load(std::memory_order_relaxed));
  EXPECT_EQ(queue->metrics().accepted_count, kSpscMessages);
  EXPECT_EQ(queue->metrics().consumed_count, kSpscMessages);
}

TEST(EventBusConcurrencyTest, SustainedMpscRacePreservesEachProducerSequence) {
  auto queue = std::make_unique<bus::MpscQueue<ConcurrentMessage, 1'024U, 256U>>();
  std::array<std::thread, kProducerCount> producers;
  std::atomic<bool> failed{false};
  for (std::size_t producer_index = 0U; producer_index < producers.size();
       ++producer_index) {
    producers[producer_index] = std::thread([&, producer_index] {
      for (std::uint32_t sequence = 1U; sequence <= kMessagesPerProducer; ++sequence) {
        const ConcurrentMessage message{.producer =
                                            static_cast<std::uint32_t>(producer_index),
                                        .sequence = sequence};
        for (;;) {
          const auto status = queue->enqueue(message);
          if (status == bus::EnqueueStatus::accepted) {
            break;
          }
          if (status != bus::EnqueueStatus::full &&
              status != bus::EnqueueStatus::contention) {
            failed.store(true, std::memory_order_relaxed);
            break;
          }
          std::this_thread::yield();
        }
      }
    });
  }
  std::array<std::uint32_t, kProducerCount> last_sequence{};
  const auto total_messages = kMessagesPerProducer * kProducerCount;
  for (std::uint64_t consumed = 0U; consumed < total_messages;) {
    ConcurrentMessage message{};
    const auto status = queue->dequeue(message);
    if (status != bus::DequeueStatus::item) {
      std::this_thread::yield();
      continue;
    }
    if (message.producer >= kProducerCount ||
        message.sequence != last_sequence[message.producer] + 1U) {
      failed.store(true, std::memory_order_relaxed);
    } else {
      last_sequence[message.producer] = message.sequence;
    }
    ++consumed;
  }
  for (auto& producer : producers) {
    producer.join();
  }
  EXPECT_FALSE(failed.load(std::memory_order_relaxed));
  for (const auto sequence : last_sequence) {
    EXPECT_EQ(sequence, kMessagesPerProducer);
  }
  EXPECT_EQ(queue->metrics().accepted_count, total_messages);
  EXPECT_EQ(queue->metrics().consumed_count, total_messages);
  EXPECT_LE(queue->metrics().maximum_consumer_lag_events, queue->capacity());
}

TEST(EventBusConcurrencyTest, SnapshotReadersNeverObserveTornState) {
  using Store = bus::ImmutableSnapshotStore<ConcurrentSnapshot, 4U>;
  auto store = std::make_unique<Store>();
  ASSERT_TRUE(store->initialize(snapshot_checksum));
  ASSERT_EQ(store->publish({.sequence = 0U, .complement = ~std::uint64_t{0U}}, 1U),
            bus::SnapshotPublishStatus::published);
  std::atomic<bool> writer_done{false};
  std::atomic<bool> failed{false};
  std::thread writer([&] {
    for (std::uint64_t sequence = 1U; sequence <= kSnapshotPublications; ++sequence) {
      while (store->publish({.sequence = sequence, .complement = ~sequence},
                            sequence + 1U) ==
             bus::SnapshotPublishStatus::no_free_slot) {
        std::this_thread::yield();
      }
    }
    writer_done.store(true, std::memory_order_release);
  });
  std::array<std::thread, 4U> readers;
  for (auto& reader : readers) {
    reader = std::thread([&] {
      while (!writer_done.load(std::memory_order_acquire)) {
        Store::ReadHandle handle;
        const auto status = store->acquire(1U, handle);
        if (status == bus::SnapshotReadStatus::snapshot &&
            handle->complement != ~handle->sequence) {
          failed.store(true, std::memory_order_relaxed);
        }
      }
    });
  }
  writer.join();
  for (auto& reader : readers) {
    reader.join();
  }
  EXPECT_FALSE(failed.load(std::memory_order_relaxed));
  EXPECT_EQ(store->metrics(kSnapshotPublications + 2U, 1'000U).published_generation,
            kSnapshotPublications + 1U);
}
// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
