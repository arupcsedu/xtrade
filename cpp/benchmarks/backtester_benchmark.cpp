#include "aegis/backtesting/backtester.hpp"

#include "aegis/market_data/synthetic/config.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

namespace aegis::benchmarking {
namespace {

class NoOpStrategy final : public backtesting::IBacktestStrategy {
public:
  [[nodiscard]] backtesting::Status
  on_market_event(const backtesting::MarketView& market,
                  const market_data::synthetic::SyntheticEvent& event,
                  backtesting::StrategyActionBuffer& output) noexcept override {
    static_cast<void>(market);
    static_cast<void>(event);
    static_cast<void>(output);
    return backtesting::Status::ok;
  }
};

void benchmark_event_backtester_synthetic(benchmark::State& state) {
  constexpr std::uint64_t kEvents = 4'096U;
  auto generator = market_data::synthetic::make_default_config();
  generator.seed = 0xB3A6'2700U;
  generator.event_count = kEvents;
  generator.instrument_count = 1U;
  generator.venue_count = 1U;
  backtesting::BacktestConfig config;
  config.deterministic_seed = generator.seed;
  config.maximum_events = kEvents;
  config.instrument_count = 1U;
  config.instruments[0] = {
      .venue_number = generator.instruments[0].venue_number,
      .instrument_number = generator.instruments[0].instrument_number,
      .venue_id = common::VenueId{0x2700U, 1U},
      .instrument_id = common::InstrumentId{0x2701U, 1U},
      .tick_value_currency_nanos = generator.instruments[0].tick_value_currency_nanos};
  for (auto iteration : state) {
    static_cast<void>(iteration);
    NoOpStrategy strategy;
    backtesting::EventBacktester engine{config};
    if (engine.add_strategy(common::StrategyId{0x2702U, 1U}, strategy) !=
        backtesting::Status::ok) {
      state.SkipWithError("strategy registration failed");
      return;
    }
    if (engine.run_synthetic(generator) != backtesting::Status::complete) {
      state.SkipWithError("event backtest failed");
      return;
    }
    auto digest = engine.report().deterministic_result_sha256;
    benchmark::DoNotOptimize(digest);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kEvents));
}

BENCHMARK(benchmark_event_backtester_synthetic)->Unit(benchmark::kMicrosecond);

} // namespace
} // namespace aegis::benchmarking
