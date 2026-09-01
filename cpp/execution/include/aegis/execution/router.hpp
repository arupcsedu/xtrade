#ifndef AEGIS_EXECUTION_ROUTER_HPP
#define AEGIS_EXECUTION_ROUTER_HPP

#include "aegis/common/build_info.hpp"
#include "aegis/common/identifiers.hpp"
#include "aegis/risk/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace aegis::execution {

inline constexpr std::size_t kMaximumRouterVenues = 16U;
inline constexpr std::uint32_t kRouterPartsPerMillion = 1'000'000U;
inline constexpr std::int64_t kMaximumRouterPriceTicks = 10'000'000'000LL;
inline constexpr std::uint64_t kMaximumRouterQuantityUnits = 1'000'000'000'000ULL;
inline constexpr std::uint64_t kMaximumRouterTickValueCurrencyNanos = 1'000'000'000ULL;
inline constexpr std::int64_t kMaximumRouterAlphaMicroticks = 1'000'000'000LL;
inline constexpr std::int64_t kMaximumRouterFeeCurrencyNanosPerUnit = 1'000'000'000LL;

enum class ExecutionPolicy : std::uint8_t {
  passive_join = 1,
  passive_improve = 2,
  aggressive_take = 3,
  cancel_replace = 4,
  immediate_or_cancel = 5,
  participation = 6,
  twap = 7,
  vwap = 8,
  pov = 9,
  auction_participation = 10,
};

enum class RoutedAction : std::uint8_t {
  new_order = 1,
  cancel = 2,
  replace = 3,
};

enum class RoutedTimeInForce : std::uint8_t {
  day = 1,
  immediate_or_cancel = 2,
  auction = 3,
};

enum class VenueTradingState : std::uint8_t {
  unknown = 0,
  open = 1,
  auction = 2,
  halted = 3,
  closed = 4,
};

enum class SelfTradePreventionState : std::uint8_t {
  unknown = 0,
  clear = 1,
  conflict = 2,
};

enum class RoutingStatus : std::uint8_t {
  routed = 1,
  abstained = 2,
  invalid_configuration = 3,
  invalid_request = 4,
  stopped = 5,
};

enum class RoutingReason : std::uint8_t {
  routed = 1,
  invalid_configuration = 2,
  invalid_objective = 3,
  invalid_request = 4,
  objective_not_started = 5,
  objective_expired = 6,
  objective_complete = 7,
  slice_not_due = 8,
  market_volume_unavailable = 9,
  auction_liquidity_unavailable = 10,
  no_eligible_venue = 11,
  routing_loop = 12,
  arithmetic_overflow = 13,
  queue_deteriorated_cancel = 14,
  queue_deteriorated_replace = 15,
  stopped = 16,
};

enum class VenueEligibilityReason : std::uint8_t {
  eligible = 1,
  disabled = 2,
  policy_not_allowed = 3,
  regulatory_ineligible = 4,
  self_trade_conflict = 5,
  trading_state_ineligible = 6,
  quote_invalid = 7,
  quote_stale = 8,
  direct_consolidated_conflict = 9,
  reject_burst = 10,
  fill_probability_too_low = 11,
  concentration_exhausted = 12,
  already_visited = 13,
  price_unavailable = 14,
  working_order_venue_mismatch = 15,
  arithmetic_overflow = 16,
};

enum class RouterHealth : std::uint8_t {
  healthy = 1,
  unsafe = 2,
  stopped = 3,
};

