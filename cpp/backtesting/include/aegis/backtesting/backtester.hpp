#ifndef AEGIS_BACKTESTING_BACKTESTER_HPP
#define AEGIS_BACKTESTING_BACKTESTER_HPP

#include "aegis/common/identifiers.hpp"
#include "aegis/common/sha256.hpp"
#include "aegis/market_data/synthetic/config.hpp"
#include "aegis/market_data/synthetic/event.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aegis::backtesting {

inline constexpr std::uint32_t kProbabilityScale = 1'000'000U;
inline constexpr std::size_t kMaximumStrategyActionsPerEvent = 8U;
inline constexpr std::size_t kMaximumBacktestStrategies = 32U;
inline constexpr std::size_t kMaximumBacktestInstruments = 64U;
inline constexpr std::size_t kCostScenarioCount = 4U;
inline constexpr std::size_t kEventPeriodCount = 5U;

enum class Status : std::uint8_t {
  ok = 0,
  complete,
  invalid_configuration,
  invalid_event,
  invalid_sequence,
  invalid_strategy_action,
  capacity_exhausted,
  arithmetic_overflow,
  strategy_failed,
  source_open_failed,
  source_invalid,
  report_io_error,
};

enum class ActionKind : std::uint8_t {
  forecast_only = 1,
  limit_order = 2,
  market_order = 3,
  cancel_order = 4,
};

enum class OrderSide : std::uint8_t { buy = 1, sell = 2 };

enum class OrderState : std::uint8_t {
  pending_ack = 1,
  working = 2,
  partially_filled = 3,
  filled = 4,
  rejected = 5,
  canceled = 6,
  expired = 7,
};

enum class LatencyDistributionKind : std::uint8_t {
  fixed = 1,
  uniform = 2,
  two_point = 3,
};

enum class EventPeriod : std::uint8_t {
  normal = 0,
  auction = 1,
  halted = 2,
  reopening = 3,
  shock = 4,
};

