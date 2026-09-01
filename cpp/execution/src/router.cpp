#include "aegis/execution/router.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace aegis::execution {
namespace {

struct SliceResult {
  RoutingReason reason{RoutingReason::routed};
  std::uint64_t quantity_units{};
  bool due{false};
};

struct ValueResult {
  std::int64_t expected_value_currency_nanos_per_unit{};
  std::int64_t decayed_alpha_microticks{};
  std::uint64_t opportunity_cost_currency_nanos_per_unit{};
  std::uint32_t effective_fill_probability_ppm{};
  std::uint32_t uncertainty_ppm{};
  bool valid{false};
};

[[nodiscard]] bool checked_add(const std::uint64_t left, const std::uint64_t right,
                               std::uint64_t& output) noexcept {
  return !__builtin_add_overflow(left, right, &output);
}

[[nodiscard]] bool checked_add_signed(const std::int64_t left, const std::int64_t right,
                                      std::int64_t& output) noexcept {
  return !__builtin_add_overflow(left, right, &output);
}

[[nodiscard]] bool checked_sub_signed(const std::int64_t left, const std::int64_t right,
                                      std::int64_t& output) noexcept {
  return !__builtin_sub_overflow(left, right, &output);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] bool multiply_divide(const std::uint64_t left, const std::uint64_t right,
                                   const std::uint64_t divisor,
                                   std::uint64_t& output) noexcept {
  if (divisor == 0U) {
    return false;
  }
  const auto quotient = left / divisor;
  const auto remainder = left % divisor;
  std::uint64_t whole{};
  std::uint64_t partial_product{};
  if (__builtin_mul_overflow(quotient, right, &whole) ||
      __builtin_mul_overflow(remainder, right, &partial_product)) {
    return false;
  }
  return checked_add(whole, partial_product / divisor, output);
}

[[nodiscard]] bool multiply_divide_ceil(const std::uint64_t left,
                                        const std::uint64_t right,
                                        const std::uint64_t divisor,
                                        std::uint64_t& output) noexcept {
  std::uint64_t product{};
  if (divisor == 0U || __builtin_mul_overflow(left, right, &product)) {
    return false;
  }
  output = product / divisor;
  return product % divisor == 0U || checked_add(output, 1U, output);
}

[[nodiscard]] std::uint64_t signed_magnitude(const std::int64_t value) noexcept {
  return value < 0 ? std::uint64_t{0U} - static_cast<std::uint64_t>(value)
                   : static_cast<std::uint64_t>(value);
}

[[nodiscard]] bool signed_multiply_divide(const std::int64_t left,
                                          const std::uint64_t right,
                                          const std::uint64_t divisor,
                                          std::int64_t& output) noexcept {
  std::uint64_t magnitude{};
  if (!multiply_divide(signed_magnitude(left), right, divisor, magnitude) ||
      magnitude >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  const auto signed_value = static_cast<std::int64_t>(magnitude);
  output = left < 0 ? -signed_value : signed_value;
  return true;
}

[[nodiscard]] bool microticks_to_currency_nanos(const std::int64_t microticks,
                                                const std::uint64_t tick_value,
                                                std::int64_t& output) noexcept {
  return signed_multiply_divide(microticks, tick_value, kRouterPartsPerMillion, output);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] bool ticks_to_currency_nanos(const std::int64_t ticks,
                                           const std::uint64_t tick_value,
                                           std::int64_t& output) noexcept {
  const auto magnitude = signed_magnitude(ticks);
  std::uint64_t product{};
  if (__builtin_mul_overflow(magnitude, tick_value, &product) ||
      product > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  const auto signed_value = static_cast<std::int64_t>(product);
  output = ticks < 0 ? -signed_value : signed_value;
  return true;
}

[[nodiscard]] const VenueRoutingRule*
find_rule(const RouterConfiguration& configuration,
          const common::VenueId venue_id) noexcept {
  for (std::size_t index = 0U; index < configuration.venue_count; ++index) {
    if (configuration.venues[index].venue_id == venue_id) {
      return &configuration.venues[index];
    }
  }
  return nullptr;
}

[[nodiscard]] bool visited(const RoutingRequest& request,
                           const common::VenueId venue_id) noexcept {
  for (std::size_t index = 0U; index < request.visited_venue_count; ++index) {
    if (request.visited_venues[index] == venue_id) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool passive_policy(const ExecutionPolicy policy) noexcept {
  return policy == ExecutionPolicy::passive_join ||
         policy == ExecutionPolicy::passive_improve ||
         policy == ExecutionPolicy::participation || policy == ExecutionPolicy::twap ||
         policy == ExecutionPolicy::vwap || policy == ExecutionPolicy::pov ||
         policy == ExecutionPolicy::cancel_replace;
}

[[nodiscard]] bool aggressive_policy(const ExecutionPolicy policy) noexcept {
  return policy == ExecutionPolicy::aggressive_take ||
         policy == ExecutionPolicy::immediate_or_cancel;
}

// Policy dispatch is centralized so every policy shares the same remaining-
// quantity and child-cap invariants.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] SliceResult compute_slice(const RoutingRequest& request) noexcept {
  const auto& objective = request.objective;
  if (request.now_process_monotonic_time_ns <
      objective.start_process_monotonic_time_ns) {
    return {.reason = RoutingReason::objective_not_started};
  }
  if (request.now_process_monotonic_time_ns > objective.end_process_monotonic_time_ns) {
    return {.reason = RoutingReason::objective_expired};
  }
  if (objective.filled_quantity_units >= objective.total_quantity_units) {
    return {.reason = RoutingReason::objective_complete};
  }
  const auto remaining =
      objective.total_quantity_units - objective.filled_quantity_units;
  auto quantity = std::min(remaining, objective.maximum_child_quantity_units);
  switch (objective.policy) {
  case ExecutionPolicy::participation:
  case ExecutionPolicy::pov: {
    if (request.market_volume_since_last_slice_units == 0U) {
      return {.reason = RoutingReason::market_volume_unavailable};
    }
    std::uint64_t target{};
    if (!multiply_divide(request.market_volume_since_last_slice_units,
                         objective.participation_rate_ppm, kRouterPartsPerMillion,
                         target)) {
      return {.reason = RoutingReason::arithmetic_overflow};
    }
    quantity = std::min(quantity, target);
    break;
  }
  case ExecutionPolicy::twap: {
    std::uint64_t next_due{};
    if (request.last_slice_process_monotonic_time_ns != 0U &&
        (!checked_add(request.last_slice_process_monotonic_time_ns,
                      objective.slice_interval_ns, next_due) ||
         request.now_process_monotonic_time_ns < next_due)) {
      return {.reason = RoutingReason::slice_not_due};
    }
    const auto time_remaining =
        objective.end_process_monotonic_time_ns - request.now_process_monotonic_time_ns;
    const auto intervals = time_remaining / objective.slice_interval_ns;
    const auto slice_count = intervals + 1U;
    quantity = std::min(quantity, (remaining / slice_count) +
                                      (remaining % slice_count == 0U ? 0U : 1U));
    break;
  }
  case ExecutionPolicy::vwap: {
    if (request.target_cumulative_volume_curve_ppm == 0U) {
      return {.reason = RoutingReason::market_volume_unavailable};
    }
    std::uint64_t cumulative_target{};
    if (!multiply_divide(objective.total_quantity_units,
                         request.target_cumulative_volume_curve_ppm,
                         kRouterPartsPerMillion, cumulative_target)) {
      return {.reason = RoutingReason::arithmetic_overflow};
    }
    std::uint64_t already_accounted{};
    if (!checked_add(objective.filled_quantity_units,
                     request.already_scheduled_quantity_units, already_accounted)) {
      return {.reason = RoutingReason::arithmetic_overflow};
    }
    if (cumulative_target <= already_accounted) {
      return {.reason = RoutingReason::slice_not_due};
    }
    quantity = std::min(quantity, cumulative_target - already_accounted);
    break;
  }
  case ExecutionPolicy::cancel_replace:
    if (!request.working_order.present ||
        request.working_order.order_id != objective.target_order_id) {
      return {.reason = RoutingReason::invalid_request};
    }
    quantity = std::min(quantity, request.working_order.remaining_quantity_units);
    break;
  case ExecutionPolicy::auction_participation:
  case ExecutionPolicy::passive_join:
  case ExecutionPolicy::passive_improve:
  case ExecutionPolicy::aggressive_take:
  case ExecutionPolicy::immediate_or_cancel:
    break;
  }
  if (quantity == 0U) {
    return {.reason = RoutingReason::slice_not_due};
  }
  return {.reason = RoutingReason::routed, .quantity_units = quantity, .due = true};
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] bool absolute_difference_exceeds(const std::int64_t left,
                                               const std::int64_t right,
                                               const std::uint32_t maximum) noexcept {
  std::int64_t difference{};
  if (__builtin_sub_overflow(left, right, &difference)) {
    return true;
  }
  return signed_magnitude(difference) > maximum;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

[[nodiscard]] bool quote_conflicts(const VenueObservation& observation,
                                   const ConsolidatedQuote& consolidated,
                                   const std::uint32_t maximum_ticks) noexcept {
  const bool bid_conflict =
      observation.venue_id == consolidated.best_bid_venue_id &&
      absolute_difference_exceeds(observation.bid_price_ticks,
                                  consolidated.best_bid_price_ticks, maximum_ticks);
  const bool ask_conflict =
      observation.venue_id == consolidated.best_ask_venue_id &&
      absolute_difference_exceeds(observation.ask_price_ticks,
                                  consolidated.best_ask_price_ticks, maximum_ticks);
  return bid_conflict || ask_conflict;
}

[[nodiscard]] bool proposed_price(const ExecutionObjective& objective,
                                  const VenueObservation& observation,
                                  const bool passive, std::int64_t& output) noexcept {
  if (objective.policy == ExecutionPolicy::auction_participation) {
    output = observation.auction_price_ticks;
  } else if (!passive) {
    output = objective.side == risk::IntentAction::buy ? observation.ask_price_ticks
                                                       : observation.bid_price_ticks;
  } else if (objective.policy == ExecutionPolicy::passive_improve ||
             objective.policy == ExecutionPolicy::cancel_replace) {
    if (objective.side == risk::IntentAction::buy) {
      if (__builtin_add_overflow(observation.bid_price_ticks, 1, &output)) {
        return false;
      }
      output = std::min(output, observation.ask_price_ticks - 1);
    } else {
      if (__builtin_sub_overflow(observation.ask_price_ticks, 1, &output)) {
        return false;
      }
      output = std::max(output, observation.bid_price_ticks + 1);
    }
  } else {
    output = objective.side == risk::IntentAction::buy ? observation.bid_price_ticks
                                                       : observation.ask_price_ticks;
  }
  if (output <= 0) {
    return false;
  }
  return objective.side == risk::IntentAction::buy
             ? output <= objective.limit_price_ticks
             : output >= objective.limit_price_ticks;
}

[[nodiscard]] bool effective_fill_probability(const ExecutionObjective& objective,
                                              const VenueObservation& observation,
                                              const std::uint64_t proposed_quantity,
                                              const bool passive,
                                              std::uint32_t& output) noexcept {
  std::uint64_t displayed = objective.side == risk::IntentAction::buy
                                ? observation.bid_quantity_units
                                : observation.ask_quantity_units;
  if (!passive) {
    displayed = objective.side == risk::IntentAction::buy
                    ? observation.ask_quantity_units
                    : observation.bid_quantity_units;
  }
  std::uint64_t liquidity{};
  if (!checked_add(displayed, observation.estimated_hidden_quantity_units, liquidity)) {
    return false;
  }
  std::uint64_t denominator{};
  if (!checked_add(liquidity, passive ? observation.queue_ahead_quantity_units : 0U,
                   denominator)) {
    return false;
  }
  std::uint64_t queue_factor = kRouterPartsPerMillion;
  if (denominator != 0U &&
      !multiply_divide(liquidity, kRouterPartsPerMillion, denominator, queue_factor)) {
    return false;
  }
  if (denominator == 0U) {
    queue_factor = 0U;
  }
  std::uint64_t coverage_factor = kRouterPartsPerMillion;
  if (proposed_quantity != 0U && liquidity < proposed_quantity &&
      !multiply_divide(liquidity, kRouterPartsPerMillion, proposed_quantity,
                       coverage_factor)) {
    return false;
  }
  std::uint64_t probability{};
  if (!multiply_divide(observation.fill_probability_ppm,
                       kRouterPartsPerMillion - observation.reject_rate_ppm,
                       kRouterPartsPerMillion, probability) ||
      !multiply_divide(probability, queue_factor, kRouterPartsPerMillion,
                       probability) ||
      !multiply_divide(probability, coverage_factor, kRouterPartsPerMillion,
                       probability) ||
      probability > kRouterPartsPerMillion) {
    return false;
  }
  output = static_cast<std::uint32_t>(probability);
  return true;
}

[[nodiscard]] ValueResult calculate_value(const RoutingRequest& request,
                                          const RouterConfiguration& configuration,
                                          const VenueObservation& observation,
                                          const std::uint64_t proposed_quantity,
                                          const bool passive,
                                          const std::int64_t price_ticks) noexcept {
  ValueResult result;
  std::uint64_t elapsed{};
  if (!checked_add(request.now_process_monotonic_time_ns -
                       request.objective.start_process_monotonic_time_ns,
                   observation.venue_latency_ns, elapsed)) {
    return result;
  }
  result.decayed_alpha_microticks =
      decay_alpha_microticks(request.objective.expected_alpha_microticks, elapsed,
                             request.objective.alpha_half_life_ns);
  if (!effective_fill_probability(request.objective, observation, proposed_quantity,
                                  passive, result.effective_fill_probability_ppm)) {
    return result;
  }
  result.uncertainty_ppm = fill_uncertainty_ppm(result.effective_fill_probability_ppm);

  std::int64_t alpha_nanos{};
  std::int64_t adverse_nanos{};
  if (!microticks_to_currency_nanos(result.decayed_alpha_microticks,
                                    request.objective.tick_value_currency_nanos,
                                    alpha_nanos) ||
      !microticks_to_currency_nanos(observation.adverse_selection_microticks,
                                    request.objective.tick_value_currency_nanos,
                                    adverse_nanos)) {
    return result;
  }
  std::int64_t reference_price{};
  if (passive) {
    reference_price = request.objective.side == risk::IntentAction::buy
                          ? request.consolidated_quote.best_bid_price_ticks
                          : request.consolidated_quote.best_ask_price_ticks;
  } else {
    reference_price = request.objective.side == risk::IntentAction::buy
                          ? request.consolidated_quote.best_ask_price_ticks
                          : request.consolidated_quote.best_bid_price_ticks;
  }
  std::int64_t price_edge_ticks{};
  if (request.objective.side == risk::IntentAction::buy) {
    if (!checked_sub_signed(reference_price, price_ticks, price_edge_ticks)) {
      return result;
    }
  } else if (!checked_sub_signed(price_ticks, reference_price, price_edge_ticks)) {
    return result;
  }
  std::int64_t price_edge_nanos{};
  if (!ticks_to_currency_nanos(price_edge_ticks,
                               request.objective.tick_value_currency_nanos,
                               price_edge_nanos)) {
    return result;
  }
  const auto fee = passive ? observation.maker_fee_currency_nanos_per_unit
                           : observation.taker_fee_currency_nanos_per_unit;
  std::int64_t gross{};
  if (!checked_add_signed(alpha_nanos, price_edge_nanos, gross) ||
      !checked_sub_signed(gross, adverse_nanos, gross) ||
      !checked_sub_signed(gross, fee, gross)) {
    return result;
  }
  std::int64_t expected_execution{};
  if (!signed_multiply_divide(gross, result.effective_fill_probability_ppm,
                              kRouterPartsPerMillion, expected_execution)) {
    return result;
  }
  const auto positive_alpha =
      alpha_nanos > 0 ? static_cast<std::uint64_t>(alpha_nanos) : 0U;
  if (!multiply_divide(positive_alpha,
                       kRouterPartsPerMillion - result.effective_fill_probability_ppm,
                       kRouterPartsPerMillion,
                       result.opportunity_cost_currency_nanos_per_unit)) {
    return result;
  }
  std::uint64_t uncertainty_penalty{};
  if (!multiply_divide(configuration.uncertainty_penalty_currency_nanos_per_unit,
                       result.uncertainty_ppm, kRouterPartsPerMillion,
                       uncertainty_penalty) ||
      uncertainty_penalty >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      result.opportunity_cost_currency_nanos_per_unit >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return result;
  }
  if (!checked_sub_signed(
          expected_execution,
          static_cast<std::int64_t>(result.opportunity_cost_currency_nanos_per_unit),
          result.expected_value_currency_nanos_per_unit) ||
      !checked_sub_signed(result.expected_value_currency_nanos_per_unit,
                          static_cast<std::int64_t>(uncertainty_penalty),
                          result.expected_value_currency_nanos_per_unit)) {
    return result;
  }
  result.valid = true;
  return result;
}

[[nodiscard]] bool better_score(const VenueScoreExplanation& candidate,
                                const VenueScoreExplanation& current) noexcept {
  if (candidate.selected_expected_value_currency_nanos_per_unit !=
      current.selected_expected_value_currency_nanos_per_unit) {
    return candidate.selected_expected_value_currency_nanos_per_unit >
           current.selected_expected_value_currency_nanos_per_unit;
  }
  if (candidate.tie_break_rank != current.tie_break_rank) {
    return candidate.tie_break_rank < current.tie_break_rank;
  }
  return candidate.venue_id < current.venue_id;
}

[[nodiscard]] RoutingDecision base_decision(const RoutingRequest& request,
                                            const RouterConfiguration& configuration,
                                            const RoutingStatus status,
                                            const RoutingReason reason) noexcept {
  RoutingDecision result{.status = status,
                         .reason = reason,
                         .objective_id = request.objective.objective_id,
                         .venue_id = {},
                         .target_order_id = {},
                         .configuration_version =
                             request.objective.configuration_version,
                         .route_attempt = request.route_attempt,
                         .source_objective_hash = request.objective.stable_hash,
                         .source_request_hash = request.stable_hash,
                         .configuration_hash = configuration.stable_hash,
                         .requires_fresh_pretrade_risk = true};
  result.stable_hash = stable_routing_decision_hash(result);
  return result;
}

} // namespace

SmartOrderRouter::SmartOrderRouter(RouterConfiguration configuration) noexcept
    : configuration_(configuration) {
  initialized_ = valid_router_configuration(configuration_);
  health_ = initialized_ ? RouterHealth::healthy : RouterHealth::unsafe;
}

bool SmartOrderRouter::initialized() const noexcept { return initialized_; }

bool SmartOrderRouter::ready() const noexcept {
  return initialized_ && health_ == RouterHealth::healthy;
}

RouterHealth SmartOrderRouter::health() const noexcept { return health_; }

const common::BuildInfo& SmartOrderRouter::build_info() noexcept {
  return common::current_build_info();
}

std::uint64_t SmartOrderRouter::configuration_hash() const noexcept {
  return configuration_.stable_hash;
}

// Hard eligibility is deliberately visible in normative evaluation order.
// Splitting it across polymorphic rules would obscure fail-closed precedence.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
RoutingDecision SmartOrderRouter::route(const RoutingRequest& request) noexcept {
  ++metrics_.requests;
  if (!initialized_) {
    ++metrics_.invalid;
    return base_decision(request, configuration_, RoutingStatus::invalid_configuration,
                         RoutingReason::invalid_configuration);
  }
  if (health_ == RouterHealth::stopped) {
    ++metrics_.invalid;
    return base_decision(request, configuration_, RoutingStatus::stopped,
                         RoutingReason::stopped);
  }
  if (!valid_routing_request(request) ||
      request.objective.configuration_version != configuration_.configuration_version) {
    ++metrics_.invalid;
    return base_decision(request, configuration_, RoutingStatus::invalid_request,
                         valid_execution_objective(request.objective)
                             ? RoutingReason::invalid_request
                             : RoutingReason::invalid_objective);
  }
  if (request.route_attempt >= configuration_.maximum_route_attempts) {
    ++metrics_.abstained;
    ++metrics_.loop_exclusions;
    return base_decision(request, configuration_, RoutingStatus::abstained,
                         RoutingReason::routing_loop);
  }
  const auto slice = compute_slice(request);
  if (!slice.due) {
    ++metrics_.abstained;
    return base_decision(request, configuration_, RoutingStatus::abstained,
                         slice.reason);
  }

  auto decision = base_decision(request, configuration_, RoutingStatus::abstained,
                                RoutingReason::no_eligible_venue);
  decision.evaluated_venue_count = request.venue_count;
  std::size_t selected_index = decision.venue_scores.size();
  bool all_excluded_by_loop = request.venue_count != 0U;
  std::uint64_t consolidated_valid_until{};
  const bool consolidated_fresh =
      request.consolidated_quote.observed_process_monotonic_time_ns <=
          request.now_process_monotonic_time_ns &&
      checked_add(request.consolidated_quote.observed_process_monotonic_time_ns,
                  configuration_.maximum_quote_age_ns, consolidated_valid_until) &&
      consolidated_valid_until >= request.now_process_monotonic_time_ns;

  for (std::size_t index = 0U; index < request.venue_count; ++index) {
    const auto& observation = request.venues[index];
    auto& explanation = decision.venue_scores[index];
    explanation.venue_id = observation.venue_id;
    explanation.observation_hash = observation.stable_hash;
    const auto* rule = find_rule(configuration_, observation.venue_id);
    if (rule == nullptr || !rule->enabled) {
      explanation.eligibility = VenueEligibilityReason::disabled;
      all_excluded_by_loop = false;
      continue;
    }
    explanation.tie_break_rank = rule->tie_break_rank;
    if ((rule->allowed_policy_mask & policy_bit(request.objective.policy)) == 0U ||
        (request.objective.policy == ExecutionPolicy::auction_participation &&
         !rule->allow_auction)) {
      explanation.eligibility = VenueEligibilityReason::policy_not_allowed;
      all_excluded_by_loop = false;
      continue;
    }
    if (visited(request, observation.venue_id)) {
      explanation.eligibility = VenueEligibilityReason::already_visited;
      ++metrics_.loop_exclusions;
      continue;
    }
    all_excluded_by_loop = false;
    if (!rule->regulatory_authorized || !observation.regulatory_eligible) {
      explanation.eligibility = VenueEligibilityReason::regulatory_ineligible;
      continue;
    }
    if (rule->require_self_trade_clear &&
        observation.self_trade_prevention != SelfTradePreventionState::clear) {
      explanation.eligibility = VenueEligibilityReason::self_trade_conflict;
      ++metrics_.self_trade_exclusions;
      continue;
    }
    const auto required_state =
        request.objective.policy == ExecutionPolicy::auction_participation
            ? VenueTradingState::auction
            : VenueTradingState::open;
    if (observation.trading_state != required_state) {
      explanation.eligibility = VenueEligibilityReason::trading_state_ineligible;
      ++metrics_.halted_venue_exclusions;
      continue;
    }
    if (!observation.quote_valid) {
      explanation.eligibility = VenueEligibilityReason::quote_invalid;
      continue;
    }
    if (!consolidated_fresh) {
      explanation.eligibility = VenueEligibilityReason::quote_stale;
      ++metrics_.stale_quote_exclusions;
      continue;
    }
    std::uint64_t quote_valid_until{};
    if (!checked_add(observation.observed_process_monotonic_time_ns,
                     configuration_.maximum_quote_age_ns, quote_valid_until) ||
        observation.observed_process_monotonic_time_ns >
            request.now_process_monotonic_time_ns ||
        quote_valid_until < request.now_process_monotonic_time_ns) {
      explanation.eligibility = VenueEligibilityReason::quote_stale;
      ++metrics_.stale_quote_exclusions;
      continue;
    }
    if (observation.venue_latency_ns > configuration_.maximum_route_latency_ns) {
      explanation.eligibility = VenueEligibilityReason::quote_stale;
      ++metrics_.stale_quote_exclusions;
      continue;
    }
    if (quote_conflicts(observation, request.consolidated_quote,
                        configuration_.maximum_direct_consolidated_divergence_ticks)) {
      explanation.eligibility = VenueEligibilityReason::direct_consolidated_conflict;
      continue;
    }
    if (observation.reject_rate_ppm > configuration_.maximum_reject_rate_ppm) {
      explanation.eligibility = VenueEligibilityReason::reject_burst;
      ++metrics_.reject_burst_exclusions;
      continue;
    }
    if (observation.fill_probability_ppm <
        configuration_.minimum_fill_probability_ppm) {
      explanation.eligibility = VenueEligibilityReason::fill_probability_too_low;
      continue;
    }
    if (request.objective.policy == ExecutionPolicy::cancel_replace &&
        observation.venue_id != request.working_order.venue_id) {
      explanation.eligibility = VenueEligibilityReason::working_order_venue_mismatch;
      continue;
    }

    std::uint64_t concentration_limit{};
    if (!multiply_divide_ceil(request.objective.total_quantity_units,
                              rule->maximum_concentration_ppm, kRouterPartsPerMillion,
                              concentration_limit)) {
      explanation.eligibility = VenueEligibilityReason::arithmetic_overflow;
      continue;
    }
    if (observation.already_routed_quantity_units >= concentration_limit) {
      explanation.eligibility = VenueEligibilityReason::concentration_exhausted;
      ++metrics_.concentration_exclusions;
      continue;
    }
    auto proposed_quantity =
        std::min({slice.quantity_units, rule->maximum_child_quantity_units,
                  concentration_limit - observation.already_routed_quantity_units});
    if (request.objective.policy == ExecutionPolicy::auction_participation) {
      std::uint64_t auction_cap{};
      if (observation.auction_paired_quantity_units == 0U ||
          observation.auction_price_ticks <= 0 ||
          !multiply_divide(observation.auction_paired_quantity_units,
                           request.objective.participation_rate_ppm,
                           kRouterPartsPerMillion, auction_cap)) {
        explanation.eligibility = VenueEligibilityReason::price_unavailable;
        continue;
      }
      proposed_quantity = std::min(proposed_quantity, auction_cap);
    }
    if (proposed_quantity == 0U) {
      explanation.eligibility = VenueEligibilityReason::concentration_exhausted;
      ++metrics_.concentration_exclusions;
      continue;
    }

    const bool use_passive = passive_policy(request.objective.policy);
    std::int64_t selected_price{};
    const bool selected_price_valid =
        proposed_price(request.objective, observation, use_passive, selected_price);
    if (!selected_price_valid) {
      if (request.objective.policy == ExecutionPolicy::cancel_replace) {
        selected_price = request.working_order.price_ticks;
      } else {
        explanation.eligibility = VenueEligibilityReason::price_unavailable;
        continue;
      }
    }
    explanation.proposed_price_ticks = selected_price;
    explanation.proposed_quantity_units = proposed_quantity;

    std::int64_t passive_price{};
    std::int64_t aggressive_price{};
    const bool passive_price_valid =
        proposed_price(request.objective, observation, true, passive_price);
    const bool aggressive_price_valid =
        proposed_price(request.objective, observation, false, aggressive_price);
    const auto passive = passive_price_valid
                             ? calculate_value(request, configuration_, observation,
                                               proposed_quantity, true, passive_price)
                             : ValueResult{};
    const auto aggressive =
        aggressive_price_valid
            ? calculate_value(request, configuration_, observation, proposed_quantity,
                              false, aggressive_price)
            : ValueResult{};
    if ((use_passive && !passive.valid && selected_price_valid) ||
        (aggressive_policy(request.objective.policy) && !aggressive.valid) ||
        (request.objective.policy == ExecutionPolicy::auction_participation &&
         !aggressive.valid)) {
      explanation.eligibility = VenueEligibilityReason::arithmetic_overflow;
      continue;
    }
    explanation.passive_expected_value_currency_nanos_per_unit =
        passive.expected_value_currency_nanos_per_unit;
    explanation.aggressive_expected_value_currency_nanos_per_unit =
        aggressive.expected_value_currency_nanos_per_unit;
    const auto& selected_value = use_passive ? passive : aggressive;
    explanation.selected_expected_value_currency_nanos_per_unit =
        selected_price_valid ? selected_value.expected_value_currency_nanos_per_unit
                             : std::numeric_limits<std::int64_t>::min() + 1;
    explanation.decayed_alpha_microticks = selected_value.decayed_alpha_microticks;
    explanation.opportunity_cost_currency_nanos_per_unit =
        selected_value.opportunity_cost_currency_nanos_per_unit;
    explanation.effective_fill_probability_ppm =
        selected_price_valid ? selected_value.effective_fill_probability_ppm : 0U;
    explanation.fill_uncertainty_ppm = selected_value.uncertainty_ppm;
    explanation.eligibility = VenueEligibilityReason::eligible;
    ++decision.eligible_venue_count;
    if (selected_index == decision.venue_scores.size() ||
        better_score(explanation, decision.venue_scores[selected_index])) {
      selected_index = index;
    }
  }

  if (selected_index == decision.venue_scores.size()) {
    decision.reason = all_excluded_by_loop ? RoutingReason::routing_loop
                                           : RoutingReason::no_eligible_venue;
    decision.stable_hash = stable_routing_decision_hash(decision);
    ++metrics_.abstained;
    return decision;
  }

  const auto& selected = decision.venue_scores[selected_index];
  decision.status = RoutingStatus::routed;
  decision.reason = RoutingReason::routed;
  decision.venue_id = selected.venue_id;
  decision.price_ticks = selected.proposed_price_ticks;
  decision.quantity_units = selected.proposed_quantity_units;
  decision.expected_value_currency_nanos_per_unit =
      selected.selected_expected_value_currency_nanos_per_unit;
  decision.action = RoutedAction::new_order;
  decision.time_in_force = RoutedTimeInForce::day;
  if (request.objective.policy == ExecutionPolicy::immediate_or_cancel) {
    decision.time_in_force = RoutedTimeInForce::immediate_or_cancel;
  } else if (request.objective.policy == ExecutionPolicy::auction_participation) {
    decision.time_in_force = RoutedTimeInForce::auction;
  } else if (request.objective.policy == ExecutionPolicy::cancel_replace) {
    decision.target_order_id = request.working_order.order_id;
    std::uint64_t threshold{};
    const bool threshold_valid =
        checked_add(request.working_order.initial_queue_ahead_quantity_units,
                    configuration_.queue_deterioration_threshold_units, threshold);
    const bool deteriorated =
        threshold_valid &&
        request.venues[selected_index].queue_ahead_quantity_units > threshold;
    if (!deteriorated) {
      decision.status = RoutingStatus::abstained;
      decision.reason = RoutingReason::slice_not_due;
      decision.venue_id = {};
      decision.target_order_id = {};
      decision.price_ticks = 0;
      decision.quantity_units = 0U;
      ++metrics_.abstained;
      decision.stable_hash = stable_routing_decision_hash(decision);
      return decision;
    }
    if (decision.price_ticks != request.working_order.price_ticks) {
      decision.action = RoutedAction::replace;
      decision.reason = RoutingReason::routed;
    } else {
      decision.action = RoutedAction::cancel;
      decision.price_ticks = request.working_order.price_ticks;
    }
  }
  decision.stable_hash = stable_routing_decision_hash(decision);
  ++metrics_.routed;
  return decision;
}

RouterMetrics SmartOrderRouter::metrics() const noexcept { return metrics_; }

void SmartOrderRouter::shutdown() noexcept { health_ = RouterHealth::stopped; }

} // namespace aegis::execution
