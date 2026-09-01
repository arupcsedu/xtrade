#include "aegis/event_bus/mpsc_queue.hpp"
#include "aegis/event_bus/snapshot_store.hpp"
#include "aegis/event_bus/spsc_ring.hpp"

#include "allocation_probe.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace {

namespace bus = aegis::event_bus;

inline constexpr std::size_t kBatchSize = 256U;
inline constexpr std::size_t kContentionProducerCount = 4U;
inline constexpr std::uint32_t kContentionMessagesPerProducer = 50'000U;
inline constexpr std::size_t kSamplesPerProducer = 1'000U;
inline constexpr std::uint32_t kSampleStride =
    kContentionMessagesPerProducer / kSamplesPerProducer;

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct SnapshotValue {
  std::uint64_t sequence{};
  std::uint64_t complement{};
};

struct ContentionMessage {
  std::uint32_t producer{};
  std::uint32_t sequence{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] std::uint64_t snapshot_checksum(const void* value) noexcept {
  const auto& snapshot = *static_cast<const SnapshotValue*>(value);
  return snapshot.sequence ^ std::rotl(snapshot.complement, 13U) ^
         0xC3A5'C85C'97CB'3127ULL;
}

void benchmark_spsc_enqueue(benchmark::State& state) {
  auto queue = std::make_unique<bus::SpscRing<std::uint64_t, 512U>>();
  std::uint64_t next{};
  std::uint64_t consumed{};
  const auto allocations_before = allocation_probe::count();
  for (auto iteration : state) {
    static_cast<void>(iteration);
    for (std::size_t index = 0U; index < kBatchSize; ++index) {
      if (queue->enqueue(++next) != bus::EnqueueStatus::accepted) {
        state.SkipWithError("unexpected SPSC enqueue rejection");
        return;
      }
    }
    state.PauseTiming();
    for (std::size_t index = 0U; index < kBatchSize; ++index) {
      if (queue->dequeue(consumed) != bus::DequeueStatus::item) {
        state.SkipWithError("unexpected SPSC drain failure");
        return;
      }
    }
    state.ResumeTiming();
  }
  benchmark::DoNotOptimize(consumed);
  state.counters["steady_state_allocations"] =
      static_cast<double>(allocation_probe::count() - allocations_before);
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(kBatchSize));
}

void benchmark_spsc_dequeue(benchmark::State& state) {
  auto queue = std::make_unique<bus::SpscRing<std::uint64_t, 512U>>();
  std::uint64_t next{};
  std::uint64_t consumed{};
  const auto allocations_before = allocation_probe::count();
  for (auto iteration : state) {
    static_cast<void>(iteration);
    state.PauseTiming();
    for (std::size_t index = 0U; index < kBatchSize; ++index) {
      if (queue->enqueue(++next) != bus::EnqueueStatus::accepted) {
        state.SkipWithError("unexpected SPSC prefill rejection");
        return;
      }
    }
    state.ResumeTiming();
    for (std::size_t index = 0U; index < kBatchSize; ++index) {
      if (queue->dequeue(consumed) != bus::DequeueStatus::item) {
        state.SkipWithError("unexpected SPSC dequeue failure");
        return;
      }
    }
  }
  benchmark::DoNotOptimize(consumed);
  state.counters["steady_state_allocations"] =
      static_cast<double>(allocation_probe::count() - allocations_before);
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(kBatchSize));
}

void benchmark_snapshot_read(benchmark::State& state) {
  using Store = bus::ImmutableSnapshotStore<SnapshotValue, 4U>;
  auto store = std::make_unique<Store>();
  if (!store->initialize(snapshot_checksum) ||
      store->publish({.sequence = 1U, .complement = ~std::uint64_t{1U}}, 1U) !=
          bus::SnapshotPublishStatus::published) {
    state.SkipWithError("snapshot setup failed");
    return;
  }
  const auto allocations_before = allocation_probe::count();
  for (auto iteration : state) {
    static_cast<void>(iteration);
    Store::ReadHandle handle;
    if (store->acquire(2U, handle) != bus::SnapshotReadStatus::snapshot) {
      state.SkipWithError("snapshot read failed");
      return;
    }
    auto observed_sequence = handle->sequence;
    benchmark::DoNotOptimize(observed_sequence);
  }
  state.counters["steady_state_allocations"] =
      static_cast<double>(allocation_probe::count() - allocations_before);
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}

