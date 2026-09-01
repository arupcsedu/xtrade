#include "aegis/backtesting/backtester.hpp"

#include "aegis/market_data/synthetic/config.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace backtest = aegis::backtesting;
namespace common = aegis::common;
namespace synthetic = aegis::market_data::synthetic;

namespace {

void usage() {
  std::cerr << "usage: aegis-backtest (--synthetic-events N | --capture PATH) "
               "[--seed N] [--strategies N] [--latency-min-ns N] "
               "[--latency-max-ns N] [--report PATH]\n";
}

[[nodiscard]] std::optional<std::uint64_t> parse_u64(const std::string_view value) {
  std::uint64_t result{};
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  return result;
}

class PeriodicJoinStrategy final : public backtest::IBacktestStrategy {
public:
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  PeriodicJoinStrategy(const std::uint64_t offset,
                       const std::uint64_t interval) noexcept
      : offset_(offset), interval_(interval) {}

  [[nodiscard]] backtest::Status
  on_market_event(const backtest::MarketView& market,
                  const synthetic::SyntheticEvent& event,
                  backtest::StrategyActionBuffer& output) noexcept override {
    if (!market.valid_book || event.global_ordinal < offset_ ||
        (event.global_ordinal - offset_) % interval_ != 0U) {
      return backtest::Status::ok;
    }
    const auto buy = ((event.global_ordinal / interval_) & 1U) == 0U;
    const backtest::StrategyAction action{
        .kind = backtest::ActionKind::limit_order,
        .client_order_key = event.global_ordinal,
        .venue_id = market.venue_id,
        .instrument_id = market.instrument_id,
        .side = buy ? backtest::OrderSide::buy : backtest::OrderSide::sell,
        .limit_price_ticks = buy ? market.best_bid_ticks : market.best_ask_ticks,
        .quantity_units = 10U,
        .time_in_force_ns = 10'000'000U,
        .forecast = {.valid = true,
                     .probability_up_ppm = buy ? 550'000U : 450'000U,
                     .return_p10_ppm = -10'000,
                     .return_p50_ppm = buy ? 500 : -500,
                     .return_p90_ppm = 10'000,
                     .horizon_ns = 1'000'000U}};
    return output.push(action) ? backtest::Status::ok
                               : backtest::Status::capacity_exhausted;
  }

private:
  std::uint64_t offset_{};
  std::uint64_t interval_{};
};

} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int main(int argc, char** argv) {
  std::optional<std::uint64_t> synthetic_events;
  std::filesystem::path capture;
  std::filesystem::path report_path;
  std::uint64_t seed = 20'260'831U;
  std::uint64_t strategy_count = 1U;
  std::uint64_t latency_min_ns = 50'000U;
  std::uint64_t latency_max_ns = 150'000U;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    const auto require_value = [&](std::string_view& value) {
      if (index + 1 >= argc) {
        return false;
      }
      value = argv[++index];
      return true;
    };
    std::string_view value;
    if (argument == "--synthetic-events" && require_value(value)) {
      synthetic_events = parse_u64(value);
    } else if (argument == "--capture" && require_value(value)) {
      capture = value;
    } else if (argument == "--seed" && require_value(value)) {
      const auto parsed = parse_u64(value);
      if (!parsed.has_value()) {
        usage();
        return 2;
      }
      seed = *parsed;
    } else if (argument == "--strategies" && require_value(value)) {
      const auto parsed = parse_u64(value);
      if (!parsed.has_value()) {
        usage();
        return 2;
      }
      strategy_count = *parsed;
    } else if (argument == "--latency-min-ns" && require_value(value)) {
      const auto parsed = parse_u64(value);
      if (!parsed.has_value()) {
        usage();
        return 2;
      }
      latency_min_ns = *parsed;
    } else if (argument == "--latency-max-ns" && require_value(value)) {
      const auto parsed = parse_u64(value);
      if (!parsed.has_value()) {
        usage();
        return 2;
      }
      latency_max_ns = *parsed;
    } else if (argument == "--report" && require_value(value)) {
      report_path = value;
    } else {
      usage();
      return 2;
    }
  }
  if (synthetic_events.has_value() == !capture.empty() || strategy_count == 0U ||
      strategy_count > backtest::kMaximumBacktestStrategies ||
      latency_max_ns < latency_min_ns) {
    usage();
    return 2;
  }

  auto generator_config = synthetic::make_default_config();
  generator_config.seed = seed;
  if (synthetic_events.has_value()) {
    generator_config.event_count = *synthetic_events;
  }
  backtest::BacktestConfig config;
  config.deterministic_seed = seed;
  config.acknowledgement_latency.kind =
      latency_min_ns == latency_max_ns ? backtest::LatencyDistributionKind::fixed
                                       : backtest::LatencyDistributionKind::uniform;
  config.acknowledgement_latency.minimum_ns = latency_min_ns;
  config.acknowledgement_latency.maximum_ns = latency_max_ns;
  config.hidden_liquidity_probability_ppm = 50'000U;
  config.maximum_hidden_liquidity_units = 100U;
  config.instrument_count = generator_config.instrument_count;
  for (std::size_t index = 0U; index < generator_config.instrument_count; ++index) {
    const auto& source = generator_config.instruments[index];
    config.instruments[index] = {
        .venue_number = source.venue_number,
        .instrument_number = source.instrument_number,
        .venue_id = common::VenueId{0x5645'4E55'4500'0000ULL, source.venue_number},
        .instrument_id =
            common::InstrumentId{0x494E'5354'0000'0000ULL, source.instrument_number},
        .tick_value_currency_nanos = source.tick_value_currency_nanos};
  }
  backtest::EventBacktester engine{config};
  std::vector<std::unique_ptr<PeriodicJoinStrategy>> strategies;
  strategies.reserve(static_cast<std::size_t>(strategy_count));
  for (std::uint64_t index = 0U; index < strategy_count; ++index) {
    strategies.push_back(std::make_unique<PeriodicJoinStrategy>(8U + index, 64U));
    const common::StrategyId strategy_id{0x5354'5241'5445'4759ULL, index + 1U};
    if (engine.add_strategy(strategy_id, *strategies.back()) != backtest::Status::ok) {
      std::cerr << "failed to register strategy\n";
      return 1;
    }
  }
  const auto result = synthetic_events.has_value()
                          ? engine.run_synthetic(generator_config)
                          : engine.run_capture(capture);
  if (result != backtest::Status::complete) {
    std::cerr << "backtest failed: " << backtest::status_name(result) << '\n';
    return 1;
  }
  if (!report_path.empty()) {
    if (!backtest::write_report(report_path, engine.report())) {
      std::cerr << "failed to write report\n";
      return 1;
    }
  } else {
    std::cout << backtest::report_json(engine.report());
  }
  return 0;
}
