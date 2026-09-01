#include "aegis/execution/router.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace aegis::execution {
namespace {

constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;

void mix(std::uint64_t& hash, const std::uint64_t value) noexcept {
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    hash ^= (value >> shift) & 0xFFU;
    hash *= kFnvPrime;
  }
}

template <typename Identifier>
void mix_identifier(std::uint64_t& hash, const Identifier& value) noexcept {
  mix(hash, value.high());
  mix(hash, value.low());
}

[[nodiscard]] std::uint64_t nonzero(const std::uint64_t value) noexcept {
  return value == 0U ? 1U : value;
}

[[nodiscard]] bool valid_side(const risk::IntentAction side) noexcept {
  return side == risk::IntentAction::buy || side == risk::IntentAction::sell;
}

[[nodiscard]] bool valid_trading_state(const VenueTradingState state) noexcept {
  return state >= VenueTradingState::unknown && state <= VenueTradingState::closed;
}

[[nodiscard]] bool valid_stp(const SelfTradePreventionState state) noexcept {
  return state >= SelfTradePreventionState::unknown &&
         state <= SelfTradePreventionState::conflict;
}

[[nodiscard]] bool valid_eligibility(const VenueEligibilityReason reason) noexcept {
  return reason >= VenueEligibilityReason::eligible &&
         reason <= VenueEligibilityReason::arithmetic_overflow;
}

[[nodiscard]] bool valid_routing_reason(const RoutingReason reason) noexcept {
  return reason >= RoutingReason::routed && reason <= RoutingReason::stopped;
}

[[nodiscard]] bool valid_rule(const VenueRoutingRule& rule) noexcept {
  constexpr std::uint32_t kAllPolicies =
      (std::uint32_t{1U} << static_cast<std::uint8_t>(
           ExecutionPolicy::auction_participation)) -
      1U;
  return rule.venue_id.valid() && rule.allowed_policy_mask != 0U &&
         (rule.allowed_policy_mask & ~kAllPolicies) == 0U &&
         rule.maximum_concentration_ppm != 0U &&
         rule.maximum_concentration_ppm <= kRouterPartsPerMillion &&
         rule.maximum_child_quantity_units != 0U &&
         rule.maximum_child_quantity_units <= kMaximumRouterQuantityUnits;
}

[[nodiscard]] bool checked_directional_difference(const std::int64_t later,
                                                  const std::int64_t earlier,
                                                  const risk::IntentAction side,
                                                  std::int64_t& output) noexcept {
  if (side == risk::IntentAction::buy) {
    return !__builtin_sub_overflow(later, earlier, &output);
  }
  return !__builtin_sub_overflow(earlier, later, &output);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] bool checked_tick_cost(const std::int64_t ticks,
                                     const std::uint64_t quantity,
                                     const std::uint64_t tick_value,
                                     std::int64_t& output) noexcept {
  const bool negative = ticks < 0;
  const auto magnitude = negative
                             ? std::uint64_t{0U} - static_cast<std::uint64_t>(ticks)
                             : static_cast<std::uint64_t>(ticks);
  std::uint64_t product{};
  if (__builtin_mul_overflow(magnitude, quantity, &product) ||
      __builtin_mul_overflow(product, tick_value, &product) ||
      product > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  const auto signed_product = static_cast<std::int64_t>(product);
  output = negative ? -signed_product : signed_product;
  return true;
}

[[nodiscard]] bool checked_add_cost(const std::int64_t value,
                                    std::int64_t& total) noexcept {
  return !__builtin_add_overflow(total, value, &total);
}

} // namespace

bool valid_execution_policy(const ExecutionPolicy policy) noexcept {
  return policy >= ExecutionPolicy::passive_join &&
         policy <= ExecutionPolicy::auction_participation;
}

