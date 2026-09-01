#include "aegis/features/feature_engine.hpp"
#include "aegis/features/publisher.hpp"

#include "allocation_probe.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace {

namespace features = aegis::features;
namespace book = aegis::order_book;
namespace synthetic = aegis::market_data::synthetic;

inline constexpr std::size_t kSampleCapacity = 4'096U;

[[nodiscard]] features::FeatureEngineConfig config() {
  features::FeatureEngineConfig result{
      .instrument_id = {1U, 1U},
      .session_id = {2U, 1U},
      .feature_definition_version = {3U, 1U},
      .venue_count = 1U,
      .depth_levels = 5U,
      .window_capacity = 1'024U,
      .minimum_book_events = 1U,
      .minimum_trade_events = 0U,
      .rolling_window_ns = 1'000'000'000U,
      .stale_after_ns = 500'000'000U,
      .maximum_quantity_units = 1'000'000U,
      .maximum_absolute_price_ticks = 1'000'000,
      .session_open_exchange_time_ns = 1'000'000'000'000LL,
      .session_close_exchange_time_ns = 2'000'000'000'000LL};
  result.venues[0U] = {.venue_id = {4U, 1U}, .venue_number = 1U};
  return result;
}

[[nodiscard]] features::BookFeatureEvent event(const std::uint64_t ordinal) {
  book::DepthSnapshot depth{};
  for (std::size_t level = 0U; level < 5U; ++level) {
    depth.bids[level] = {.price_ticks = 10'000 - static_cast<std::int64_t>(level),
                         .quantity_units = 100U + (ordinal % 100U) + level,
                         .order_count = 1U};
    depth.asks[level] = {.price_ticks = 10'002 + static_cast<std::int64_t>(level),
                         .quantity_units = 110U + (ordinal % 100U) + level,
                         .order_count = 1U};
  }
  depth.bid_count = 5U;
  depth.ask_count = 5U;
  depth.version = ordinal;
  depth.validity = book::BookValidity::valid;
  return {.global_event_id = {5U, ordinal},
          .session_id = {2U, 1U},
          .instrument_id = {1U, 1U},
          .venue_id = {4U, 1U},
          .venue_number = 1U,
          .global_ordinal = ordinal,
          .exchange_event_time_ns =
              1'000'000'000'000LL + static_cast<std::int64_t>(ordinal * 1'000U),
          .process_monotonic_time_ns = ordinal * 1'000U,
          .operation = book::BookOperation::observe_quote,
          .depth = depth,
          .data_quality = synthetic::DataQuality::valid,
          .top_changed = true};
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

void benchmark_event_to_feature(benchmark::State& state) {
  auto engine = std::make_unique<features::FeatureEngine>(config());
  std::uint64_t ordinal{};
  std::array<std::uint64_t, kSampleCapacity> samples{};
  std::size_t sample_count{};
  const auto allocations_before = allocation_probe::count();
  for (auto iteration : state) {
    static_cast<void>(iteration);
    const auto input = event(++ordinal);
    const auto before = std::chrono::steady_clock::now();
    const auto result = engine->on_book(input);
    const auto after = std::chrono::steady_clock::now();
    if (!result.accepted()) {
      state.SkipWithError("feature event rejected");
      break;
    }
    samples[sample_count % samples.size()] = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count());
    ++sample_count;
  }
  state.counters["steady_state_allocations"] =
      static_cast<double>(allocation_probe::count() - allocations_before);
  state.counters["bytes_per_symbol"] =
      static_cast<double>(engine->memory_bytes_per_symbol());
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
  record_percentiles(state, samples, sample_count);
}

struct SnapshotSink {
  features::FeatureSnapshot latest;
};

[[nodiscard]] bool
publish_snapshot(void* context, const features::FeatureSnapshot& snapshot) noexcept {
  static_cast<SnapshotSink*>(context)->latest = snapshot;
  return true;
}

void benchmark_snapshot_publication(benchmark::State& state) {
  auto engine = std::make_unique<features::FeatureEngine>(config());
  if (!engine->on_book(event(1U)).accepted()) {
    state.SkipWithError("feature engine setup failed");
    return;
  }
  SnapshotSink sink{};
  features::FeatureSnapshotPublisher publisher{&sink, publish_snapshot};
  std::array<std::uint64_t, kSampleCapacity> samples{};
  std::size_t sample_count{};
  const auto allocations_before = allocation_probe::count();
  for (auto iteration : state) {
    static_cast<void>(iteration);
    const auto before = std::chrono::steady_clock::now();
    const auto error = publisher.publish(*engine, 1'000U);
    const auto after = std::chrono::steady_clock::now();
    if (error != features::FeatureError::none) {
      state.SkipWithError("snapshot publication failed");
      break;
    }
    samples[sample_count % samples.size()] = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count());
    ++sample_count;
    benchmark::DoNotOptimize(sink.latest.stable_hash);
  }
  state.counters["steady_state_allocations"] =
      static_cast<double>(allocation_probe::count() - allocations_before);
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
  record_percentiles(state, samples, sample_count);
}

BENCHMARK(benchmark_event_to_feature)->MinTime(0.02);
BENCHMARK(benchmark_snapshot_publication)->MinTime(0.02);

} // namespace