// Strategy-owned objective. It intentionally has no VenueId, gateway command,
// or risk decision. A routed child must become a new exact RiskIntent downstream.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct ExecutionObjective {
  common::IntentId objective_id;
  common::SessionId session_id;
  common::AccountId account_id;
  common::StrategyId strategy_id;
  common::InstrumentId instrument_id;
  common::ForecastId source_forecast_id;
  common::FeatureSnapshotId feature_snapshot_id;
  common::ConfigurationVersion configuration_version;
  common::OrderId target_order_id;
  risk::IntentAction side{risk::IntentAction::buy};
  ExecutionPolicy policy{ExecutionPolicy::passive_join};
  std::int64_t limit_price_ticks{};
  std::uint64_t total_quantity_units{};
  std::uint64_t filled_quantity_units{};
  std::uint64_t maximum_child_quantity_units{};
  std::uint64_t start_process_monotonic_time_ns{};
  std::uint64_t end_process_monotonic_time_ns{};
  std::uint64_t slice_interval_ns{};
  std::uint64_t alpha_half_life_ns{};
  std::int64_t expected_alpha_microticks{};
  std::uint64_t tick_value_currency_nanos{};
  std::uint32_t participation_rate_ppm{};
  bool has_limit_price{true};
  std::uint64_t stable_hash{};
};

struct VenueRoutingRule {
  common::VenueId venue_id;
  std::uint32_t allowed_policy_mask{};
  std::uint32_t maximum_concentration_ppm{kRouterPartsPerMillion};
  std::uint64_t maximum_child_quantity_units{};
  std::uint16_t tie_break_rank{};
  bool enabled{false};
  bool regulatory_authorized{false};
  bool require_self_trade_clear{true};
  bool allow_auction{false};
};

struct RouterConfiguration {
  common::ConfigurationVersion configuration_version;
  std::uint64_t maximum_quote_age_ns{};
  std::uint64_t maximum_route_latency_ns{};
  std::uint64_t queue_deterioration_threshold_units{};
  std::uint64_t uncertainty_penalty_currency_nanos_per_unit{};
  std::uint32_t maximum_reject_rate_ppm{};
  std::uint32_t minimum_fill_probability_ppm{};
  std::uint32_t maximum_direct_consolidated_divergence_ticks{};
  std::uint16_t maximum_route_attempts{};
  std::array<VenueRoutingRule, kMaximumRouterVenues> venues{};
  std::uint16_t venue_count{};
  std::uint64_t stable_hash{};
};

struct ConsolidatedQuote {
  common::VenueId best_bid_venue_id;
  common::VenueId best_ask_venue_id;
  std::int64_t best_bid_price_ticks{};
  std::int64_t best_ask_price_ticks{};
  std::uint64_t observed_process_monotonic_time_ns{};
  bool valid{false};
  std::uint64_t stable_hash{};
};

struct VenueObservation {
  common::VenueId venue_id;
  common::InstrumentId instrument_id;
  VenueTradingState trading_state{VenueTradingState::unknown};
  SelfTradePreventionState self_trade_prevention{SelfTradePreventionState::unknown};
  std::int64_t bid_price_ticks{};
  std::int64_t ask_price_ticks{};
  std::uint64_t bid_quantity_units{};
  std::uint64_t ask_quantity_units{};
  std::uint64_t estimated_hidden_quantity_units{};
  std::uint64_t queue_ahead_quantity_units{};
  std::uint64_t venue_latency_ns{};
  std::uint64_t observed_process_monotonic_time_ns{};
  std::uint64_t already_routed_quantity_units{};
  std::uint64_t auction_paired_quantity_units{};
  std::int64_t auction_price_ticks{};
  std::int64_t maker_fee_currency_nanos_per_unit{};
  std::int64_t taker_fee_currency_nanos_per_unit{};
  std::int64_t adverse_selection_microticks{};
  std::uint32_t fill_probability_ppm{};
  std::uint32_t reject_rate_ppm{};
  bool quote_valid{false};
  bool regulatory_eligible{false};
  std::uint64_t stable_hash{};
};

struct WorkingOrderView {
  common::OrderId order_id;
  common::VenueId venue_id;
  std::int64_t price_ticks{};
  std::uint64_t remaining_quantity_units{};
  std::uint64_t initial_queue_ahead_quantity_units{};
  bool present{false};
};