std::uint64_t
stable_execution_objective_hash(const ExecutionObjective& value) noexcept {
  std::uint64_t hash = kFnvOffset;
  mix_identifier(hash, value.objective_id);
  mix_identifier(hash, value.session_id);
  mix_identifier(hash, value.account_id);
  mix_identifier(hash, value.strategy_id);
  mix_identifier(hash, value.instrument_id);
  mix_identifier(hash, value.source_forecast_id);
  mix_identifier(hash, value.feature_snapshot_id);
  mix_identifier(hash, value.configuration_version);
  mix_identifier(hash, value.target_order_id);
  mix(hash, static_cast<std::uint8_t>(value.side));
  mix(hash, static_cast<std::uint8_t>(value.policy));
  mix(hash, std::bit_cast<std::uint64_t>(value.limit_price_ticks));
  mix(hash, value.total_quantity_units);
  mix(hash, value.filled_quantity_units);
  mix(hash, value.maximum_child_quantity_units);
  mix(hash, value.start_process_monotonic_time_ns);
  mix(hash, value.end_process_monotonic_time_ns);
  mix(hash, value.slice_interval_ns);
  mix(hash, value.alpha_half_life_ns);
  mix(hash, std::bit_cast<std::uint64_t>(value.expected_alpha_microticks));
  mix(hash, value.tick_value_currency_nanos);
  mix(hash, value.participation_rate_ppm);
  mix(hash, value.has_limit_price ? 1U : 0U);
  return nonzero(hash);
}

bool valid_execution_objective(const ExecutionObjective& value) noexcept {
  if (!value.objective_id.valid() || !value.session_id.valid() ||
      !value.account_id.valid() || !value.strategy_id.valid() ||
      !value.instrument_id.valid() || !value.source_forecast_id.valid() ||
      !value.feature_snapshot_id.valid() || !value.configuration_version.valid() ||
      !valid_side(value.side) || !valid_execution_policy(value.policy) ||
      !value.has_limit_price || value.limit_price_ticks <= 0 ||
      value.limit_price_ticks > kMaximumRouterPriceTicks ||
      value.total_quantity_units == 0U ||
      value.total_quantity_units > kMaximumRouterQuantityUnits ||
      value.filled_quantity_units > value.total_quantity_units ||
      value.maximum_child_quantity_units == 0U ||
      value.maximum_child_quantity_units > kMaximumRouterQuantityUnits ||
      value.start_process_monotonic_time_ns == 0U ||
      value.end_process_monotonic_time_ns <= value.start_process_monotonic_time_ns ||
      value.alpha_half_life_ns == 0U ||
      value.expected_alpha_microticks < -kMaximumRouterAlphaMicroticks ||
      value.expected_alpha_microticks > kMaximumRouterAlphaMicroticks ||
      value.tick_value_currency_nanos == 0U ||
      value.tick_value_currency_nanos > kMaximumRouterTickValueCurrencyNanos ||
      value.participation_rate_ppm > kRouterPartsPerMillion ||
      value.stable_hash == 0U ||
      value.stable_hash != stable_execution_objective_hash(value)) {
    return false;
  }
  if (value.policy == ExecutionPolicy::cancel_replace &&
      !value.target_order_id.valid()) {
    return false;
  }
  if ((value.policy == ExecutionPolicy::participation ||
       value.policy == ExecutionPolicy::pov ||
       value.policy == ExecutionPolicy::auction_participation) &&
      value.participation_rate_ppm == 0U) {
    return false;
  }
  return value.policy != ExecutionPolicy::twap || value.slice_interval_ns != 0U;
}