template <std::size_t SampleCount>
void record_tail_percentiles(benchmark::State& state,
                             std::array<std::uint64_t, SampleCount>& samples,
                             const std::size_t actual_count) {
  std::sort(samples.begin(),
            samples.begin() + static_cast<std::ptrdiff_t>(actual_count));
  const auto percentile = [&](const std::size_t thousandths) {
    return static_cast<double>(samples[((actual_count - 1U) * thousandths) / 1'000U]);
  };
  state.counters["enqueue_p50_ns"] = percentile(500U);
  state.counters["enqueue_p95_ns"] = percentile(950U);
  state.counters["enqueue_p99_ns"] = percentile(990U);
  state.counters["enqueue_p99_9_ns"] = percentile(999U);
  state.counters["enqueue_max_ns"] = static_cast<double>(samples[actual_count - 1U]);
}

// The bounded orchestration is kept together so the reported clock interval
// covers one synchronized producer/consumer run.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void benchmark_mpsc_contention(benchmark::State& state) {
  using Queue = bus::MpscQueue<ContentionMessage, 4'096U, 256U>;
  for (auto iteration : state) {
    static_cast<void>(iteration);
    auto queue = std::make_unique<Queue>();
    std::array<std::thread, kContentionProducerCount> producers;
    std::array<std::array<std::uint64_t, kSamplesPerProducer>, kContentionProducerCount>
        producer_samples{};
    std::array<std::size_t, kContentionProducerCount> sample_counts{};
    std::array<std::uint32_t, kContentionProducerCount> last_sequences{};
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};

    std::thread consumer([&] {
      constexpr std::uint64_t kTotalMessages =
          static_cast<std::uint64_t>(kContentionProducerCount) *
          kContentionMessagesPerProducer;
      for (std::uint64_t consumed_count = 0U; consumed_count < kTotalMessages;) {
        ContentionMessage message{};
        if (queue->dequeue(message) != bus::DequeueStatus::item) {
          std::this_thread::yield();
          continue;
        }
        if (message.producer >= kContentionProducerCount ||
            message.sequence != last_sequences[message.producer] + 1U) {
          failed.store(true, std::memory_order_relaxed);
        } else {
          last_sequences[message.producer] = message.sequence;
        }
        ++consumed_count;
      }
    });

    for (std::size_t producer_index = 0U; producer_index < producers.size();
         ++producer_index) {
      producers[producer_index] = std::thread([&, producer_index] {
        while (!start.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        for (std::uint32_t sequence = 1U; sequence <= kContentionMessagesPerProducer;
             ++sequence) {
          const auto before = std::chrono::steady_clock::now();
          const ContentionMessage message{
              .producer = static_cast<std::uint32_t>(producer_index),
              .sequence = sequence};
          auto status = bus::EnqueueStatus::contention;
          while (status != bus::EnqueueStatus::accepted) {
            status = queue->enqueue(message);
            if (status != bus::EnqueueStatus::accepted &&
                status != bus::EnqueueStatus::full &&
                status != bus::EnqueueStatus::contention) {
              failed.store(true, std::memory_order_relaxed);
              return;
            }
            if (status != bus::EnqueueStatus::accepted) {
              std::this_thread::yield();
            }
          }
          const auto after = std::chrono::steady_clock::now();
          if (sequence % kSampleStride == 0U) {
            producer_samples[producer_index][sample_counts[producer_index]++] =
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(after - before)
                        .count());
          }
        }
      });
    }

    const auto before = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    for (auto& producer : producers) {
      producer.join();
    }
    consumer.join();
    const auto after = std::chrono::steady_clock::now();
    if (failed.load(std::memory_order_relaxed)) {
      state.SkipWithError("MPSC contention correctness failure");
      return;
    }

    std::array<std::uint64_t, kContentionProducerCount * kSamplesPerProducer> samples{};
    std::size_t sample_count{};
    for (std::size_t producer_index = 0U; producer_index < kContentionProducerCount;
         ++producer_index) {
      for (std::size_t sample_index = 0U; sample_index < sample_counts[producer_index];
           ++sample_index) {
        samples[sample_count++] = producer_samples[producer_index][sample_index];
      }
    }
    record_tail_percentiles(state, samples, sample_count);
    const auto elapsed_seconds = std::chrono::duration<double>(after - before).count();
    constexpr auto kTotalMessages = static_cast<double>(kContentionProducerCount) *
                                    static_cast<double>(kContentionMessagesPerProducer);
    state.counters["throughput_events_per_second"] = kTotalMessages / elapsed_seconds;
    state.counters["full_rejections"] =
        static_cast<double>(queue->metrics().full_rejection_count);
    state.counters["contention_rejections"] =
        static_cast<double>(queue->metrics().contention_rejection_count);
    state.counters["maximum_consumer_lag"] =
        static_cast<double>(queue->metrics().maximum_consumer_lag_events);
  }
}

BENCHMARK(benchmark_spsc_enqueue)->MinTime(0.02);
BENCHMARK(benchmark_spsc_dequeue)->MinTime(0.02);
BENCHMARK(benchmark_snapshot_read)->MinTime(0.02);
BENCHMARK(benchmark_mpsc_contention)->Iterations(1);

} // namespace
