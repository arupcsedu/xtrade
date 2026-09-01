#include "aegis/order_book/order_book.hpp"

#include "allocation_probe.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

namespace {

namespace book = aegis::order_book;
namespace synthetic = aegis::market_data::synthetic;

inline constexpr std::size_t kSampleCapacity = 4'096U;

[[nodiscard]] book::BookConfig config(const std::uint32_t order_capacity) {
  return {
      .identity = {.venue_id = aegis::common::VenueId{1U, 1U},
                   .instrument_id = aegis::common::InstrumentId{2U, 1U},
                   .venue_number = 1U,
                   .instrument_number = 1U},
      .session_id = aegis::common::SessionId{3U, 1U},
      .mode = book::BookMode::order_by_order,
      .order_capacity = order_capacity,
      .level_capacity = 128U,
      .published_depth = 64U,
      .maximum_sequence = std::numeric_limits<std::uint64_t>::max(),
      .permit_crossed_transition = false,
      .require_contiguous_sequence = true,
      .start_recovering = false,
  };
}

[[nodiscard]] constexpr aegis::common::OrderId order_id(const std::uint64_t id) {
  return {0x4245'4e43'4800'0001ULL, id};
}

[[nodiscard]] book::BookUpdate add_update(const std::uint64_t sequence,
                                          const std::uint64_t id,
                                          const std::int64_t price = 100) {
  return {.operation = book::BookOperation::add,
          .side = synthetic::Side::bid,
          .order_id = order_id(id),
          .price_ticks = price,
          .quantity_units = 100U,
          .sequence = sequence,
          .process_monotonic_time_ns = sequence};
}

void prime(book::OrderBook& engine, const std::uint32_t size, std::uint64_t& sequence) {
  for (std::uint32_t index = 0U; index < size; ++index) {
    const auto price = 100 - static_cast<std::int64_t>(index % 64U);
    const auto result = engine.apply(add_update(++sequence, index + 1U, price));
    if (!result.accepted()) {
      std::abort();
    }
  }
}

void record_percentiles(benchmark::State& state,
                        std::array<std::uint64_t, kSampleCapacity>& samples,
                        std::size_t sample_count) {
  sample_count = std::min(sample_count, samples.size());
  std::sort(samples.begin(),
            samples.begin() + static_cast<std::ptrdiff_t>(sample_count));
  const auto value = [&](const std::size_t thousandths) {
    if (sample_count == 0U) {
      return 0.0;
    }
    return static_cast<double>(samples[((sample_count - 1U) * thousandths) / 1'000U]);
  };
  state.counters["p50_ns"] = value(500U);
  state.counters["p95_ns"] = value(950U);
  state.counters["p99_ns"] = value(990U);
  state.counters["p99_9_ns"] = value(999U);
}

enum class MeasuredOperation : std::uint8_t {
  add = 1,
  cancel = 2,
  execute = 3,
  top_update = 4,
};

void benchmark_mutation(benchmark::State& state) {
  const auto operation = static_cast<MeasuredOperation>(state.range(0));
  const auto initial_size = static_cast<std::uint32_t>(state.range(1));
  auto engine = std::make_unique<book::OrderBook>(config(initial_size + 1U));
  std::uint64_t sequence = 0U;
  prime(*engine, initial_size, sequence);
  const auto transient_id = static_cast<std::uint64_t>(initial_size) + 1U;
  std::array<std::uint64_t, kSampleCapacity> samples{};
  std::size_t sample_count = 0U;
  const auto allocations_before = allocation_probe::count();

  for (auto iteration : state) {
    static_cast<void>(iteration);
    if (operation != MeasuredOperation::add) {
      state.PauseTiming();
      const auto setup = engine->apply(add_update(++sequence, transient_id));
      state.ResumeTiming();
      if (!setup.accepted()) {
        state.SkipWithError("mutation setup failed");
        break;
      }
    }
    book::BookUpdate measured{};
    if (operation == MeasuredOperation::add) {
      measured = add_update(++sequence, transient_id);
    } else {
      measured = {.operation = operation == MeasuredOperation::execute
                                   ? book::BookOperation::execute
                                   : book::BookOperation::cancel,
                  .order_id = order_id(transient_id),
                  .quantity_units = operation == MeasuredOperation::execute ? 100U : 0U,
                  .sequence = ++sequence,
                  .process_monotonic_time_ns = sequence};
    }
    if (operation == MeasuredOperation::top_update) {
      measured.operation = book::BookOperation::replace;
      measured.side = synthetic::Side::bid;
      measured.price_ticks = 101;
      measured.quantity_units = 100U;
    }
    const auto before = std::chrono::steady_clock::now();
    const auto result = engine->apply(measured);
    const auto after = std::chrono::steady_clock::now();
    if (!result.accepted()) {
      state.SkipWithError("measured mutation failed");
      break;
    }
    samples[sample_count % samples.size()] = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count());
    ++sample_count;
    if (operation == MeasuredOperation::add ||
        operation == MeasuredOperation::top_update) {
      state.PauseTiming();
      const auto cleanup = engine->apply({.operation = book::BookOperation::cancel,
                                          .order_id = order_id(transient_id),
                                          .sequence = ++sequence,
                                          .process_monotonic_time_ns = sequence});
      state.ResumeTiming();
      if (!cleanup.accepted()) {
        state.SkipWithError("mutation cleanup failed");
        break;
      }
    }
  }
  const auto allocations_after = allocation_probe::count();
  state.counters["steady_state_allocations"] =
      static_cast<double>(allocations_after - allocations_before);
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
  record_percentiles(state, samples, sample_count);
}

void benchmark_depth_query(benchmark::State& state) {
  const auto initial_size = static_cast<std::uint32_t>(state.range(0));
  auto engine = std::make_unique<book::OrderBook>(config(initial_size));
  std::uint64_t sequence = 0U;
  prime(*engine, initial_size, sequence);
  book::DepthSnapshot depth{};
  const auto allocations_before = allocation_probe::count();
  for (auto iteration : state) {
    static_cast<void>(iteration);
    auto result = engine->depth(depth, 32U);
    benchmark::DoNotOptimize(result);
    benchmark::DoNotOptimize(depth.version);
  }
  const auto allocations_after = allocation_probe::count();
  state.counters["steady_state_allocations"] =
      static_cast<double>(allocations_after - allocations_before);
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}

void benchmark_throughput(benchmark::State& state) {
  const auto initial_size = static_cast<std::uint32_t>(state.range(0));
  auto engine = std::make_unique<book::OrderBook>(config(initial_size));
  std::uint64_t sequence = 0U;
  prime(*engine, initial_size, sequence);
  std::uint64_t cursor = 0U;
  const auto allocations_before = allocation_probe::count();
  for (auto iteration : state) {
    static_cast<void>(iteration);
    const auto id = 1U + (cursor++ % initial_size);
    const auto* current = engine->find_order(order_id(id));
    if (current == nullptr) {
      state.SkipWithError("throughput order lookup failed");
      break;
    }
    const auto result = engine->apply({.operation = book::BookOperation::replace,
                                       .side = current->side,
                                       .order_id = current->order_id,
                                       .price_ticks = current->price_ticks,
                                       .quantity_units = 100U + (cursor & 1U),
                                       .sequence = ++sequence,
                                       .process_monotonic_time_ns = sequence});
    if (!result.accepted()) {
      state.SkipWithError("throughput replace failed");
      break;
    }
  }
  const auto allocations_after = allocation_probe::count();
  state.counters["steady_state_allocations"] =
      static_cast<double>(allocations_after - allocations_before);
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}

BENCHMARK(benchmark_mutation)
    ->Args({static_cast<std::int64_t>(MeasuredOperation::add), 256})
    ->Args({static_cast<std::int64_t>(MeasuredOperation::cancel), 256})
    ->Args({static_cast<std::int64_t>(MeasuredOperation::execute), 256})
    ->Args({static_cast<std::int64_t>(MeasuredOperation::top_update), 256});
BENCHMARK(benchmark_depth_query)->Arg(32)->Arg(256)->Arg(1'024)->Arg(2'048);
BENCHMARK(benchmark_throughput)->Arg(32)->Arg(256)->Arg(1'024)->Arg(2'048);

} // namespace