std::uint64_t
stable_router_configuration_hash(const RouterConfiguration& value) noexcept {
  std::uint64_t hash = kFnvOffset;
  mix_identifier(hash, value.configuration_version);
  mix(hash, value.maximum_quote_age_ns);
  mix(hash, value.maximum_route_latency_ns);
  mix(hash, value.queue_deterioration_threshold_units);
  mix(hash, value.uncertainty_penalty_currency_nanos_per_unit);
  mix(hash, value.maximum_reject_rate_ppm);
  mix(hash, value.minimum_fill_probability_ppm);
  mix(hash, value.maximum_direct_consolidated_divergence_ticks);
  mix(hash, value.maximum_route_attempts);
  mix(hash, value.venue_count);
  for (std::size_t index = 0U; index < value.venue_count && index < value.venues.size();
       ++index) {
    const auto& rule = value.venues[index];
    mix_identifier(hash, rule.venue_id);
    mix(hash, rule.allowed_policy_mask);
    mix(hash, rule.maximum_concentration_ppm);
    mix(hash, rule.maximum_child_quantity_units);
    mix(hash, rule.tie_break_rank);
    mix(hash, rule.enabled ? 1U : 0U);
    mix(hash, rule.regulatory_authorized ? 1U : 0U);
    mix(hash, rule.require_self_trade_clear ? 1U : 0U);
    mix(hash, rule.allow_auction ? 1U : 0U);
  }
  return nonzero(hash);
}

bool valid_router_configuration(const RouterConfiguration& value) noexcept {
  if (!value.configuration_version.valid() || value.maximum_quote_age_ns == 0U ||
      value.maximum_route_latency_ns == 0U ||
      value.uncertainty_penalty_currency_nanos_per_unit >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      value.maximum_reject_rate_ppm > kRouterPartsPerMillion ||
      value.minimum_fill_probability_ppm > kRouterPartsPerMillion ||
      value.maximum_route_attempts == 0U || value.venue_count == 0U ||
      value.venue_count > value.venues.size() || value.stable_hash == 0U ||
      value.stable_hash != stable_router_configuration_hash(value)) {
    return false;
  }
  for (std::size_t index = 0U; index < value.venue_count; ++index) {
    if (!valid_rule(value.venues[index])) {
      return false;
    }
    for (std::size_t other = 0U; other < index; ++other) {
      if (value.venues[other].venue_id == value.venues[index].venue_id) {
        return false;
      }
    }
  }
  return true;
}

std::uint64_t stable_consolidated_quote_hash(const ConsolidatedQuote& value) noexcept {
  std::uint64_t hash = kFnvOffset;
  mix_identifier(hash, value.best_bid_venue_id);
  mix_identifier(hash, value.best_ask_venue_id);
  mix(hash, std::bit_cast<std::uint64_t>(value.best_bid_price_ticks));
  mix(hash, std::bit_cast<std::uint64_t>(value.best_ask_price_ticks));
  mix(hash, value.observed_process_monotonic_time_ns);
  mix(hash, value.valid ? 1U : 0U);
  return nonzero(hash);
}

bool valid_consolidated_quote(const ConsolidatedQuote& value) noexcept {
  return value.valid && value.best_bid_venue_id.valid() &&
         value.best_ask_venue_id.valid() && value.best_bid_price_ticks > 0 &&
         value.best_ask_price_ticks > value.best_bid_price_ticks &&
         value.best_ask_price_ticks <= kMaximumRouterPriceTicks &&
         value.observed_process_monotonic_time_ns != 0U && value.stable_hash != 0U &&
         value.stable_hash == stable_consolidated_quote_hash(value);
}

std::uint64_t stable_venue_observation_hash(const VenueObservation& value) noexcept {
  std::uint64_t hash = kFnvOffset;
  mix_identifier(hash, value.venue_id);
  mix_identifier(hash, value.instrument_id);
  mix(hash, static_cast<std::uint8_t>(value.trading_state));
  mix(hash, static_cast<std::uint8_t>(value.self_trade_prevention));
  mix(hash, std::bit_cast<std::uint64_t>(value.bid_price_ticks));
  mix(hash, std::bit_cast<std::uint64_t>(value.ask_price_ticks));
  mix(hash, value.bid_quantity_units);
  mix(hash, value.ask_quantity_units);
  mix(hash, value.estimated_hidden_quantity_units);
  mix(hash, value.queue_ahead_quantity_units);
  mix(hash, value.venue_latency_ns);
  mix(hash, value.observed_process_monotonic_time_ns);
  mix(hash, value.already_routed_quantity_units);
  mix(hash, value.auction_paired_quantity_units);
  mix(hash, std::bit_cast<std::uint64_t>(value.auction_price_ticks));
  mix(hash, std::bit_cast<std::uint64_t>(value.maker_fee_currency_nanos_per_unit));
  mix(hash, std::bit_cast<std::uint64_t>(value.taker_fee_currency_nanos_per_unit));
  mix(hash, std::bit_cast<std::uint64_t>(value.adverse_selection_microticks));
  mix(hash, value.fill_probability_ppm);
  mix(hash, value.reject_rate_ppm);
  mix(hash, value.quote_valid ? 1U : 0U);
  mix(hash, value.regulatory_eligible ? 1U : 0U);
  return nonzero(hash);
}