struct RoutingRequest {
  ExecutionObjective objective;
  ConsolidatedQuote consolidated_quote;
  std::array<VenueObservation, kMaximumRouterVenues> venues{};
  std::array<common::VenueId, kMaximumRouterVenues> visited_venues{};
  WorkingOrderView working_order;
  std::uint64_t now_process_monotonic_time_ns{};
  std::uint64_t last_slice_process_monotonic_time_ns{};
  std::uint64_t market_volume_since_last_slice_units{};
  std::uint64_t already_scheduled_quantity_units{};
  std::uint32_t target_cumulative_volume_curve_ppm{};
  std::uint16_t venue_count{};
  std::uint16_t visited_venue_count{};
  std::uint16_t route_attempt{};
  std::uint64_t stable_hash{};
};

struct VenueScoreExplanation {
  common::VenueId venue_id;
  VenueEligibilityReason eligibility{VenueEligibilityReason::quote_invalid};
  std::int64_t passive_expected_value_currency_nanos_per_unit{};
  std::int64_t aggressive_expected_value_currency_nanos_per_unit{};
  std::int64_t selected_expected_value_currency_nanos_per_unit{};
  std::int64_t decayed_alpha_microticks{};
  std::uint64_t opportunity_cost_currency_nanos_per_unit{};
  std::uint32_t effective_fill_probability_ppm{};
  std::uint32_t fill_uncertainty_ppm{};
  std::uint64_t proposed_quantity_units{};
  std::int64_t proposed_price_ticks{};
  std::uint16_t tie_break_rank{};
  std::uint64_t observation_hash{};
};

struct RoutingDecision {
  RoutingStatus status{RoutingStatus::invalid_request};
  RoutingReason reason{RoutingReason::invalid_request};
  common::IntentId objective_id;
  common::VenueId venue_id;
  common::OrderId target_order_id;
  common::ConfigurationVersion configuration_version;
  RoutedAction action{RoutedAction::new_order};
  RoutedTimeInForce time_in_force{RoutedTimeInForce::day};
  std::int64_t price_ticks{};
  std::uint64_t quantity_units{};
  std::int64_t expected_value_currency_nanos_per_unit{};
  std::uint16_t route_attempt{};
  std::uint16_t evaluated_venue_count{};
  std::uint16_t eligible_venue_count{};
  std::array<VenueScoreExplanation, kMaximumRouterVenues> venue_scores{};
  std::uint64_t source_objective_hash{};
  std::uint64_t source_request_hash{};
  std::uint64_t configuration_hash{};
  bool requires_fresh_pretrade_risk{true};
  std::uint64_t stable_hash{};
};

struct ExecutionCostAttributionInput {
  common::IntentId objective_id;
  common::VenueId venue_id;
  risk::IntentAction side{risk::IntentAction::buy};
  std::int64_t arrival_price_ticks{};
  std::int64_t decision_price_ticks{};
  std::int64_t fill_price_ticks{};
  std::int64_t post_fill_reference_price_ticks{};
  std::uint64_t filled_quantity_units{};
  std::uint64_t unfilled_quantity_units{};
  std::uint64_t fee_currency_nanos{};
  std::uint64_t modeled_impact_ticks{};
  std::uint64_t tick_value_currency_nanos{};
};

struct ExecutionCostAttribution {
  common::IntentId objective_id;
  common::VenueId venue_id;
  std::int64_t spread_cost_currency_nanos{};
  std::int64_t slippage_cost_currency_nanos{};
  std::int64_t impact_cost_currency_nanos{};
  std::int64_t adverse_selection_cost_currency_nanos{};
  std::int64_t opportunity_cost_currency_nanos{};
  std::int64_t explicit_fee_currency_nanos{};
  std::int64_t total_cost_currency_nanos{};
  std::uint64_t stable_hash{};
};

