#include "aegis/oms/service.hpp"

#include "../oms/tests/test_support.hpp"
#include "allocation_probe.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace oms = aegis::oms;
namespace support = aegis::oms::test;

namespace {

inline constexpr std::size_t kOmsBenchmarkSamples = 256U;

enum class PreparedState : std::uint8_t { ready = 1, pending_ack = 2, working = 3 };

// Benchmark fixture aggregate is intentionally public for setup replacement.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct Harness {
  std::unique_ptr<oms::OmsJournal> journal;
  std::unique_ptr<oms::DeterministicOms> service;
  std::array<aegis::common::OrderId, kOmsBenchmarkSamples> order_ids{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] Harness prepare(const PreparedState target) {
  Harness result;
  result.journal = std::make_unique<oms::OmsJournal>();
  result.service = std::make_unique<oms::DeterministicOms>(support::configuration(),
                                                           *result.journal);
  for (std::size_t index = 0U; index < result.order_ids.size(); ++index) {
    const auto ordinal = static_cast<std::uint64_t>(index + 1U);
    const auto accepted = result.service->apply(
        support::accept(support::approval(ordinal), (ordinal * 10U) + 1U));
    result.order_ids[index] = accepted.order_id;
    benchmark::DoNotOptimize(result.service->apply(support::internal(
        oms::InputKind::mark_ready, accepted.order_id, (ordinal * 10U) + 2U)));
    if (target == PreparedState::ready) {
      continue;
    }
    benchmark::DoNotOptimize(result.service->apply(support::internal(
        oms::InputKind::dispatch, accepted.order_id, (ordinal * 10U) + 3U)));
    if (target == PreparedState::pending_ack) {
      continue;
    }
    benchmark::DoNotOptimize(result.service->apply(support::venue(
        oms::InputKind::acknowledgement, accepted.order_id, (ordinal * 10U) + 4U,
        ordinal, oms::ExternalOrderId{.high = 500U, .low = ordinal})));
  }
  return result;
}

template <typename EventFactory>
void benchmark_transition(benchmark::State& state, const PreparedState prepared,
                          EventFactory event_factory) {
  auto active = prepare(prepared);
  std::size_t cursor{};
  bool started = false;
  std::uint64_t allocations{};
  for (auto iteration : state) {
    static_cast<void>(iteration);
    if (cursor == 0U && started) {
      state.PauseTiming();
      active = prepare(prepared);
      state.ResumeTiming();
    }
    started = true;
    const auto event = event_factory(active.order_ids[cursor], cursor);
    const auto before = allocation_probe::count();
    auto result = active.service->apply(event);
    allocations += allocation_probe::count() - before;
    benchmark::DoNotOptimize(result);
    cursor = (cursor + 1U) % active.order_ids.size();
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["steady_state_allocations"] =
      benchmark::Counter(static_cast<double>(allocations));
}

void benchmark_oms_dispatch(benchmark::State& state) {
  benchmark_transition(
      state, PreparedState::ready,
      [](const aegis::common::OrderId order_id, const std::size_t index) {
        return support::internal(oms::InputKind::dispatch, order_id,
                                 50'000U + static_cast<std::uint64_t>(index));
      });
}

void benchmark_oms_acknowledgement(benchmark::State& state) {
  benchmark_transition(
      state, PreparedState::pending_ack,
      [](const aegis::common::OrderId order_id, const std::size_t index) {
        const auto ordinal = static_cast<std::uint64_t>(index + 1U);
        return support::venue(oms::InputKind::acknowledgement, order_id,
                              60'000U + ordinal, ordinal,
                              oms::ExternalOrderId{.high = 600U, .low = ordinal});
      });
}

void benchmark_oms_fill(benchmark::State& state) {
  benchmark_transition(
      state, PreparedState::working,
      [](const aegis::common::OrderId order_id, const std::size_t index) {
        const auto ordinal = static_cast<std::uint64_t>(index + 1U);
        auto event =
            support::fill(order_id, 70'000U + ordinal, ordinal + 1U, ordinal, 1U);
        event.external_order_id = {.high = 500U, .low = ordinal};
        event.stable_hash = oms::stable_input_hash(event);
        return event;
      });
}

void benchmark_oms_snapshot_read(benchmark::State& state) {
  auto active = prepare(PreparedState::working);
  for (auto iteration : state) {
    static_cast<void>(iteration);
    oms::OmsSnapshotStore::ReadHandle handle;
    auto status = active.service->acquire_snapshot(support::kNow + 100U, handle);
    benchmark::DoNotOptimize(status);
    benchmark::DoNotOptimize(handle.get());
  }
  state.SetItemsProcessed(state.iterations());
}

void benchmark_oms_restart_replay(benchmark::State& state) {
  auto source = prepare(PreparedState::working);
  for (auto iteration : state) {
    static_cast<void>(iteration);
    state.PauseTiming();
    auto target_journal = std::make_unique<oms::OmsJournal>();
    auto target = std::make_unique<oms::DeterministicOms>(support::configuration(),
                                                          *target_journal);
    state.ResumeTiming();
    auto status = target->recover_from(*source.journal, support::kNow + 100U);
    benchmark::DoNotOptimize(status);
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(source.journal->size()));
}

BENCHMARK(benchmark_oms_dispatch);
BENCHMARK(benchmark_oms_acknowledgement);
BENCHMARK(benchmark_oms_fill);
BENCHMARK(benchmark_oms_snapshot_read);
BENCHMARK(benchmark_oms_restart_replay);

} // namespace