bool valid_venue_observation(const VenueObservation& value) noexcept {
  if (!value.venue_id.valid() || !value.instrument_id.valid() ||
      !valid_trading_state(value.trading_state) ||
      !valid_stp(value.self_trade_prevention) ||
      value.fill_probability_ppm > kRouterPartsPerMillion ||
      value.reject_rate_ppm > kRouterPartsPerMillion ||
      value.bid_quantity_units > kMaximumRouterQuantityUnits ||
      value.ask_quantity_units > kMaximumRouterQuantityUnits ||
      value.estimated_hidden_quantity_units > kMaximumRouterQuantityUnits ||
      value.queue_ahead_quantity_units > kMaximumRouterQuantityUnits ||
      value.already_routed_quantity_units > kMaximumRouterQuantityUnits ||
      value.auction_paired_quantity_units > kMaximumRouterQuantityUnits ||
      value.maker_fee_currency_nanos_per_unit <
          -kMaximumRouterFeeCurrencyNanosPerUnit ||
      value.maker_fee_currency_nanos_per_unit > kMaximumRouterFeeCurrencyNanosPerUnit ||
      value.taker_fee_currency_nanos_per_unit <
          -kMaximumRouterFeeCurrencyNanosPerUnit ||
      value.taker_fee_currency_nanos_per_unit > kMaximumRouterFeeCurrencyNanosPerUnit ||
      value.adverse_selection_microticks < -kMaximumRouterAlphaMicroticks ||
      value.adverse_selection_microticks > kMaximumRouterAlphaMicroticks ||
      value.stable_hash == 0U ||
      value.stable_hash != stable_venue_observation_hash(value)) {
    return false;
  }
  if (!value.quote_valid) {
    return true;
  }
  return value.bid_price_ticks > 0 && value.ask_price_ticks > value.bid_price_ticks &&
         value.ask_price_ticks <= kMaximumRouterPriceTicks &&
         value.bid_quantity_units <= kMaximumRouterQuantityUnits &&
         value.ask_quantity_units <= kMaximumRouterQuantityUnits &&
         value.observed_process_monotonic_time_ns != 0U;
}

std::uint64_t stable_routing_request_hash(const RoutingRequest& value) noexcept {
  std::uint64_t hash = kFnvOffset;
  mix(hash, value.objective.stable_hash);
  mix(hash, value.consolidated_quote.stable_hash);
  mix(hash, value.now_process_monotonic_time_ns);
  mix(hash, value.last_slice_process_monotonic_time_ns);
  mix(hash, value.market_volume_since_last_slice_units);
  mix(hash, value.already_scheduled_quantity_units);
  mix(hash, value.target_cumulative_volume_curve_ppm);
  mix(hash, value.venue_count);
  mix(hash, value.visited_venue_count);
  mix(hash, value.route_attempt);
  mix_identifier(hash, value.working_order.order_id);
  mix_identifier(hash, value.working_order.venue_id);
  mix(hash, std::bit_cast<std::uint64_t>(value.working_order.price_ticks));
  mix(hash, value.working_order.remaining_quantity_units);
  mix(hash, value.working_order.initial_queue_ahead_quantity_units);
  mix(hash, value.working_order.present ? 1U : 0U);
  for (std::size_t index = 0U; index < value.venue_count && index < value.venues.size();
       ++index) {
    mix(hash, value.venues[index].stable_hash);
  }
  for (std::size_t index = 0U;
       index < value.visited_venue_count && index < value.visited_venues.size();
       ++index) {
    mix_identifier(hash, value.visited_venues[index]);
  }
  return nonzero(hash);
}

