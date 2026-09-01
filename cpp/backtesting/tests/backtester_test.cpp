#include "aegis/backtesting/backtester.hpp"

#include "aegis/market_data/synthetic/artifacts.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include <cstdlib>

namespace backtest = aegis::backtesting;
namespace common = aegis::common;
namespace synthetic = aegis::market_data::synthetic;

namespace {

constexpr common::VenueId kVenueOne{0xA001U, 1U};
constexpr common::VenueId kVenueTwo{0xA001U, 2U};
constexpr common::InstrumentId kInstrumentOne{0xB001U, 1U};
constexpr common::InstrumentId kInstrumentTwo{0xB001U, 2U};
constexpr common::StrategyId kStrategyOne{0xC001U, 1U};
constexpr common::StrategyId kStrategyTwo{0xC001U, 2U};

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    std::array<char, 72U> pattern{};
    constexpr std::string_view prefix = "/tmp/aegis-backtester-test.XXXXXX";
    std::ranges::copy(prefix, pattern.begin());
    if (auto* path = ::mkdtemp(pattern.data()); path != nullptr) {
      path_ = path;
    }
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

[[nodiscard]] backtest::BacktestConfig configuration(const bool two_markets = false) {
  backtest::BacktestConfig config;
  config.deterministic_seed = 0x27AE'6150U;
  config.maximum_events = 10'000U;
  config.maximum_orders = 1'000U;
  config.maximum_external_orders = 2'000U;
  config.maximum_fills = 2'000U;
  config.stale_order_after_ns = 1'000U;
  config.default_forecast_horizon_ns = 10U;
  config.adverse_selection_horizon_ns = 10U;
  config.acknowledgement_latency.minimum_ns = 5U;
  config.acknowledgement_latency.maximum_ns = 5U;
  config.maker_fee_per_unit_currency_nanos = 0;
  config.taker_fee_per_unit_currency_nanos = 0;
  config.slippage_ticks = 0U;
  config.impact_ticks_per_million_units = 0U;
  config.instrument_count = two_markets ? 2U : 1U;
  config.instruments[0] = {.venue_number = 1U,
                           .instrument_number = 1U,
                           .venue_id = kVenueOne,
                           .instrument_id = kInstrumentOne,
                           .tick_value_currency_nanos = 1'000U};
  config.instruments[1] = {.venue_number = 2U,
                           .instrument_number = 2U,
                           .venue_id = kVenueTwo,
                           .instrument_id = kInstrumentTwo,
                           .tick_value_currency_nanos = 1'000U};
  return config;
}

[[nodiscard]] synthetic::SyntheticEvent
base_event(const std::uint64_t ordinal, const std::uint64_t time_ns,
           const std::uint32_t venue = 1U, const std::uint32_t instrument = 1U,
           const std::uint32_t channel = 1U, const std::uint64_t sequence = 0U) {
  return {.data_quality = synthetic::DataQuality::valid,
          .venue_number = venue,
          .channel_number = channel,
          .instrument_number = instrument,
          .channel_sequence = sequence == 0U ? ordinal : sequence,
          .global_ordinal = ordinal,
          .exchange_event_time_ns =
              1'800'000'000'000'000'000LL + static_cast<std::int64_t>(time_ns),
          .nic_receive_time_ns =
              1'800'000'000'000'000'100LL + static_cast<std::int64_t>(time_ns),
          .process_monotonic_time_ns = time_ns};
}

[[nodiscard]] synthetic::SyntheticEvent
// Test fixture builders intentionally expose the event fields at each call site.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
add_event(const std::uint64_t ordinal, const std::uint64_t time_ns,
          const synthetic::Side side, const std::int64_t price,
          const std::uint64_t quantity, const std::uint64_t order_id,
          const std::uint32_t venue = 1U, const std::uint32_t instrument = 1U,
          const std::uint32_t channel = 1U, const std::uint64_t sequence = 0U) {
  auto event = base_event(ordinal, time_ns, venue, instrument, channel, sequence);
  event.type = synthetic::NativeMessageType::add_order;
  event.side = side;
  event.action = synthetic::BookAction::add;
  event.message_flags = synthetic::kMessageFlagOrderLevel;
  event.synthetic_order_id = order_id;
  event.price_ticks = price;
  event.quantity_units = quantity;
  event.level_quantity_units = quantity;
  event.order_count = 1U;
  event.event_hash = synthetic::calculate_event_hash(event);
  return event;
}

[[nodiscard]] synthetic::SyntheticEvent
modify_event(const std::uint64_t ordinal, const std::uint64_t time_ns,
             const synthetic::Side side, const std::int64_t price,
             const std::uint64_t quantity, const std::uint64_t order_id) {
  auto event = base_event(ordinal, time_ns);
  event.type = synthetic::NativeMessageType::modify_order;
  event.side = side;
  event.action = synthetic::BookAction::change;
  event.message_flags = synthetic::kMessageFlagOrderLevel;
  event.synthetic_order_id = order_id;
  event.price_ticks = price;
  event.quantity_units = quantity;
  event.level_quantity_units = quantity;
  event.order_count = 1U;
  event.event_hash = synthetic::calculate_event_hash(event);
  return event;
}

[[nodiscard]] synthetic::SyntheticEvent trade_event(const std::uint64_t ordinal,
                                                    const std::uint64_t time_ns,
                                                    const synthetic::Side side,
                                                    const std::int64_t price,
                                                    const std::uint64_t quantity) {
  auto event = base_event(ordinal, time_ns);
  event.type = synthetic::NativeMessageType::trade;
  event.side = side;
  event.price_ticks = price;
  event.quantity_units = quantity;
  event.auxiliary_value = ordinal;
  event.event_hash = synthetic::calculate_event_hash(event);
  return event;
}

[[nodiscard]] synthetic::SyntheticEvent
price_level_event(const std::uint64_t ordinal, const std::uint64_t time_ns,
                  const synthetic::Side side, const std::int64_t price,
                  const std::uint64_t quantity) {
  auto event = base_event(ordinal, time_ns);
  event.type = synthetic::NativeMessageType::price_level;
  event.side = side;
  event.action = synthetic::BookAction::add;
  event.message_flags = synthetic::kMessageFlagPriceLevel;
  event.price_ticks = price;
  event.quantity_units = quantity;
  event.level_quantity_units = quantity;
  event.order_count = 1U;
  event.event_hash = synthetic::calculate_event_hash(event);
  return event;
}

[[nodiscard]] synthetic::SyntheticEvent
status_event(const std::uint64_t ordinal, const std::uint64_t time_ns,
             const synthetic::TradingStatus status) {
  auto event = base_event(ordinal, time_ns);
  event.type = synthetic::NativeMessageType::trading_status;
  event.status = status;
  event.event_hash = synthetic::calculate_event_hash(event);
  return event;
}

[[nodiscard]] synthetic::SyntheticEvent auction_event(const std::uint64_t ordinal,
                                                      const std::uint64_t time_ns,
                                                      const std::int64_t price,
                                                      const std::uint64_t quantity) {
  auto event = base_event(ordinal, time_ns);
  event.type = synthetic::NativeMessageType::auction_imbalance;
  event.side = synthetic::Side::ask;
  event.price_ticks = price;
  event.quantity_units = quantity;
  event.auxiliary_value = quantity;
  event.event_hash = synthetic::calculate_event_hash(event);
  return event;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

class OneShotStrategy final : public backtest::IBacktestStrategy {
public:
  OneShotStrategy(const std::uint64_t trigger, backtest::StrategyAction action)
      : trigger_(trigger), action_(action) {}

  [[nodiscard]] backtest::Status
  on_market_event(const backtest::MarketView& market,
                  const synthetic::SyntheticEvent& event,
                  backtest::StrategyActionBuffer& output) noexcept override {
    static_cast<void>(market);
    if (!sent_ && event.global_ordinal == trigger_) {
      if (!output.push(action_)) {
        return backtest::Status::capacity_exhausted;
      }
      sent_ = true;
    }
    return backtest::Status::ok;
  }

private:
  std::uint64_t trigger_{};
  backtest::StrategyAction action_;
  bool sent_{false};
};

class NoOpStrategy final : public backtest::IBacktestStrategy {
public:
  [[nodiscard]] backtest::Status
  on_market_event(const backtest::MarketView& market,
                  const synthetic::SyntheticEvent& event,
                  backtest::StrategyActionBuffer& output) noexcept override {
    static_cast<void>(market);
    static_cast<void>(event);
    static_cast<void>(output);
    return backtest::Status::ok;
  }
};

[[nodiscard]] backtest::StrategyAction bid_order(const std::uint64_t key = 1U,
                                                 const std::uint64_t quantity = 6U) {
  return {.kind = backtest::ActionKind::limit_order,
          .client_order_key = key,
          .venue_id = kVenueOne,
          .instrument_id = kInstrumentOne,
          .side = backtest::OrderSide::buy,
          .limit_price_ticks = 100,
          .quantity_units = quantity,
          .forecast = {}};
}

} // namespace

TEST(EventBacktester, ModelsCancellationExecutionAheadAndPartialFills) {
  auto config = configuration();
  backtest::EventBacktester engine{config};
  OneShotStrategy strategy{2U, bid_order()};
  ASSERT_EQ(engine.add_strategy(kStrategyOne, strategy), backtest::Status::ok);
  const std::array events{add_event(1U, 100U, synthetic::Side::bid, 100, 10U, 10U),
                          add_event(2U, 200U, synthetic::Side::ask, 102, 10U, 20U),
                          modify_event(3U, 210U, synthetic::Side::bid, 100, 6U, 10U),
                          trade_event(4U, 220U, synthetic::Side::bid, 100, 8U),
                          trade_event(5U, 240U, synthetic::Side::bid, 100, 4U)};
  ASSERT_EQ(engine.run_events(events), backtest::Status::complete);
  ASSERT_EQ(engine.orders().size(), 1U);
  const auto& order = engine.orders().front();
  EXPECT_EQ(order.state, backtest::OrderState::filled);
  EXPECT_EQ(order.initial_queue_ahead_units, 10U);
  EXPECT_EQ(order.cancellations_ahead_units, 4U);
  EXPECT_EQ(order.executions_ahead_units, 6U);
  EXPECT_EQ(order.filled_quantity_units, 6U);
  ASSERT_EQ(engine.fills().size(), 2U);
  EXPECT_EQ(engine.fills()[0].quantity_units, 2U);
  EXPECT_EQ(engine.fills()[1].quantity_units, 4U);
}

TEST(EventBacktester, TradeBeforeAcknowledgementCannotFillOrder) {
  auto config = configuration();
  config.acknowledgement_latency.minimum_ns = 50U;
  config.acknowledgement_latency.maximum_ns = 50U;
  backtest::EventBacktester engine{config};
  OneShotStrategy strategy{2U, bid_order(1U, 4U)};
  ASSERT_EQ(engine.add_strategy(kStrategyOne, strategy), backtest::Status::ok);
  const std::array events{add_event(1U, 100U, synthetic::Side::bid, 100, 1U, 10U),
                          add_event(2U, 200U, synthetic::Side::ask, 102, 10U, 20U),
                          trade_event(3U, 220U, synthetic::Side::bid, 100, 5U),
                          trade_event(4U, 260U, synthetic::Side::bid, 100, 4U)};
  ASSERT_EQ(engine.run_events(events), backtest::Status::complete);
  ASSERT_EQ(engine.fills().size(), 1U);
  EXPECT_EQ(engine.fills().front().process_monotonic_time_ns, 260U);
  EXPECT_EQ(engine.orders().front().acknowledgement_time_ns, 250U);
  EXPECT_FALSE(engine.report().uses_bar_close_fills);
}

TEST(EventBacktester, LatencyDistributionsAreBoundedAndDeterministic) {
  auto uniform_config = configuration();
  uniform_config.acknowledgement_latency.kind =
      backtest::LatencyDistributionKind::uniform;
  uniform_config.acknowledgement_latency.minimum_ns = 10U;
  uniform_config.acknowledgement_latency.maximum_ns = 20U;
  const std::array events{add_event(1U, 100U, synthetic::Side::bid, 100, 1U, 10U),
                          add_event(2U, 200U, synthetic::Side::ask, 102, 10U, 20U),
                          trade_event(3U, 300U, synthetic::Side::bid, 100, 20U)};
  OneShotStrategy first_strategy{2U, bid_order()};
  backtest::EventBacktester first{uniform_config};
  ASSERT_EQ(first.add_strategy(kStrategyOne, first_strategy), backtest::Status::ok);
  ASSERT_EQ(first.run_events(events), backtest::Status::complete);
  const auto first_ack = first.orders().front().acknowledgement_time_ns;
  EXPECT_GE(first_ack, 210U);
  EXPECT_LE(first_ack, 220U);

  OneShotStrategy second_strategy{2U, bid_order()};
  backtest::EventBacktester second{uniform_config};
  ASSERT_EQ(second.add_strategy(kStrategyOne, second_strategy), backtest::Status::ok);
  ASSERT_EQ(second.run_events(events), backtest::Status::complete);
  EXPECT_EQ(second.orders().front().acknowledgement_time_ns, first_ack);

  auto two_point_config = uniform_config;
  two_point_config.acknowledgement_latency.kind =
      backtest::LatencyDistributionKind::two_point;
  two_point_config.acknowledgement_latency.minimum_ns = 5U;
  two_point_config.acknowledgement_latency.secondary_ns = 37U;
  two_point_config.acknowledgement_latency.secondary_probability_ppm =
      backtest::kProbabilityScale;
  OneShotStrategy two_point_strategy{2U, bid_order()};
  backtest::EventBacktester two_point{two_point_config};
  ASSERT_EQ(two_point.add_strategy(kStrategyOne, two_point_strategy),
            backtest::Status::ok);
  ASSERT_EQ(two_point.run_events(events), backtest::Status::complete);
  EXPECT_EQ(two_point.orders().front().acknowledgement_time_ns, 237U);
}

TEST(EventBacktester, StaleOrdersExpireBeforeLaterTrade) {
  auto config = configuration();
  config.acknowledgement_latency.minimum_ns = 0U;
  config.acknowledgement_latency.maximum_ns = 0U;
  config.stale_order_after_ns = 5U;
  backtest::EventBacktester engine{config};
  OneShotStrategy strategy{2U, bid_order()};
  ASSERT_EQ(engine.add_strategy(kStrategyOne, strategy), backtest::Status::ok);
  const std::array events{add_event(1U, 100U, synthetic::Side::bid, 100, 1U, 10U),
                          add_event(2U, 200U, synthetic::Side::ask, 102, 10U, 20U),
                          trade_event(3U, 210U, synthetic::Side::bid, 100, 20U)};
  ASSERT_EQ(engine.run_events(events), backtest::Status::complete);
  EXPECT_EQ(engine.orders().front().state, backtest::OrderState::expired);
  EXPECT_TRUE(engine.fills().empty());
}

TEST(EventBacktester, HiddenLiquidityIsSeededRecordedAndAhead) {
  auto config = configuration();
  config.acknowledgement_latency.minimum_ns = 0U;
  config.acknowledgement_latency.maximum_ns = 0U;
  config.hidden_liquidity_probability_ppm = backtest::kProbabilityScale;
  config.maximum_hidden_liquidity_units = 1U;
  backtest::EventBacktester engine{config};
  OneShotStrategy strategy{2U, bid_order(1U, 1U)};
  ASSERT_EQ(engine.add_strategy(kStrategyOne, strategy), backtest::Status::ok);
  const std::array events{add_event(1U, 100U, synthetic::Side::bid, 100, 1U, 10U),
                          add_event(2U, 200U, synthetic::Side::ask, 102, 10U, 20U),
                          trade_event(3U, 210U, synthetic::Side::bid, 100, 1U),
                          trade_event(4U, 220U, synthetic::Side::bid, 100, 1U),
                          trade_event(5U, 230U, synthetic::Side::bid, 100, 1U)};
  ASSERT_EQ(engine.run_events(events), backtest::Status::complete);
  const auto& order = engine.orders().front();
  EXPECT_EQ(order.inferred_hidden_ahead_units, 1U);
  EXPECT_EQ(order.initial_queue_ahead_units, 2U);
  EXPECT_EQ(order.executions_ahead_units, 2U);
  EXPECT_EQ(order.state, backtest::OrderState::filled);
}

TEST(EventBacktester, PriceLevelInputIsExplicitlyReportedAsAggregatedQueue) {
  auto config = configuration();
  config.acknowledgement_latency.minimum_ns = 0U;
  config.acknowledgement_latency.maximum_ns = 0U;
  backtest::EventBacktester engine{config};
  OneShotStrategy strategy{2U, bid_order(1U, 1U)};
  ASSERT_EQ(engine.add_strategy(kStrategyOne, strategy), backtest::Status::ok);
  const std::array events{price_level_event(1U, 100U, synthetic::Side::bid, 100, 10U),
                          price_level_event(2U, 200U, synthetic::Side::ask, 102, 10U),
                          trade_event(3U, 220U, synthetic::Side::bid, 100, 11U)};
  ASSERT_EQ(engine.run_events(events), backtest::Status::complete);
  EXPECT_EQ(engine.report().worst_queue_model_quality,
            backtest::QueueModelQuality::aggregated_price_level);
  EXPECT_EQ(engine.orders().front().initial_queue_ahead_units, 10U);
  EXPECT_EQ(engine.orders().front().state, backtest::OrderState::filled);
}

TEST(EventBacktester, HaltsSuppressFillsAndAuctionReopensExplicitly) {
  auto config = configuration();
  config.acknowledgement_latency.minimum_ns = 0U;
  config.acknowledgement_latency.maximum_ns = 0U;
  auto action = bid_order(1U, 2U);
  action.limit_price_ticks = 101;
  backtest::EventBacktester engine{config};
  OneShotStrategy strategy{2U, action};
  ASSERT_EQ(engine.add_strategy(kStrategyOne, strategy), backtest::Status::ok);
  const std::array events{add_event(1U, 100U, synthetic::Side::bid, 100, 1U, 10U),
                          add_event(2U, 200U, synthetic::Side::ask, 102, 1U, 20U),
                          status_event(3U, 210U, synthetic::TradingStatus::halted),
                          trade_event(4U, 220U, synthetic::Side::bid, 101, 10U),
                          status_event(5U, 230U, synthetic::TradingStatus::auction),
                          auction_event(6U, 240U, 101, 10U),
                          status_event(7U, 250U, synthetic::TradingStatus::open)};
  ASSERT_EQ(engine.run_events(events), backtest::Status::complete);
  ASSERT_EQ(engine.fills().size(), 1U);
  EXPECT_EQ(engine.fills().front().process_monotonic_time_ns, 240U);
  const auto& periods = engine.report().strategies.front().event_periods;
  EXPECT_GT(
      periods[static_cast<std::size_t>(backtest::EventPeriod::halted)].event_count, 0U);
  EXPECT_GT(
      periods[static_cast<std::size_t>(backtest::EventPeriod::reopening)].event_count,
      0U);
}

TEST(EventBacktester, MultipleVenuesAndShadowOrdersRemainPortfolioIsolated) {
  auto config = configuration(true);
  config.acknowledgement_latency.minimum_ns = 0U;
  config.acknowledgement_latency.maximum_ns = 0U;
  auto first = bid_order(1U, 1U);
  first.kind = backtest::ActionKind::market_order;
  auto second = first;
  second.venue_id = kVenueTwo;
  second.instrument_id = kInstrumentTwo;
  second.client_order_key = 2U;
  second.shadow = true;
  backtest::EventBacktester engine{config};
  OneShotStrategy strategy_one{2U, first};
  OneShotStrategy strategy_two{4U, second};
  ASSERT_EQ(engine.add_strategy(kStrategyOne, strategy_one), backtest::Status::ok);
  ASSERT_EQ(engine.add_strategy(kStrategyTwo, strategy_two), backtest::Status::ok);
  const std::array events{
      add_event(1U, 100U, synthetic::Side::bid, 100, 10U, 10U),
      add_event(2U, 200U, synthetic::Side::ask, 102, 10U, 20U),
      add_event(3U, 300U, synthetic::Side::bid, 200, 10U, 30U, 2U, 2U, 2U, 1U),
      add_event(4U, 400U, synthetic::Side::ask, 202, 10U, 40U, 2U, 2U, 2U, 2U)};
  ASSERT_EQ(engine.run_events(events), backtest::Status::complete);
  ASSERT_EQ(engine.report().strategies.size(), 2U);
  EXPECT_NE(engine.report().strategies[0].portfolio.net_pnl_currency_nanos, 0);
  EXPECT_EQ(engine.report().strategies[1].portfolio.net_pnl_currency_nanos, 0);
  EXPECT_EQ(engine.report().strategies[1].execution.shadow_orders, 1U);
}

TEST(EventBacktester, ReportsForecastExecutionPortfolioAndCostSensitivity) {
  auto config = configuration();
  config.acknowledgement_latency.minimum_ns = 0U;
  config.acknowledgement_latency.maximum_ns = 0U;
  config.slippage_ticks = 1U;
  config.impact_ticks_per_million_units = 1U;
  config.taker_fee_per_unit_currency_nanos = 500;
  auto action = bid_order(1U, 2U);
  action.kind = backtest::ActionKind::market_order;
  action.forecast = {.valid = true,
                     .probability_up_ppm = 800'000U,
                     .return_p10_ppm = -10'000,
                     .return_p50_ppm = 2'000,
                     .return_p90_ppm = 10'000,
                     .horizon_ns = 10U};
  backtest::EventBacktester engine{config};
  OneShotStrategy strategy{2U, action};
  ASSERT_EQ(engine.add_strategy(kStrategyOne, strategy), backtest::Status::ok);
  const std::array events{add_event(1U, 100U, synthetic::Side::bid, 100, 10U, 10U),
                          add_event(2U, 200U, synthetic::Side::ask, 102, 10U, 20U),
                          add_event(3U, 220U, synthetic::Side::bid, 101, 10U, 30U),
                          trade_event(4U, 240U, synthetic::Side::ask, 102, 1U)};
  ASSERT_EQ(engine.run_events(events), backtest::Status::complete);
  const auto& report = engine.report();
  ASSERT_EQ(report.strategies.size(), 1U);
  const auto& strategy_report = report.strategies.front();
  EXPECT_EQ(strategy_report.forecast.resolved_samples, 1U);
  EXPECT_NEAR(strategy_report.forecast.log_loss, -std::log(0.8), 1.0e-12);
  EXPECT_NEAR(strategy_report.forecast.brier_score, 0.04, 1.0e-12);
  EXPECT_DOUBLE_EQ(strategy_report.forecast.directional_precision, 1.0);
  EXPECT_DOUBLE_EQ(strategy_report.forecast.p10_p90_coverage, 1.0);
  EXPECT_EQ(strategy_report.execution.filled_quantity_units, 2U);
  EXPECT_GT(strategy_report.portfolio.transaction_cost_currency_nanos, 0);
  EXPECT_GT(strategy_report.cost_sensitivity[0].net_pnl_currency_nanos,
            strategy_report.cost_sensitivity[2].net_pnl_currency_nanos);
  EXPECT_FALSE(report.raw_accuracy_only_claims_permitted);
  EXPECT_FALSE(report.live_trading_capable);
  const auto json = backtest::report_json(report);
  EXPECT_NE(json.find("forecast accuracy alone is not evidence of profitability"),
            std::string::npos);
  EXPECT_NE(json.find("\"cost_sensitivity\""), std::string::npos);
}

TEST(EventBacktester, RejectsAndUnsafeEventsFailOrSuppressClosed) {
  auto config = configuration();
  config.reject_probability_ppm = backtest::kProbabilityScale;
  config.acknowledgement_latency.minimum_ns = 0U;
  config.acknowledgement_latency.maximum_ns = 0U;
  backtest::EventBacktester engine{config};
  OneShotStrategy strategy{2U, bid_order()};
  ASSERT_EQ(engine.add_strategy(kStrategyOne, strategy), backtest::Status::ok);
  auto stale = trade_event(3U, 210U, synthetic::Side::bid, 100, 20U);
  stale.data_quality = synthetic::DataQuality::stale;
  stale.event_hash = synthetic::calculate_event_hash(stale);
  const std::array events{add_event(1U, 100U, synthetic::Side::bid, 100, 1U, 10U),
                          add_event(2U, 200U, synthetic::Side::ask, 102, 10U, 20U),
                          stale};
  ASSERT_EQ(engine.run_events(events), backtest::Status::complete);
  EXPECT_EQ(engine.orders().front().state, backtest::OrderState::rejected);
  EXPECT_EQ(engine.report().suppressed_unsafe_events, 1U);

  auto invalid = events;
  invalid[1].event_hash ^= 1U;
  NoOpStrategy no_op;
  backtest::EventBacktester invalid_engine{configuration()};
  ASSERT_EQ(invalid_engine.add_strategy(kStrategyOne, no_op), backtest::Status::ok);
  EXPECT_EQ(invalid_engine.run_events(invalid), backtest::Status::invalid_event);
}

TEST(EventBacktester, SyntheticAndCaptureRunsAreDeterministic) {
  auto generator_config = synthetic::make_default_config();
  generator_config.seed = 0x27C4'9700U;
  generator_config.event_count = 128U;
  generator_config.instrument_count = 1U;
  generator_config.venue_count = 1U;
  auto config = configuration();
  config.deterministic_seed = generator_config.seed;
  NoOpStrategy first_strategy;
  backtest::EventBacktester first{config};
  ASSERT_EQ(first.add_strategy(kStrategyOne, first_strategy), backtest::Status::ok);
  ASSERT_EQ(first.run_synthetic(generator_config), backtest::Status::complete);

  NoOpStrategy second_strategy;
  backtest::EventBacktester second{config};
  ASSERT_EQ(second.add_strategy(kStrategyOne, second_strategy), backtest::Status::ok);
  ASSERT_EQ(second.run_synthetic(generator_config), backtest::Status::complete);
  EXPECT_EQ(first.report().deterministic_result_sha256,
            second.report().deterministic_result_sha256);

  const TemporaryDirectory temporary;
  const synthetic::ArtifactPaths paths{.capture = temporary.path() / "events.smxcap",
                                       .canonical = temporary.path() / "events.amae",
                                       .expected_book =
                                           temporary.path() / "events.book.txt"};
  ASSERT_EQ(synthetic::generate_artifacts(generator_config, paths, false).error,
            synthetic::ArtifactError::none);
  NoOpStrategy capture_strategy;
  backtest::EventBacktester capture{config};
  ASSERT_EQ(capture.add_strategy(kStrategyOne, capture_strategy), backtest::Status::ok);
  EXPECT_EQ(capture.run_capture(paths.capture), backtest::Status::complete);
  EXPECT_EQ(first.report().deterministic_result_sha256,
            capture.report().deterministic_result_sha256);
}