enum class QueueModelQuality : std::uint8_t {
  order_level = 1,
  aggregated_price_level = 2,
};

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct LatencyDistribution {
  LatencyDistributionKind kind{LatencyDistributionKind::fixed};
  std::uint64_t minimum_ns{50'000U};
  std::uint64_t maximum_ns{50'000U};
  std::uint64_t secondary_ns{50'000U};
  std::uint32_t secondary_probability_ppm{};
};

struct InstrumentMapping {
  std::uint32_t venue_number{};
  std::uint32_t instrument_number{};
  common::VenueId venue_id;
  common::InstrumentId instrument_id;
  std::int64_t tick_value_currency_nanos{1'000'000U};
};

struct BacktestConfig {
  std::uint64_t deterministic_seed{20'260'831U};
  std::uint64_t maximum_events{1'000'000U};
  std::uint64_t maximum_orders{100'000U};
  std::uint64_t maximum_external_orders{100'000U};
  std::uint64_t maximum_fills{500'000U};
  std::uint64_t stale_order_after_ns{5'000'000'000U};
  std::uint64_t default_forecast_horizon_ns{1'000'000'000U};
  std::uint64_t adverse_selection_horizon_ns{1'000'000'000U};
  LatencyDistribution acknowledgement_latency;
  std::uint32_t reject_probability_ppm{};
  std::uint32_t hidden_liquidity_probability_ppm{};
  std::uint64_t maximum_hidden_liquidity_units{};
  std::int64_t maker_fee_per_unit_currency_nanos{-100};
  std::int64_t taker_fee_per_unit_currency_nanos{500U};
  std::uint64_t slippage_ticks{1U};
  std::uint64_t impact_ticks_per_million_units{1U};
  bool allow_auction_orders{true};
  std::array<InstrumentMapping, kMaximumBacktestInstruments> instruments{};
  std::uint32_t instrument_count{};
  std::array<std::uint32_t, kCostScenarioCount> cost_multipliers_ppm{
      0U, 500'000U, 1'000'000U, 2'000'000U};
};

struct ForecastSignal {
  bool valid{false};
  std::uint32_t probability_up_ppm{500'000U};
  std::int64_t return_p10_ppm{};
  std::int64_t return_p50_ppm{};
  std::int64_t return_p90_ppm{};
  std::uint64_t horizon_ns{};
};

struct StrategyAction {
  ActionKind kind{ActionKind::forecast_only};
  std::uint64_t client_order_key{};
  common::VenueId venue_id;
  common::InstrumentId instrument_id;
  OrderSide side{OrderSide::buy};
  std::int64_t limit_price_ticks{};
  std::uint64_t quantity_units{};
  std::uint64_t time_in_force_ns{};
  bool shadow{false};
  ForecastSignal forecast;
};

struct MarketView {
  common::VenueId venue_id;
  common::InstrumentId instrument_id;
  std::uint32_t venue_number{};
  std::uint32_t instrument_number{};
  market_data::synthetic::TradingStatus status{
      market_data::synthetic::TradingStatus::pre_open};
  market_data::synthetic::DataQuality data_quality{
      market_data::synthetic::DataQuality::valid};
  std::uint64_t source_ordinal{};
  std::uint64_t process_monotonic_time_ns{};
  std::int64_t best_bid_ticks{};
  std::int64_t best_ask_ticks{};
  std::uint64_t best_bid_quantity_units{};
  std::uint64_t best_ask_quantity_units{};
  std::int64_t midpoint_x2{};
  std::int64_t last_trade_ticks{};
  QueueModelQuality queue_model_quality{QueueModelQuality::order_level};
  bool valid_book{false};
};

class StrategyActionBuffer final {
public:
  [[nodiscard]] bool push(const StrategyAction& action) noexcept;
  [[nodiscard]] std::span<const StrategyAction> actions() const noexcept;
  void clear() noexcept;

private:
  std::array<StrategyAction, kMaximumStrategyActionsPerEvent> actions_{};
  std::size_t size_{};
};

class IBacktestStrategy {
public:
  virtual ~IBacktestStrategy() = default;
  [[nodiscard]] virtual Status
  on_market_event(const MarketView& market,
                  const market_data::synthetic::SyntheticEvent& event,
                  StrategyActionBuffer& output) noexcept = 0;
};

struct SimulatedOrder {
  common::OrderId order_id;
  common::StrategyId strategy_id;
  common::VenueId venue_id;
  common::InstrumentId instrument_id;
  std::uint64_t client_order_key{};
  OrderSide side{OrderSide::buy};
  ActionKind kind{ActionKind::limit_order};
  OrderState state{OrderState::pending_ack};
  std::int64_t limit_price_ticks{};
  std::uint64_t requested_quantity_units{};
  std::uint64_t filled_quantity_units{};
  std::uint64_t initial_queue_ahead_units{};
  std::uint64_t cancellations_ahead_units{};
  std::uint64_t executions_ahead_units{};
  std::uint64_t inferred_hidden_ahead_units{};
  std::uint64_t decision_time_ns{};
  std::uint64_t acknowledgement_time_ns{};
  std::uint64_t first_fill_time_ns{};
  std::uint64_t last_fill_time_ns{};
  std::uint64_t expiry_time_ns{};
  std::int64_t arrival_midpoint_x2{};
  bool shadow{false};
};

struct SimulatedFill {
  common::GlobalEventId fill_id;
  common::OrderId order_id;
  common::StrategyId strategy_id;
  common::VenueId venue_id;
  common::InstrumentId instrument_id;
  OrderSide side{OrderSide::buy};
  std::uint64_t quantity_units{};
  std::int64_t raw_price_ticks{};
  std::int64_t execution_price_ticks{};
  std::int64_t arrival_midpoint_x2{};
  std::int64_t future_midpoint_x2{};
  std::int64_t fee_currency_nanos{};
  std::uint64_t process_monotonic_time_ns{};
  std::uint64_t mark_due_time_ns{};
  std::uint64_t source_event_hash{};
  std::uint64_t modeled_slippage_ticks{};
  std::uint64_t modeled_impact_ticks{};
  bool maker{true};
  bool shadow{false};
  bool future_mark_resolved{false};
};

struct ForecastMetrics {
  std::uint64_t resolved_samples{};
  std::uint64_t unresolved_samples{};
  double log_loss{};
  double brier_score{};
  double directional_precision{};
  double expected_calibration_error{};
  double p10_p90_coverage{};
};

struct ExecutionMetrics {
  std::uint64_t orders{};
  std::uint64_t rejected_orders{};
  std::uint64_t expired_orders{};
  std::uint64_t shadow_orders{};
  std::uint64_t requested_quantity_units{};
  std::uint64_t filled_quantity_units{};
  std::uint64_t partial_fill_events{};
  double fill_ratio{};
  double mean_time_to_fill_ns{};
  double effective_spread_ticks{};
  double realized_spread_ticks{};
  double slippage_ticks{};
  double adverse_selection_ticks{};
  double implementation_shortfall_currency_nanos{};
};

struct PortfolioMetrics {
  std::int64_t gross_pnl_currency_nanos{};
  std::int64_t transaction_cost_currency_nanos{};
  std::int64_t net_pnl_currency_nanos{};
  std::uint64_t turnover_currency_nanos{};
  double sharpe_event_sample{};
  double sortino_event_sample{};
  std::int64_t maximum_drawdown_currency_nanos{};
  double cvar_95_currency_nanos{};
};

struct CostSensitivityPoint {
  std::uint32_t cost_multiplier_ppm{};
  std::int64_t net_pnl_currency_nanos{};
};

struct EventPeriodMetrics {
  EventPeriod period{EventPeriod::normal};
  std::uint64_t event_count{};
  std::int64_t pnl_change_currency_nanos{};
};

struct StrategyReport {
  common::StrategyId strategy_id;
  ForecastMetrics forecast;
  ExecutionMetrics execution;
  PortfolioMetrics portfolio;
  std::array<CostSensitivityPoint, kCostScenarioCount> cost_sensitivity{};
  std::array<EventPeriodMetrics, kEventPeriodCount> event_periods{};
};

struct BacktestReport {
  Status status{Status::ok};
  std::uint64_t deterministic_seed{};
  std::uint64_t configuration_hash{};
  std::uint64_t source_events{};
  std::uint64_t accepted_events{};
  std::uint64_t suppressed_unsafe_events{};
  std::uint64_t order_count{};
  std::uint64_t fill_count{};
  std::uint64_t final_book_hash{};
  std::uint64_t source_event_chain_hash{};
  LatencyDistribution acknowledgement_latency;
  std::uint32_t hidden_liquidity_probability_ppm{};
  std::uint64_t maximum_hidden_liquidity_units{};
  std::int64_t maker_fee_per_unit_currency_nanos{};
  std::int64_t taker_fee_per_unit_currency_nanos{};
  std::uint64_t slippage_ticks{};
  std::uint64_t impact_ticks_per_million_units{};
  QueueModelQuality worst_queue_model_quality{QueueModelQuality::order_level};
  common::Sha256Digest deterministic_result_sha256{};
  std::vector<StrategyReport> strategies;
  bool uses_bar_close_fills{false};
  bool raw_accuracy_only_claims_permitted{false};
  bool live_trading_capable{false};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

class EventBacktester final {
public:
  explicit EventBacktester(BacktestConfig config);
  ~EventBacktester();
  EventBacktester(EventBacktester&&) noexcept;
  EventBacktester& operator=(EventBacktester&&) noexcept;
  EventBacktester(const EventBacktester&) = delete;
  EventBacktester& operator=(const EventBacktester&) = delete;

  [[nodiscard]] Status add_strategy(common::StrategyId strategy_id,
                                    IBacktestStrategy& strategy) noexcept;
  [[nodiscard]] Status
  run_events(std::span<const market_data::synthetic::SyntheticEvent> events) noexcept;
  [[nodiscard]] Status
  run_synthetic(const market_data::synthetic::GeneratorConfig& config) noexcept;
  [[nodiscard]] Status run_capture(const std::filesystem::path& path) noexcept;
  [[nodiscard]] const BacktestReport& report() const noexcept;
  [[nodiscard]] std::span<const SimulatedOrder> orders() const noexcept;
  [[nodiscard]] std::span<const SimulatedFill> fills() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] bool valid_config(const BacktestConfig& config) noexcept;
[[nodiscard]] std::uint64_t configuration_hash(BacktestConfig config) noexcept;
[[nodiscard]] std::string report_json(const BacktestReport& report);
[[nodiscard]] bool write_report(const std::filesystem::path& path,
                                const BacktestReport& report) noexcept;
[[nodiscard]] std::string_view status_name(Status status) noexcept;
[[nodiscard]] std::string_view order_state_name(OrderState state) noexcept;
[[nodiscard]] std::string_view event_period_name(EventPeriod period) noexcept;

} // namespace aegis::backtesting

#endif // AEGIS_BACKTESTING_BACKTESTER_HPP