bool valid_routing_request(const RoutingRequest& value) noexcept {
  if (!valid_execution_objective(value.objective) ||
      !valid_consolidated_quote(value.consolidated_quote) ||
      value.now_process_monotonic_time_ns == 0U ||
      value.last_slice_process_monotonic_time_ns >
          value.now_process_monotonic_time_ns ||
      value.target_cumulative_volume_curve_ppm > kRouterPartsPerMillion ||
      value.venue_count == 0U || value.venue_count > value.venues.size() ||
      value.visited_venue_count > value.visited_venues.size() ||
      value.stable_hash == 0U ||
      value.stable_hash != stable_routing_request_hash(value)) {
    return false;
  }
  if (value.working_order.present &&
      (!value.working_order.order_id.valid() || !value.working_order.venue_id.valid() ||
       value.working_order.price_ticks <= 0 ||
       value.working_order.remaining_quantity_units == 0U)) {
    return false;
  }
  for (std::size_t index = 0U; index < value.venue_count; ++index) {
    if (!valid_venue_observation(value.venues[index]) ||
        value.venues[index].instrument_id != value.objective.instrument_id) {
      return false;
    }
    for (std::size_t other = 0U; other < index; ++other) {
      if (value.venues[other].venue_id == value.venues[index].venue_id) {
        return false;
      }
    }
  }
  for (std::size_t index = 0U; index < value.visited_venue_count; ++index) {
    if (!value.visited_venues[index].valid()) {
      return false;
    }
  }
  return true;
}

