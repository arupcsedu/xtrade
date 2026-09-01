#include "aegis/risk/engine.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>

namespace risk = aegis::risk;

namespace {

struct ConcurrentCounts {
  std::atomic<std::uint64_t> journaled;
  std::atomic<std::uint64_t> busy;
  std::atomic<std::uint64_t> invalid;
};

void run_fill_updates(risk::DeterministicPreTradeRiskEngine& engine,
                      ConcurrentCounts& counts) {
  for (std::uint64_t index = 0U; index < 200U; ++index) {
    const auto status = engine.apply_fill(
        {.symbol_index = 0U,
         .strategy_index = 0U,
         .side = risk::IntentAction::buy,
         .fill_quantity_units = 1U,
         .release_reserved_quantity_units = 0U,
         .realized_pnl_delta_currency_nanos = 0,
         .as_of_process_monotonic_time_ns = risk::test::kNow + index});
    if (status != risk::StateUpdateStatus::applied &&
        status != risk::StateUpdateStatus::busy) {
      counts.invalid.fetch_add(1U, std::memory_order_relaxed);
    }
  }
}

void run_evaluations(risk::DeterministicPreTradeRiskEngine& engine,
                     ConcurrentCounts& counts, const std::uint64_t base) {
  for (std::uint64_t index = 0U; index < 200U; ++index) {
    const auto result = engine.evaluate(risk::test::request(base + index));
    if (result.status == risk::EvaluationStatus::journaled) {
      counts.journaled.fetch_add(1U, std::memory_order_relaxed);
    } else if (result.status == risk::EvaluationStatus::engine_busy) {
      counts.busy.fetch_add(1U, std::memory_order_relaxed);
    } else {
      counts.invalid.fetch_add(1U, std::memory_order_relaxed);
    }
  }
}

} // namespace

TEST(RiskConcurrencyTest, ConcurrentFillsAndOrdersRemainBoundedAndJournaled) {
  auto limits = risk::test::limits();
  limits.maximum_orders_per_window = 1'000U;
  limits.symbols[0U].maximum_absolute_position_units = 100'000U;
  limits.maximum_gross_exposure_currency_nanos = 100'000'000'000U;
  limits.maximum_absolute_net_exposure_currency_nanos = 100'000'000'000U;
  limits.maximum_sector_exposure_currency_nanos[0U] = 100'000'000'000U;
  limits.maximum_factor_exposure_currency_nanos[0U] = 100'000'000'000U;
  limits.credit_capital_limit_currency_nanos = 100'000'000'000U;
  limits.stable_hash = risk::stable_limit_snapshot_hash(limits);
  risk::RiskDecisionJournal journal;
  risk::DeterministicPreTradeRiskEngine engine{limits, journal};
  risk::test::initialize_state(engine);
  ConcurrentCounts counts;

  std::array<std::thread, 3> workers{
      std::thread{run_fill_updates, std::ref(engine), std::ref(counts)},
      std::thread{run_evaluations, std::ref(engine), std::ref(counts), 1'000U},
      std::thread{run_evaluations, std::ref(engine), std::ref(counts), 2'000U}};
  for (auto& worker : workers) {
    worker.join();
  }

  EXPECT_EQ(counts.invalid.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(journal.size(), counts.journaled.load(std::memory_order_relaxed));
  EXPECT_EQ(counts.journaled.load(std::memory_order_relaxed) +
                counts.busy.load(std::memory_order_relaxed),
            400U);
  const auto metrics = engine.metrics();
  EXPECT_EQ(metrics.approvals + metrics.rejections,
            counts.journaled.load(std::memory_order_relaxed));
}
