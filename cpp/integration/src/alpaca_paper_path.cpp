#include "aegis/integration/alpaca_paper_path.hpp"

#include "aegis/common/sha256.hpp"
#include "aegis/execution/router.hpp"
#include "aegis/execution/simulated_gateway.hpp"
#include "aegis/oms/service.hpp"
#include "aegis/risk/engine.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace aegis::integration {
namespace {

inline constexpr std::uint64_t kNow = 1'000'000U;
inline constexpr common::SessionId kSession{0xA11A'CA00U, 1U};
inline constexpr common::AccountId kAccount{0xA11A'CA00U, 2U};
inline constexpr common::StrategyId kStrategy{0xA11A'CA00U, 3U};
inline constexpr common::VenueId kVenue{0xA11A'CA00U, 4U};
inline constexpr common::ConfigurationVersion kConfiguration{0xA11A'CA00U, 6U};
inline constexpr common::RiskSnapshotId kRiskSnapshot{0xA11A'CA00U, 7U};
inline constexpr oms::LeaderAuthority kAuthority{.exchange_session_epoch = 77U,
                                                 .fencing_token = 88U};
inline constexpr std::uint32_t kAllPolicyMask =
    (std::uint32_t{1U} << static_cast<std::uint8_t>(
         execution::ExecutionPolicy::auction_participation)) -
    1U;
inline constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

template <typename Value> void mix(std::uint64_t& hash, const Value value) noexcept {
  const auto bytes = std::bit_cast<std::array<std::uint8_t, sizeof(Value)>>(value);
  for (const auto byte : bytes) {
    hash ^= byte;
    hash *= kFnvPrime;
  }
}

void mix_chars(std::uint64_t& hash, const std::span<const char> value) noexcept {
  for (const auto character : value) {
    mix(hash, static_cast<std::uint8_t>(character));
  }
}

[[nodiscard]] std::uint64_t nonzero_hash(const std::uint64_t value) noexcept {
  return value == 0U ? 1U : value;
}

[[nodiscard]] common::Sha256Digest seed_digest(const std::uint64_t seed) noexcept {
  const auto bytes = std::bit_cast<std::array<std::uint8_t, sizeof(seed)>>(seed);
  return common::sha256(bytes);
}

[[nodiscard]] common::InstrumentId
certification_instrument_id(const std::string_view symbol) noexcept {
  constexpr std::string_view prefix = "AEGIS-ALPACA-PAPER-INSTRUMENT-V1:";
  std::array<std::uint8_t, prefix.size() + kAlpacaPaperSymbolBytes> material{};
  std::size_t offset = 0U;
  for (const auto character : prefix) {
    material[offset++] = static_cast<std::uint8_t>(character);
  }
  for (const auto character : symbol) {
    material[offset++] = static_cast<std::uint8_t>(character);
  }
  const auto digest =
      common::sha256(std::span<const std::uint8_t>{material.data(), offset});
  const auto word = [&digest](const std::size_t start) noexcept {
    std::uint64_t value = 0U;
    for (std::size_t index = start; index < start + 8U; ++index) {
      value = (value << 8U) | digest[index];
    }
    return value;
  };
  return {word(0U), word(8U)};
}

[[nodiscard]] market_state::MarketStateSnapshot market_snapshot() noexcept {
  market_state::MarketStateSnapshot snapshot{
      .state = market_state::MarketState::normal,
      .primary_reason = market_state::MarketStateReason::normal_conditions_stable,
      .reason_mask = market_state::reason_bit(
          market_state::MarketStateReason::normal_conditions_stable),
      .input_sequence = 1U,
      .transition_sequence = 1U,
      .observed_process_monotonic_time_ns = kNow - 10U,
      .observed_wall_clock_utc_ns = 1'800'000'000'000'000'000ULL,
      .state_entered_process_monotonic_time_ns = kNow - 100U};
  snapshot.stable_hash = market_state::stable_market_state_hash(snapshot);
  return snapshot;
}

[[nodiscard]] time::ClockQualitySnapshot clock_snapshot() noexcept {
  return {.state = time::ClockQualityState::healthy,
          .reason = time::ClockQualityReason::within_thresholds,
          .operation_mode = time::ClockOperationMode::normal,
          .observed_at = time::MonotonicTimeNs{kNow - 10U},
          .ptp_offset = time::DurationNs{10},
          .drift_ppb = 1,
          .source_id = time::ClockSourceId{12U, 12U},
          .last_synchronization_time = time::MonotonicTimeNs{kNow - 20U},
          .synchronization_age = time::DurationNs{20},
          .hardware_timestamp_available = true,
          .transition_count = 1U,
          .observation_count = 2U};
}

[[nodiscard]] execution::RouterConfiguration router_configuration() noexcept {
  execution::RouterConfiguration result{
      .configuration_version = kConfiguration,
      .maximum_quote_age_ns = 1'000U,
      .maximum_route_latency_ns = 1'000U,
      .queue_deterioration_threshold_units = 1U,
      .uncertainty_penalty_currency_nanos_per_unit = 1U,
      .maximum_reject_rate_ppm = 10'000U,
      .minimum_fill_probability_ppm = 1U,
      .maximum_direct_consolidated_divergence_ticks = 0U,
      .maximum_route_attempts = 1U,
      .venue_count = 1U};
  result.venues[0U] = {.venue_id = kVenue,
                       .allowed_policy_mask = kAllPolicyMask,
                       .maximum_concentration_ppm = execution::kRouterPartsPerMillion,
                       .maximum_child_quantity_units = 1U,
                       .tie_break_rank = 1U,
                       .enabled = true,
                       .regulatory_authorized = true,
                       .require_self_trade_clear = true,
                       .allow_auction = false};
  result.stable_hash = execution::stable_router_configuration_hash(result);
  return result;
}

[[nodiscard]] execution::RoutingRequest
routing_request(const common::InstrumentId instrument_id) noexcept {
  execution::ExecutionObjective objective{
      .objective_id = common::IntentId{100U, 1U},
      .session_id = kSession,
      .account_id = kAccount,
      .strategy_id = kStrategy,
      .instrument_id = instrument_id,
      .source_forecast_id = common::ForecastId{101U, 1U},
      .feature_snapshot_id = common::FeatureSnapshotId{102U, 1U},
      .configuration_version = kConfiguration,
      .target_order_id = {},
      .side = risk::IntentAction::buy,
      .policy = execution::ExecutionPolicy::passive_join,
      .limit_price_ticks = 1,
      .total_quantity_units = 1U,
      .filled_quantity_units = 0U,
      .maximum_child_quantity_units = 1U,
      .start_process_monotonic_time_ns = kNow - 100U,
      .end_process_monotonic_time_ns = kNow + 10'000U,
      .slice_interval_ns = 100U,
      .alpha_half_life_ns = 10'000U,
      .expected_alpha_microticks = 1,
      .tick_value_currency_nanos = kAlpacaPaperPriceIncrementCurrencyNanos,
      .participation_rate_ppm = 0U,
      .has_limit_price = true};
  objective.stable_hash = execution::stable_execution_objective_hash(objective);
  execution::ConsolidatedQuote quote{.best_bid_venue_id = kVenue,
                                     .best_ask_venue_id = kVenue,
                                     .best_bid_price_ticks = 1,
                                     .best_ask_price_ticks = 2,
                                     .observed_process_monotonic_time_ns = kNow - 1U,
                                     .valid = true};
  quote.stable_hash = execution::stable_consolidated_quote_hash(quote);
  execution::VenueObservation venue{.venue_id = kVenue,
                                    .instrument_id = instrument_id,
                                    .trading_state = execution::VenueTradingState::open,
                                    .self_trade_prevention =
                                        execution::SelfTradePreventionState::clear,
                                    .bid_price_ticks = 1,
                                    .ask_price_ticks = 2,
                                    .bid_quantity_units = 1U,
                                    .ask_quantity_units = 1U,
                                    .estimated_hidden_quantity_units = 0U,
                                    .queue_ahead_quantity_units = 1U,
                                    .venue_latency_ns = 1U,
                                    .observed_process_monotonic_time_ns = kNow - 1U,
                                    .already_routed_quantity_units = 0U,
                                    .auction_paired_quantity_units = 0U,
                                    .auction_price_ticks = 0,
                                    .maker_fee_currency_nanos_per_unit = 0,
                                    .taker_fee_currency_nanos_per_unit = 0,
                                    .adverse_selection_microticks = 0,
                                    .fill_probability_ppm = 1U,
                                    .reject_rate_ppm = 0U,
                                    .quote_valid = true,
                                    .regulatory_eligible = true};
  venue.stable_hash = execution::stable_venue_observation_hash(venue);
  execution::RoutingRequest result{.objective = objective,
                                   .consolidated_quote = quote,
                                   .working_order = {},
                                   .now_process_monotonic_time_ns = kNow,
                                   .last_slice_process_monotonic_time_ns = 0U,
                                   .market_volume_since_last_slice_units = 1U,
                                   .already_scheduled_quantity_units = 0U,
                                   .target_cumulative_volume_curve_ppm = 0U,
                                   .venue_count = 1U,
                                   .visited_venue_count = 0U,
                                   .route_attempt = 0U};
  result.venues[0U] = venue;
  result.stable_hash = execution::stable_routing_request_hash(result);
  return result;
}

[[nodiscard]] risk::RiskLimitSnapshot
risk_limits(const common::InstrumentId instrument_id) noexcept {
  risk::RiskLimitSnapshot result{
      .risk_snapshot_id = kRiskSnapshot,
      .configuration_version = kConfiguration,
      .policy_version = common::ConfigurationVersion{0xA11A'CA00U, 8U},
      .session_id = kSession,
      .account_id = kAccount,
      .revision = 1U,
      .authority_epoch = 10U,
      .published_process_monotonic_time_ns = kNow - 1'000U,
      .valid_until_process_monotonic_time_ns = kNow + 1'000'000U,
      .approval_ttl_ns = 100'000U,
      .maximum_position_age_ns = 10'000U,
      .maximum_market_state_age_ns = 1'000U,
      .maximum_clock_state_age_ns = 1'000U,
      .maximum_feed_book_age_ns = 1'000U,
      .maximum_configuration_age_ns = 10'000U,
      .order_rate_window_ns = 1'000U,
      .cancel_rate_window_ns = 1'000U,
      .maximum_orders_per_window = 1U,
      .maximum_cancels_per_window = 1U,
      .allowed_market_state_mask =
          static_cast<std::uint16_t>(std::uint16_t{1U} << static_cast<std::uint8_t>(
                                         market_state::MarketState::normal)),
      .maximum_gross_exposure_currency_nanos = 100'000'000U,
      .maximum_absolute_net_exposure_currency_nanos = 100'000'000U,
      .maximum_daily_loss_currency_nanos = 100'000'000U,
      .maximum_drawdown_currency_nanos = 100'000'000U,
      .credit_capital_limit_currency_nanos = 100'000'000U,
      .symbol_count = 1U,
      .strategy_count = 1U,
      .venue_count = 1U,
      .sector_count = 1U,
      .factor_count = 1U,
      .allow_simulation = false,
      .allow_paper = true,
      .require_locate_for_short_sale = true,
      .require_self_trade_prevention = true};
  result.maximum_sector_exposure_currency_nanos[0U] = 100'000'000U;
  result.maximum_factor_exposure_currency_nanos[0U] = 100'000'000U;
  result.symbols[0U] = {.instrument_id = instrument_id,
                        .tick_value_currency_nanos =
                            kAlpacaPaperPriceIncrementCurrencyNanos,
                        .price_increment_ticks = 1U,
                        .maximum_order_quantity_units = 1U,
                        .maximum_order_notional_currency_nanos = 10'000'000U,
                        .maximum_price_deviation_ticks = 1U,
                        .maximum_absolute_position_units = 1U,
                        .sector_index = 0U,
                        .factor_beta_ppm = {1'000'000},
                        .authorized = true,
                        .restricted = false};
  result.strategies[0U] = {.strategy_id = kStrategy,
                           .maximum_loss_currency_nanos = 100'000'000U,
                           .maximum_drawdown_currency_nanos = 100'000'000U,
                           .authorized = true};
  result.venues[0U] = {.venue_id = kVenue, .authorized = true};
  result.stable_hash = risk::stable_limit_snapshot_hash(result);
  return result;
}

[[nodiscard]] risk::RiskEvaluationRequest
risk_request(const execution::RoutingDecision& route,
             const common::InstrumentId instrument_id,
             const std::uint64_t seed) noexcept {
  risk::RiskIntent intent{.intent_id = route.objective_id,
                          .session_id = kSession,
                          .account_id = kAccount,
                          .strategy_id = kStrategy,
                          .venue_id = route.venue_id,
                          .instrument_id = instrument_id,
                          .source_forecast_id = common::ForecastId{101U, 1U},
                          .feature_snapshot_id = common::FeatureSnapshotId{102U, 1U},
                          .target_order_id = route.target_order_id,
                          .configuration_version = kConfiguration,
                          .action = risk::IntentAction::buy,
                          .limit_price_ticks = route.price_ticks,
                          .quantity_units = route.quantity_units,
                          .created_process_monotonic_time_ns = kNow,
                          .expire_process_monotonic_time_ns = kNow + 100'000U,
                          .canonical_sha256 = seed_digest(seed)};
  intent.stable_hash = risk::stable_risk_intent_hash(intent);
  risk::RiskContext context{.now_process_monotonic_time_ns = kNow + 10U,
                            .trading_mode = risk::TradingMode::paper,
                            .operator_authorized = true,
                            .session_authorized = true,
                            .operator_authorization_valid_until_ns = kNow + 100'000U,
                            .authority_epoch = 10U,
                            .process_id = 11U,
                            .market_state_snapshot = market_snapshot(),
                            .official_trading_status =
                                market_state::OfficialTradingStatus::open,
                            .feed_health = market_state::FeedHealth::healthy,
                            .book_validity = market_state::BookValidity::valid,
                            .clock_quality_snapshot = clock_snapshot(),
                            .feed_book_observed_process_monotonic_time_ns = kNow - 1U,
                            .feed_book_state_hash = 13U,
                            .reference_price_ticks = 1,
                            .short_sale_locate = risk::PolicyHookResult::allowed,
                            .self_trade_prevention = risk::PolicyHookResult::allowed};
  context.stable_hash = risk::stable_risk_context_hash(context);
  return {.global_event_id = common::GlobalEventId{140U, 1U},
          .intent = intent,
          .context = context};
}

[[nodiscard]] oms::OmsConfiguration oms_configuration() noexcept {
  oms::OmsConfiguration result{.session_id = kSession,
                               .account_id = kAccount,
                               .configuration_version = kConfiguration,
                               .trading_mode = risk::TradingMode::paper,
                               .exchange_session_epoch =
                                   kAuthority.exchange_session_epoch,
                               .initial_fencing_token = kAuthority.fencing_token,
                               .maximum_order_age_ns = 1'000'000U,
                               .require_drop_copy = false};
  result.stable_hash = oms::stable_configuration_hash(result);
  return result;
}

[[nodiscard]] execution::GatewayConfiguration
gateway_configuration(const common::InstrumentId instrument_id,
                      const std::uint64_t seed) noexcept {
  execution::GatewayConfiguration result{
      .session_id = kSession,
      .account_id = kAccount,
      .venue_id = kVenue,
      .configuration_version = kConfiguration,
      .startup_mode = execution::GatewayMode::paper,
      .exchange_session_epoch = kAuthority.exchange_session_epoch,
      .initial_fencing_token = kAuthority.fencing_token,
      .heartbeat_interval_ns = 100'000U,
      .heartbeat_timeout_ns = 500'000U,
      .maximum_clock_age_ns = 1'000U,
      .command_rate_window_ns = 1'000U,
      .maximum_new_orders_per_window = 1U,
      .maximum_cancels_per_window = 1U,
      .maximum_replaces_per_window = 1U,
      .mapping_count = 1U,
      .paper_model = {.deterministic_seed = seed,
                      .acknowledgement_latency_ns = 100U,
                      .cancel_latency_ns = 100U,
                      .replace_latency_ns = 100U,
                      .initial_queue_ahead_units = 1U,
                      .maximum_fill_chunk_units = 1U,
                      .fee_per_unit_currency_nanos = 0U,
                      .slippage_ticks = 0U,
                      .impact_ticks_per_million_units = 0U,
                      .reject_every_nth_new_order = 0U,
                      .allow_auction_orders = false}};
  result.mappings[0U] = {.venue_id = kVenue,
                         .instrument_id = instrument_id,
                         .synthetic_venue_number = 1U,
                         .synthetic_instrument_number = 1U};
  result.stable_hash = execution::stable_gateway_configuration_hash(result);
  return result;
}

[[nodiscard]] oms::OmsInput oms_internal(const oms::InputKind kind,
                                         const common::OrderId order_id,
                                         const std::uint64_t ordinal) noexcept {
  oms::OmsInput result{.receipt_id = common::GlobalEventId{160U, ordinal},
                       .kind = kind,
                       .source = oms::EventSource::internal,
                       .order_id = order_id,
                       .authority = kAuthority,
                       .process_monotonic_time_ns = kNow + 11U + ordinal};
  result.stable_hash = oms::stable_input_hash(result);
  return result;
}

[[nodiscard]] execution::FinalSafetyState
final_safety(const execution::GatewayConfiguration& configuration,
             const risk::RiskContext& context) noexcept {
  execution::FinalSafetyState result{
      .observed_process_monotonic_time_ns = kNow + 20U,
      .market_state_snapshot = context.market_state_snapshot,
      .official_trading_status = context.official_trading_status,
      .feed_health = context.feed_health,
      .book_validity = context.book_validity,
      .clock_quality_snapshot = context.clock_quality_snapshot,
      .authority = kAuthority,
      .effective_configuration_hash = configuration.stable_hash,
      .activation_record_hash = 0U,
      .operator_authorization_valid_until_ns = 0U,
      .signed_configuration_valid = false,
      .operator_authorized = false,
      .activation_record_durable = false,
      .kill_switch_engaged = false,
      .journal_ready = true};
  result.stable_hash = execution::stable_final_safety_state_hash(result);
  return result;
}

void copy_symbol(const std::string_view symbol,
                 std::array<char, kAlpacaPaperSymbolBytes>& output) noexcept {
  std::ranges::copy(symbol, output.begin());
  output[symbol.size()] = '\0';
}

} // namespace

bool valid_alpaca_paper_symbol(const std::string_view symbol) noexcept {
  if (symbol.empty() || symbol.size() >= kAlpacaPaperSymbolBytes ||
      symbol.front() < 'A' || symbol.front() > 'Z') {
    return false;
  }
  bool previous_separator = false;
  bool has_separator = false;
  for (const auto character : symbol) {
    const bool alphanumeric = (character >= 'A' && character <= 'Z') ||
                              (character >= '0' && character <= '9');
    if (alphanumeric) {
      previous_separator = false;
      continue;
    }
    if ((character != '.' && character != '-') || previous_separator || has_separator) {
      return false;
    }
    previous_separator = true;
    has_separator = true;
  }
  return !previous_separator;
}

std::uint64_t stable_alpaca_paper_path_hash(AlpacaPaperPathEvidence evidence) noexcept {
  evidence.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix(hash, evidence.schema_major);
  mix(hash, evidence.schema_minor);
  mix(hash, static_cast<std::uint8_t>(evidence.stage));
  mix_chars(hash, evidence.symbol);
  mix(hash, evidence.instrument_id_high);
  mix(hash, evidence.instrument_id_low);
  mix_chars(hash, evidence.client_order_id);
  mix(hash, evidence.price_ticks);
  mix(hash, evidence.price_increment_currency_nanos);
  mix(hash, evidence.quantity_units);
  mix(hash, evidence.router_configuration_hash);
  mix(hash, evidence.routing_request_hash);
  mix(hash, evidence.routing_decision_hash);
  mix(hash, evidence.risk_snapshot_hash);
  mix(hash, evidence.risk_context_hash);
  mix(hash, evidence.risk_decision_hash);
  mix(hash, evidence.risk_journal_sequence);
  mix(hash, evidence.oms_configuration_hash);
  mix(hash, evidence.oms_command_hash);
  mix(hash, evidence.oms_journal_sequence);
  mix(hash, evidence.gateway_configuration_hash);
  mix(hash, evidence.gateway_request_hash);
  mix(hash, evidence.gateway_audit_sequence);
  mix(hash, evidence.gateway_audit_hash);
  mix(hash, evidence.gateway_outbound_sequence);
  mix(hash, evidence.paper_mode ? 1U : 0U);
  mix(hash, evidence.buy_only ? 1U : 0U);
  mix(hash, evidence.regular_hours_only ? 1U : 0U);
  mix(hash, evidence.fresh_pretrade_risk_required ? 1U : 0U);
  mix(hash, evidence.risk_approved ? 1U : 0U);
  mix(hash, evidence.oms_command_valid ? 1U : 0U);
  mix(hash, evidence.gateway_final_gate_accepted ? 1U : 0U);
  mix(hash, evidence.live_trading_compiled ? 1U : 0U);
  return nonzero_hash(hash);
}

bool valid_alpaca_paper_path_evidence(
    const AlpacaPaperPathEvidence& evidence) noexcept {
  const auto* const symbol_end = std::ranges::find(evidence.symbol, '\0');
  const auto* const client_id_end = std::ranges::find(evidence.client_order_id, '\0');
  if (symbol_end == evidence.symbol.end() ||
      client_id_end == evidence.client_order_id.end()) {
    return false;
  }
  const auto symbol =
      std::string_view{evidence.symbol.data(),
                       static_cast<std::size_t>(symbol_end - evidence.symbol.begin())};
  const auto client_id = std::string_view{
      evidence.client_order_id.data(),
      static_cast<std::size_t>(client_id_end - evidence.client_order_id.begin())};
  const auto expected_instrument_id = certification_instrument_id(symbol);
  return evidence.schema_major == kAlpacaPaperPathSchemaMajor &&
         evidence.schema_minor == kAlpacaPaperPathSchemaMinor &&
         evidence.stage == AlpacaPaperPathStage::complete &&
         valid_alpaca_paper_symbol(symbol) &&
         evidence.instrument_id_high == expected_instrument_id.high() &&
         evidence.instrument_id_low == expected_instrument_id.low() &&
         client_id.size() == 32U && evidence.price_ticks == 1 &&
         evidence.price_increment_currency_nanos ==
             kAlpacaPaperPriceIncrementCurrencyNanos &&
         evidence.quantity_units == 1U && evidence.router_configuration_hash != 0U &&
         evidence.routing_request_hash != 0U && evidence.routing_decision_hash != 0U &&
         evidence.risk_snapshot_hash != 0U && evidence.risk_context_hash != 0U &&
         evidence.risk_decision_hash != 0U && evidence.risk_journal_sequence != 0U &&
         evidence.oms_configuration_hash != 0U && evidence.oms_command_hash != 0U &&
         evidence.oms_journal_sequence != 0U &&
         evidence.gateway_configuration_hash != 0U &&
         evidence.gateway_request_hash != 0U && evidence.gateway_audit_sequence != 0U &&
         evidence.gateway_audit_hash != 0U &&
         evidence.gateway_outbound_sequence != 0U && evidence.paper_mode &&
         evidence.buy_only && evidence.regular_hours_only &&
         evidence.fresh_pretrade_risk_required && evidence.risk_approved &&
         evidence.oms_command_valid && evidence.gateway_final_gate_accepted &&
         !evidence.live_trading_compiled && evidence.stable_hash != 0U &&
         evidence.stable_hash == stable_alpaca_paper_path_hash(evidence);
}

AlpacaPaperPathEvidence run_alpaca_paper_path(const std::string_view symbol,
                                              const std::uint64_t seed) noexcept {
  AlpacaPaperPathEvidence evidence{};
  evidence.live_trading_compiled = execution::kLiveTradingCompiled;
  if (!valid_alpaca_paper_symbol(symbol) || seed == 0U) {
    evidence.stage = AlpacaPaperPathStage::invalid_symbol;
    evidence.stable_hash = stable_alpaca_paper_path_hash(evidence);
    return evidence;
  }
  copy_symbol(symbol, evidence.symbol);
  const auto instrument_id = certification_instrument_id(symbol);
  evidence.instrument_id_high = instrument_id.high();
  evidence.instrument_id_low = instrument_id.low();
  if (execution::kLiveTradingCompiled) {
    evidence.stage = AlpacaPaperPathStage::live_capable_build;
    evidence.stable_hash = stable_alpaca_paper_path_hash(evidence);
    return evidence;
  }

  const auto router_config = router_configuration();
  execution::SmartOrderRouter router{router_config};
  const auto route_request = routing_request(instrument_id);
  const auto route = router.route(route_request);
  evidence.router_configuration_hash = router_config.stable_hash;
  evidence.routing_request_hash = route_request.stable_hash;
  evidence.routing_decision_hash = route.stable_hash;
  evidence.fresh_pretrade_risk_required = route.requires_fresh_pretrade_risk;
  if (route.status != execution::RoutingStatus::routed ||
      route.reason != execution::RoutingReason::routed ||
      route.action != execution::RoutedAction::new_order ||
      route.time_in_force != execution::RoutedTimeInForce::day ||
      route.venue_id != kVenue || route.price_ticks != 1 ||
      route.quantity_units != 1U || !route.requires_fresh_pretrade_risk) {
    evidence.stage = AlpacaPaperPathStage::router_rejected;
    evidence.stable_hash = stable_alpaca_paper_path_hash(evidence);
    return evidence;
  }

  auto risk_journal = std::make_unique<risk::RiskDecisionJournal>();
  const auto limits = risk_limits(instrument_id);
  risk::DeterministicPreTradeRiskEngine risk_engine{limits, *risk_journal};
  const auto position_status =
      risk_engine.seed_position({.symbol_index = 0U,
                                 .net_quantity_units = 0,
                                 .mark_price_ticks = 1,
                                 .as_of_process_monotonic_time_ns = kNow - 1U});
  const auto pnl_status =
      risk_engine.update_profit_loss({.strategy_index = 0U,
                                      .firm_daily_pnl_currency_nanos = 0,
                                      .firm_peak_pnl_currency_nanos = 0,
                                      .strategy_pnl_currency_nanos = 0,
                                      .strategy_peak_pnl_currency_nanos = 0,
                                      .as_of_process_monotonic_time_ns = kNow - 1U});
  const auto risk_evaluation = risk_request(route, instrument_id, seed);
  const auto risk_result = risk_engine.evaluate(risk_evaluation);
  evidence.risk_snapshot_hash = risk_result.decision.risk_snapshot_hash;
  evidence.risk_context_hash = risk_result.decision.risk_context_hash;
  evidence.risk_decision_hash = risk_result.decision.stable_hash;
  evidence.risk_journal_sequence = risk_result.decision.journal_sequence;
  evidence.risk_approved =
      risk_result.decision.decision == risk::DecisionCode::approved;
  if (position_status != risk::StateUpdateStatus::applied ||
      pnl_status != risk::StateUpdateStatus::applied ||
      risk_result.status != risk::EvaluationStatus::journaled ||
      !evidence.risk_approved) {
    evidence.stage = AlpacaPaperPathStage::risk_rejected;
    evidence.stable_hash = stable_alpaca_paper_path_hash(evidence);
    return evidence;
  }

  auto oms_journal = std::make_unique<oms::OmsJournal>();
  const auto oms_config = oms_configuration();
  oms::DeterministicOms oms_service{oms_config, *oms_journal};
  oms::OmsInput accept{.receipt_id = common::GlobalEventId{160U, 1U},
                       .kind = oms::InputKind::accept_intent,
                       .source = oms::EventSource::internal,
                       .intent = risk_evaluation.intent,
                       .risk_decision = risk_result.decision,
                       .process_monotonic_time_ns = kNow + 11U};
  accept.stable_hash = oms::stable_input_hash(accept);
  const auto accepted = oms_service.apply(accept);
  const auto ready = oms_service.apply(
      oms_internal(oms::InputKind::mark_ready, accepted.order_id, 2U));
  const auto dispatched =
      oms_service.apply(oms_internal(oms::InputKind::dispatch, accepted.order_id, 3U));
  evidence.oms_configuration_hash = oms_config.stable_hash;
  evidence.oms_command_hash = dispatched.gateway_command.stable_hash;
  evidence.oms_journal_sequence = dispatched.journal_sequence;
  evidence.oms_command_valid = oms::valid_gateway_command(dispatched.gateway_command);
  if (accepted.status != oms::ApplyStatus::applied ||
      ready.status != oms::ApplyStatus::applied ||
      dispatched.status != oms::ApplyStatus::applied || !evidence.oms_command_valid) {
    evidence.stage = AlpacaPaperPathStage::oms_rejected;
    evidence.stable_hash = stable_alpaca_paper_path_hash(evidence);
    return evidence;
  }

  const auto gateway_config = gateway_configuration(instrument_id, seed);
  auto gateway_journal = std::make_unique<execution::GatewayAuditJournal>();
  execution::PaperBrokerGateway gateway{gateway_config, *gateway_journal};
  if (!gateway.start(kNow - 1'000U)) {
    evidence.stage = AlpacaPaperPathStage::gateway_rejected;
    evidence.stable_hash = stable_alpaca_paper_path_hash(evidence);
    return evidence;
  }
  execution::GatewayRequest gateway_request{
      .command = dispatched.gateway_command,
      .risk_decision = risk_result.decision,
      .safety = final_safety(gateway_config, risk_evaluation.context)};
  gateway_request.stable_hash = execution::stable_gateway_request_hash(gateway_request);
  const auto gateway_result = gateway.send_order(gateway_request);
  evidence.gateway_configuration_hash = gateway_config.stable_hash;
  evidence.gateway_request_hash = gateway_request.stable_hash;
  evidence.gateway_audit_sequence = gateway_result.journal_sequence;
  evidence.gateway_audit_hash = gateway_result.journal_record_hash;
  evidence.gateway_outbound_sequence = gateway_result.outbound_sequence;
  evidence.gateway_final_gate_accepted =
      gateway_result.status == execution::GatewaySubmitStatus::accepted &&
      gateway_result.reason == execution::GatewayReason::accepted &&
      gateway_journal->verify_chain();
  if (!evidence.gateway_final_gate_accepted) {
    evidence.stage = AlpacaPaperPathStage::gateway_rejected;
    evidence.stable_hash = stable_alpaca_paper_path_hash(evidence);
    return evidence;
  }

  evidence.client_order_id = dispatched.gateway_command.client_order_id;
  evidence.price_ticks = dispatched.gateway_command.price_ticks;
  evidence.price_increment_currency_nanos = kAlpacaPaperPriceIncrementCurrencyNanos;
  evidence.quantity_units = dispatched.gateway_command.quantity_units;
  evidence.paper_mode = risk_result.decision.trading_mode == risk::TradingMode::paper &&
                        gateway.mode() == execution::GatewayMode::paper;
  evidence.buy_only = dispatched.gateway_command.side == risk::IntentAction::buy;
  evidence.regular_hours_only = true;
  evidence.stage = AlpacaPaperPathStage::complete;
  evidence.stable_hash = stable_alpaca_paper_path_hash(evidence);
  return evidence;
}

} // namespace aegis::integration