std::uint32_t fill_uncertainty_ppm(const std::uint32_t probability_ppm) noexcept {
  if (probability_ppm > kRouterPartsPerMillion) {
    return kRouterPartsPerMillion;
  }
  const auto complement = kRouterPartsPerMillion - probability_ppm;
  const auto product = static_cast<std::uint64_t>(probability_ppm) * complement;
  return static_cast<std::uint32_t>((4U * product) / kRouterPartsPerMillion);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::int64_t decay_alpha_microticks(const std::int64_t alpha_microticks,
                                    const std::uint64_t elapsed_ns,
                                    const std::uint64_t half_life_ns) noexcept {
  if (half_life_ns == 0U || alpha_microticks == 0) {
    return 0;
  }
  const auto completed_half_lives = elapsed_ns / half_life_ns;
  if (completed_half_lives >= 63U) {
    return 0;
  }
  const auto divisor = std::uint64_t{1U} << completed_half_lives;
  const auto base = alpha_microticks / static_cast<std::int64_t>(divisor);
  const auto remainder_ns = elapsed_ns % half_life_ns;
  std::uint64_t fraction_ppm{};
  if (half_life_ns >= kRouterPartsPerMillion) {
    const auto nanoseconds_per_part = half_life_ns / kRouterPartsPerMillion;
    fraction_ppm = remainder_ns / nanoseconds_per_part;
  } else {
    fraction_ppm = (remainder_ns * kRouterPartsPerMillion) / half_life_ns;
  }
  fraction_ppm = std::min<std::uint64_t>(fraction_ppm, kRouterPartsPerMillion);
  const auto multiplier_ppm = kRouterPartsPerMillion - (fraction_ppm / 2U);
  return (base * static_cast<std::int64_t>(multiplier_ppm)) /
         static_cast<std::int64_t>(kRouterPartsPerMillion);
}

std::uint64_t stable_routing_decision_hash(const RoutingDecision& value) noexcept {
  std::uint64_t hash = kFnvOffset;
  mix(hash, static_cast<std::uint8_t>(value.status));
  mix(hash, static_cast<std::uint8_t>(value.reason));
  mix_identifier(hash, value.objective_id);
  mix_identifier(hash, value.venue_id);
  mix_identifier(hash, value.target_order_id);
  mix_identifier(hash, value.configuration_version);
  mix(hash, static_cast<std::uint8_t>(value.action));
  mix(hash, static_cast<std::uint8_t>(value.time_in_force));
  mix(hash, std::bit_cast<std::uint64_t>(value.price_ticks));
  mix(hash, value.quantity_units);
  mix(hash, std::bit_cast<std::uint64_t>(value.expected_value_currency_nanos_per_unit));
  mix(hash, value.route_attempt);
  mix(hash, value.evaluated_venue_count);
  mix(hash, value.eligible_venue_count);
  for (std::size_t index = 0U;
       index < value.evaluated_venue_count && index < value.venue_scores.size();
       ++index) {
    const auto& score = value.venue_scores[index];
    mix_identifier(hash, score.venue_id);
    mix(hash, static_cast<std::uint8_t>(score.eligibility));
    mix(hash, std::bit_cast<std::uint64_t>(
                  score.passive_expected_value_currency_nanos_per_unit));
    mix(hash, std::bit_cast<std::uint64_t>(
                  score.aggressive_expected_value_currency_nanos_per_unit));
    mix(hash, std::bit_cast<std::uint64_t>(
                  score.selected_expected_value_currency_nanos_per_unit));
    mix(hash, std::bit_cast<std::uint64_t>(score.decayed_alpha_microticks));
    mix(hash, score.opportunity_cost_currency_nanos_per_unit);
    mix(hash, score.effective_fill_probability_ppm);
    mix(hash, score.fill_uncertainty_ppm);
    mix(hash, score.proposed_quantity_units);
    mix(hash, std::bit_cast<std::uint64_t>(score.proposed_price_ticks));
    mix(hash, score.tie_break_rank);
    mix(hash, score.observation_hash);
  }
  mix(hash, value.source_objective_hash);
  mix(hash, value.source_request_hash);
  mix(hash, value.configuration_hash);
  mix(hash, value.requires_fresh_pretrade_risk ? 1U : 0U);
  return nonzero(hash);
}

bool valid_routing_decision(const RoutingDecision& value) noexcept {
  if (value.status < RoutingStatus::routed || value.status > RoutingStatus::stopped ||
      !valid_routing_reason(value.reason) || !value.objective_id.valid() ||
      !value.configuration_version.valid() ||
      value.evaluated_venue_count > value.venue_scores.size() ||
      value.eligible_venue_count > value.evaluated_venue_count ||
      value.source_objective_hash == 0U || value.source_request_hash == 0U ||
      value.configuration_hash == 0U || !value.requires_fresh_pretrade_risk ||
      value.stable_hash == 0U ||
      value.stable_hash != stable_routing_decision_hash(value)) {
    return false;
  }
  for (std::size_t index = 0U; index < value.evaluated_venue_count; ++index) {
    if (!value.venue_scores[index].venue_id.valid() ||
        !valid_eligibility(value.venue_scores[index].eligibility) ||
        value.venue_scores[index].effective_fill_probability_ppm >
            kRouterPartsPerMillion ||
        value.venue_scores[index].fill_uncertainty_ppm > kRouterPartsPerMillion) {
      return false;
    }
  }
  if (value.status != RoutingStatus::routed) {
    return !value.venue_id.valid() && value.quantity_units == 0U;
  }
  return value.reason == RoutingReason::routed && value.venue_id.valid() &&
         value.quantity_units != 0U && value.price_ticks > 0 &&
         value.eligible_venue_count != 0U && value.action >= RoutedAction::new_order &&
         value.action <= RoutedAction::replace &&
         value.time_in_force >= RoutedTimeInForce::day &&
         value.time_in_force <= RoutedTimeInForce::auction;
}

std::uint64_t
stable_execution_cost_attribution_hash(const ExecutionCostAttribution& value) noexcept {
  std::uint64_t hash = kFnvOffset;
  mix_identifier(hash, value.objective_id);
  mix_identifier(hash, value.venue_id);
  mix(hash, std::bit_cast<std::uint64_t>(value.spread_cost_currency_nanos));
  mix(hash, std::bit_cast<std::uint64_t>(value.slippage_cost_currency_nanos));
  mix(hash, std::bit_cast<std::uint64_t>(value.impact_cost_currency_nanos));
  mix(hash, std::bit_cast<std::uint64_t>(value.adverse_selection_cost_currency_nanos));
  mix(hash, std::bit_cast<std::uint64_t>(value.opportunity_cost_currency_nanos));
  mix(hash, std::bit_cast<std::uint64_t>(value.explicit_fee_currency_nanos));
  mix(hash, std::bit_cast<std::uint64_t>(value.total_cost_currency_nanos));
  return nonzero(hash);
}

bool execution_cost_attribution(const ExecutionCostAttributionInput& input,
                                ExecutionCostAttribution& output) noexcept {
  if (!input.objective_id.valid() || !input.venue_id.valid() ||
      !valid_side(input.side) || input.arrival_price_ticks <= 0 ||
      input.decision_price_ticks <= 0 || input.fill_price_ticks <= 0 ||
      input.post_fill_reference_price_ticks <= 0 ||
      (input.filled_quantity_units == 0U && input.unfilled_quantity_units == 0U) ||
      input.tick_value_currency_nanos == 0U ||
      input.tick_value_currency_nanos > kMaximumRouterTickValueCurrencyNanos ||
      input.fee_currency_nanos >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }

  ExecutionCostAttribution result{.objective_id = input.objective_id,
                                  .venue_id = input.venue_id};
  std::int64_t ticks{};
  if (!checked_directional_difference(input.decision_price_ticks,
                                      input.arrival_price_ticks, input.side, ticks) ||
      !checked_tick_cost(ticks, input.filled_quantity_units,
                         input.tick_value_currency_nanos,
                         result.spread_cost_currency_nanos) ||
      !checked_directional_difference(input.fill_price_ticks,
                                      input.decision_price_ticks, input.side, ticks) ||
      !checked_tick_cost(ticks, input.filled_quantity_units,
                         input.tick_value_currency_nanos,
                         result.slippage_cost_currency_nanos) ||
      input.modeled_impact_ticks >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      !checked_tick_cost(static_cast<std::int64_t>(input.modeled_impact_ticks),
                         input.filled_quantity_units, input.tick_value_currency_nanos,
                         result.impact_cost_currency_nanos) ||
      !checked_directional_difference(input.fill_price_ticks,
                                      input.post_fill_reference_price_ticks, input.side,
                                      ticks) ||
      !checked_tick_cost(ticks, input.filled_quantity_units,
                         input.tick_value_currency_nanos,
                         result.adverse_selection_cost_currency_nanos) ||
      !checked_directional_difference(input.post_fill_reference_price_ticks,
                                      input.arrival_price_ticks, input.side, ticks) ||
      !checked_tick_cost(ticks, input.unfilled_quantity_units,
                         input.tick_value_currency_nanos,
                         result.opportunity_cost_currency_nanos)) {
    return false;
  }
  result.explicit_fee_currency_nanos =
      static_cast<std::int64_t>(input.fee_currency_nanos);
  std::int64_t total{};
  if (!checked_add_cost(result.spread_cost_currency_nanos, total) ||
      !checked_add_cost(result.slippage_cost_currency_nanos, total) ||
      !checked_add_cost(result.impact_cost_currency_nanos, total) ||
      !checked_add_cost(result.adverse_selection_cost_currency_nanos, total) ||
      !checked_add_cost(result.opportunity_cost_currency_nanos, total) ||
      !checked_add_cost(result.explicit_fee_currency_nanos, total)) {
    return false;
  }
  result.total_cost_currency_nanos = total;
  result.stable_hash = stable_execution_cost_attribution_hash(result);
  output = result;
  return true;
}

} // namespace aegis::execution