struct RouterMetrics {
  std::uint64_t requests{};
  std::uint64_t routed{};
  std::uint64_t abstained{};
  std::uint64_t invalid{};
  std::uint64_t stale_quote_exclusions{};
  std::uint64_t halted_venue_exclusions{};
  std::uint64_t reject_burst_exclusions{};
  std::uint64_t self_trade_exclusions{};
  std::uint64_t concentration_exclusions{};
  std::uint64_t loop_exclusions{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] constexpr std::uint32_t
policy_bit(const ExecutionPolicy policy) noexcept {
  const auto ordinal = static_cast<std::uint8_t>(policy);
  return ordinal >= 1U && ordinal <= 31U
             ? std::uint32_t{1U} << static_cast<std::uint32_t>(ordinal - 1U)
             : 0U;
}

[[nodiscard]] bool valid_execution_policy(ExecutionPolicy policy) noexcept;
[[nodiscard]] bool valid_execution_objective(const ExecutionObjective& value) noexcept;
[[nodiscard]] bool
valid_router_configuration(const RouterConfiguration& value) noexcept;
[[nodiscard]] bool valid_consolidated_quote(const ConsolidatedQuote& value) noexcept;
[[nodiscard]] bool valid_venue_observation(const VenueObservation& value) noexcept;
[[nodiscard]] bool valid_routing_request(const RoutingRequest& value) noexcept;
[[nodiscard]] bool valid_routing_decision(const RoutingDecision& value) noexcept;
[[nodiscard]] std::uint32_t
fill_uncertainty_ppm(std::uint32_t probability_ppm) noexcept;
// Public fixed-unit operands remain adjacent for a compact arithmetic API.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] std::int64_t decay_alpha_microticks(std::int64_t alpha_microticks,
                                                  std::uint64_t elapsed_ns,
                                                  std::uint64_t half_life_ns) noexcept;
[[nodiscard]] std::uint64_t
stable_execution_objective_hash(const ExecutionObjective& value) noexcept;
[[nodiscard]] std::uint64_t
stable_router_configuration_hash(const RouterConfiguration& value) noexcept;
[[nodiscard]] std::uint64_t
stable_consolidated_quote_hash(const ConsolidatedQuote& value) noexcept;
[[nodiscard]] std::uint64_t
stable_venue_observation_hash(const VenueObservation& value) noexcept;
[[nodiscard]] std::uint64_t
stable_routing_request_hash(const RoutingRequest& value) noexcept;
[[nodiscard]] std::uint64_t
stable_routing_decision_hash(const RoutingDecision& value) noexcept;
[[nodiscard]] std::uint64_t
stable_execution_cost_attribution_hash(const ExecutionCostAttribution& value) noexcept;
[[nodiscard]] bool
execution_cost_attribution(const ExecutionCostAttributionInput& input,
                           ExecutionCostAttribution& output) noexcept;

class SmartOrderRouter final {
public:
  explicit SmartOrderRouter(RouterConfiguration configuration) noexcept;

  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] RouterHealth health() const noexcept;
  [[nodiscard]] static const common::BuildInfo& build_info() noexcept;
  [[nodiscard]] std::uint64_t configuration_hash() const noexcept;
  [[nodiscard]] RoutingDecision route(const RoutingRequest& request) noexcept;
  [[nodiscard]] RouterMetrics metrics() const noexcept;
  void shutdown() noexcept;

private:
  RouterConfiguration configuration_;
  RouterMetrics metrics_{};
  RouterHealth health_{RouterHealth::unsafe};
  bool initialized_{false};
};

static_assert(std::is_trivially_copyable_v<ExecutionObjective>);
static_assert(std::is_trivially_copyable_v<RoutingRequest>);
static_assert(std::is_trivially_copyable_v<RoutingDecision>);
static_assert(std::is_trivially_copyable_v<ExecutionCostAttribution>);

} // namespace aegis::execution

#endif // AEGIS_EXECUTION_ROUTER_HPP
