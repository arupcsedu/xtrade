#include "aegis/risk/types.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>

namespace aegis::risk {
namespace {

constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;
constexpr std::int32_t kMaximumAbsoluteFactorBetaPpm = 10'000'000;

void mix(std::uint64_t& hash, const std::uint64_t value) noexcept {
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    hash ^= (value >> shift) & 0xFFU;
    hash *= kFnvPrime;
  }
}

template <typename Tag>
void mix_identifier(std::uint64_t& hash,
                    const common::Identifier128<Tag> identifier) noexcept {
  mix(hash, identifier.high());
  mix(hash, identifier.low());
}

void mix_digest(std::uint64_t& hash, const common::Sha256Digest& digest) noexcept {
  for (const auto byte : digest) {
    hash ^= byte;
    hash *= kFnvPrime;
  }
}

[[nodiscard]] std::uint64_t nonzero_hash(const std::uint64_t hash) noexcept {
  return hash == 0U ? 1U : hash;
}

[[nodiscard]] bool
valid_market_state_value(const market_state::MarketState state) noexcept {
  return market_state::valid_market_state(state);
}

[[nodiscard]] bool
valid_official_status(const market_state::OfficialTradingStatus status) noexcept {
  return status >= market_state::OfficialTradingStatus::unknown &&
         status <= market_state::OfficialTradingStatus::closed;
}

[[nodiscard]] bool valid_feed_health(const market_state::FeedHealth health) noexcept {
  return health >= market_state::FeedHealth::unknown &&
         health <= market_state::FeedHealth::invalid;
}

[[nodiscard]] bool
valid_book_validity(const market_state::BookValidity validity) noexcept {
  return validity >= market_state::BookValidity::recovering &&
         validity <= market_state::BookValidity::invalid;
}

[[nodiscard]] bool valid_clock_state(const time::ClockQualityState state) noexcept {
  return state >= time::ClockQualityState::unknown &&
         state <= time::ClockQualityState::unsafe;
}

[[nodiscard]] bool valid_clock_operation(const time::ClockOperationMode mode) noexcept {
  return mode >= time::ClockOperationMode::blocked &&
         mode <= time::ClockOperationMode::normal;
}

[[nodiscard]] bool duplicate_symbol(const RiskLimitSnapshot& snapshot,
                                    const std::size_t index) noexcept {
  for (std::size_t other = 0U; other < index; ++other) {
    if (snapshot.symbols[other].instrument_id ==
        snapshot.symbols[index].instrument_id) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool duplicate_strategy(const RiskLimitSnapshot& snapshot,
                                      const std::size_t index) noexcept {
  for (std::size_t other = 0U; other < index; ++other) {
    if (snapshot.strategies[other].strategy_id ==
        snapshot.strategies[index].strategy_id) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool duplicate_venue(const RiskLimitSnapshot& snapshot,
                                   const std::size_t index) noexcept {
  for (std::size_t other = 0U; other < index; ++other) {
    if (snapshot.venues[other].venue_id == snapshot.venues[index].venue_id) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool valid_symbol(const RiskLimitSnapshot& snapshot,
                                const std::size_t index) noexcept {
  const auto& symbol = snapshot.symbols[index];
  if (!symbol.instrument_id.valid() || duplicate_symbol(snapshot, index) ||
      symbol.tick_value_currency_nanos == 0U || symbol.price_increment_ticks == 0U ||
      symbol.maximum_order_quantity_units == 0U ||
      symbol.maximum_order_notional_currency_nanos == 0U ||
      symbol.maximum_absolute_position_units == 0U ||
      symbol.maximum_order_notional_currency_nanos > kMaximumMonetaryNanos ||
      symbol.sector_index >= snapshot.sector_count) {
    return false;
  }
  for (std::size_t factor = 0U; factor < snapshot.factor_count; ++factor) {
    const auto beta = symbol.factor_beta_ppm[factor];
    if (beta < -kMaximumAbsoluteFactorBetaPpm || beta > kMaximumAbsoluteFactorBetaPpm) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool valid_strategy(const RiskLimitSnapshot& snapshot,
                                  const std::size_t index) noexcept {
  const auto& strategy = snapshot.strategies[index];
  return strategy.strategy_id.valid() && !duplicate_strategy(snapshot, index) &&
         strategy.maximum_loss_currency_nanos != 0U &&
         strategy.maximum_drawdown_currency_nanos != 0U &&
         strategy.maximum_loss_currency_nanos <= kMaximumMonetaryNanos &&
         strategy.maximum_drawdown_currency_nanos <= kMaximumMonetaryNanos;
}

[[nodiscard]] bool valid_venue(const RiskLimitSnapshot& snapshot,
                               const std::size_t index) noexcept {
  return snapshot.venues[index].venue_id.valid() && !duplicate_venue(snapshot, index);
}

void mix_symbol(std::uint64_t& hash, const SymbolLimit& symbol,
                const std::size_t factor_count) noexcept {
  mix_identifier(hash, symbol.instrument_id);
  mix(hash, symbol.tick_value_currency_nanos);
  mix(hash, symbol.price_increment_ticks);
  mix(hash, symbol.maximum_order_quantity_units);
  mix(hash, symbol.maximum_order_notional_currency_nanos);
  mix(hash, symbol.maximum_price_deviation_ticks);
  mix(hash, symbol.maximum_absolute_position_units);
  mix(hash, symbol.sector_index);
  for (std::size_t factor = 0U; factor < factor_count; ++factor) {
    mix(hash, std::bit_cast<std::uint32_t>(symbol.factor_beta_ppm[factor]));
  }
  mix(hash, symbol.authorized ? 1U : 0U);
  mix(hash, symbol.restricted ? 1U : 0U);
}

[[nodiscard]] bool valid_decision_code(const DecisionCode decision) noexcept {
  return decision == DecisionCode::rejected || decision == DecisionCode::approved;
}

[[nodiscard]] bool valid_risk_reason(const RiskReason reason) noexcept {
  return reason >= RiskReason::within_limits && reason <= RiskReason::engine_busy;
}

[[nodiscard]] bool valid_risk_check(const RiskCheck check) noexcept {
  return check >= RiskCheck::none && check <= RiskCheck::configuration_freshness;
}

} // namespace

bool valid_trading_mode(const TradingMode mode) noexcept {
  return mode == TradingMode::simulation || mode == TradingMode::paper ||
         mode == TradingMode::live;
}

bool valid_intent_action(const IntentAction action) noexcept {
  return action == IntentAction::buy || action == IntentAction::sell ||
         action == IntentAction::cancel;
}

bool valid_policy_hook_result(const PolicyHookResult result) noexcept {
  return result >= PolicyHookResult::not_required &&
         result <= PolicyHookResult::unavailable;
}

std::uint64_t reason_bit(const RiskReason reason) noexcept {
  const auto ordinal = static_cast<std::uint8_t>(reason);
  return valid_risk_reason(reason) && ordinal < 64U ? std::uint64_t{1U} << ordinal : 0U;
}

std::uint32_t check_bit(const RiskCheck check) noexcept {
  const auto ordinal = static_cast<std::uint8_t>(check);
  return check != RiskCheck::none && valid_risk_check(check) && ordinal <= 32U
             ? std::uint32_t{1U} << (ordinal - 1U)
             : 0U;
}

std::uint64_t stable_limit_snapshot_hash(RiskLimitSnapshot snapshot) noexcept {
  snapshot.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix_identifier(hash, snapshot.risk_snapshot_id);
  mix_identifier(hash, snapshot.configuration_version);
  mix_identifier(hash, snapshot.policy_version);
  mix_identifier(hash, snapshot.session_id);
  mix_identifier(hash, snapshot.account_id);
  mix(hash, snapshot.revision);
  mix(hash, snapshot.authority_epoch);
  mix(hash, snapshot.published_process_monotonic_time_ns);
  mix(hash, snapshot.valid_until_process_monotonic_time_ns);
  mix(hash, snapshot.approval_ttl_ns);
  mix(hash, snapshot.maximum_position_age_ns);
  mix(hash, snapshot.maximum_market_state_age_ns);
  mix(hash, snapshot.maximum_clock_state_age_ns);
  mix(hash, snapshot.maximum_feed_book_age_ns);
  mix(hash, snapshot.maximum_configuration_age_ns);
  mix(hash, snapshot.order_rate_window_ns);
  mix(hash, snapshot.cancel_rate_window_ns);
  mix(hash, snapshot.maximum_orders_per_window);
  mix(hash, snapshot.maximum_cancels_per_window);
  mix(hash, snapshot.allowed_market_state_mask);
  mix(hash, snapshot.maximum_gross_exposure_currency_nanos);
  mix(hash, snapshot.maximum_absolute_net_exposure_currency_nanos);
  mix(hash, snapshot.maximum_daily_loss_currency_nanos);
  mix(hash, snapshot.maximum_drawdown_currency_nanos);
  mix(hash, snapshot.credit_capital_limit_currency_nanos);
  mix(hash, snapshot.symbol_count);
  mix(hash, snapshot.strategy_count);
  mix(hash, snapshot.venue_count);
  mix(hash, snapshot.sector_count);
  mix(hash, snapshot.factor_count);
  mix(hash, snapshot.allow_simulation ? 1U : 0U);
  mix(hash, snapshot.allow_paper ? 1U : 0U);
  mix(hash, snapshot.require_locate_for_short_sale ? 1U : 0U);
  mix(hash, snapshot.require_self_trade_prevention ? 1U : 0U);
  for (std::size_t index = 0U; index < snapshot.sector_count; ++index) {
    mix(hash, snapshot.maximum_sector_exposure_currency_nanos[index]);
  }
  for (std::size_t index = 0U; index < snapshot.factor_count; ++index) {
    mix(hash, snapshot.maximum_factor_exposure_currency_nanos[index]);
  }
  for (std::size_t index = 0U; index < snapshot.symbol_count; ++index) {
    mix_symbol(hash, snapshot.symbols[index], snapshot.factor_count);
  }
  for (std::size_t index = 0U; index < snapshot.strategy_count; ++index) {
    const auto& strategy = snapshot.strategies[index];
    mix_identifier(hash, strategy.strategy_id);
    mix(hash, strategy.maximum_loss_currency_nanos);
    mix(hash, strategy.maximum_drawdown_currency_nanos);
    mix(hash, strategy.authorized ? 1U : 0U);
  }
  for (std::size_t index = 0U; index < snapshot.venue_count; ++index) {
    mix_identifier(hash, snapshot.venues[index].venue_id);
    mix(hash, snapshot.venues[index].authorized ? 1U : 0U);
  }
  return nonzero_hash(hash);
}

bool valid_limit_snapshot(const RiskLimitSnapshot& snapshot) noexcept {
  constexpr auto kAllMarketStates = static_cast<std::uint16_t>(
      (std::uint32_t{1U} << market_state::kMarketStateCount) - 1U);
  if (!snapshot.risk_snapshot_id.valid() || !snapshot.configuration_version.valid() ||
      !snapshot.policy_version.valid() || !snapshot.session_id.valid() ||
      !snapshot.account_id.valid() || snapshot.revision == 0U ||
      snapshot.authority_epoch == 0U ||
      snapshot.published_process_monotonic_time_ns == 0U ||
      snapshot.valid_until_process_monotonic_time_ns <=
          snapshot.published_process_monotonic_time_ns ||
      snapshot.approval_ttl_ns == 0U || snapshot.maximum_position_age_ns == 0U ||
      snapshot.maximum_market_state_age_ns == 0U ||
      snapshot.maximum_clock_state_age_ns == 0U ||
      snapshot.maximum_feed_book_age_ns == 0U ||
      snapshot.maximum_configuration_age_ns == 0U ||
      snapshot.order_rate_window_ns == 0U || snapshot.cancel_rate_window_ns == 0U ||
      snapshot.maximum_orders_per_window == 0U ||
      snapshot.maximum_orders_per_window > kMaximumRateEvents ||
      snapshot.maximum_cancels_per_window == 0U ||
      snapshot.maximum_cancels_per_window > kMaximumRateEvents ||
      snapshot.allowed_market_state_mask == 0U ||
      (snapshot.allowed_market_state_mask & ~kAllMarketStates) != 0U ||
      snapshot.maximum_gross_exposure_currency_nanos == 0U ||
      snapshot.maximum_absolute_net_exposure_currency_nanos == 0U ||
      snapshot.maximum_daily_loss_currency_nanos == 0U ||
      snapshot.maximum_drawdown_currency_nanos == 0U ||
      snapshot.credit_capital_limit_currency_nanos == 0U ||
      snapshot.maximum_gross_exposure_currency_nanos > kMaximumMonetaryNanos ||
      snapshot.maximum_absolute_net_exposure_currency_nanos > kMaximumMonetaryNanos ||
      snapshot.maximum_daily_loss_currency_nanos > kMaximumMonetaryNanos ||
      snapshot.maximum_drawdown_currency_nanos > kMaximumMonetaryNanos ||
      snapshot.credit_capital_limit_currency_nanos > kMaximumMonetaryNanos ||
      snapshot.symbol_count == 0U || snapshot.symbol_count > kMaximumRiskSymbols ||
      snapshot.strategy_count == 0U ||
      snapshot.strategy_count > kMaximumRiskStrategies || snapshot.venue_count == 0U ||
      snapshot.venue_count > kMaximumRiskVenues || snapshot.sector_count == 0U ||
      snapshot.sector_count > kMaximumRiskSectors || snapshot.factor_count == 0U ||
      snapshot.factor_count > kMaximumRiskFactors ||
      (!snapshot.allow_simulation && !snapshot.allow_paper) ||
      snapshot.stable_hash == 0U ||
      snapshot.stable_hash != stable_limit_snapshot_hash(snapshot)) {
    return false;
  }
  for (std::size_t index = 0U; index < snapshot.sector_count; ++index) {
    if (snapshot.maximum_sector_exposure_currency_nanos[index] == 0U ||
        snapshot.maximum_sector_exposure_currency_nanos[index] >
            kMaximumMonetaryNanos) {
      return false;
    }
  }
  for (std::size_t index = 0U; index < snapshot.factor_count; ++index) {
    if (snapshot.maximum_factor_exposure_currency_nanos[index] == 0U ||
        snapshot.maximum_factor_exposure_currency_nanos[index] >
            kMaximumMonetaryNanos) {
      return false;
    }
  }
  for (std::size_t index = 0U; index < snapshot.symbol_count; ++index) {
    if (!valid_symbol(snapshot, index)) {
      return false;
    }
  }
  for (std::size_t index = 0U; index < snapshot.strategy_count; ++index) {
    if (!valid_strategy(snapshot, index)) {
      return false;
    }
  }
  for (std::size_t index = 0U; index < snapshot.venue_count; ++index) {
    if (!valid_venue(snapshot, index)) {
      return false;
    }
  }
  return true;
}

std::uint64_t stable_risk_intent_hash(RiskIntent intent) noexcept {
  intent.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix_identifier(hash, intent.intent_id);
  mix_identifier(hash, intent.session_id);
  mix_identifier(hash, intent.account_id);
  mix_identifier(hash, intent.strategy_id);
  mix_identifier(hash, intent.venue_id);
  mix_identifier(hash, intent.instrument_id);
  mix_identifier(hash, intent.source_forecast_id);
  mix_identifier(hash, intent.feature_snapshot_id);
  mix_identifier(hash, intent.target_order_id);
  mix_identifier(hash, intent.configuration_version);
  mix(hash, static_cast<std::uint64_t>(intent.action));
  mix(hash, std::bit_cast<std::uint64_t>(intent.limit_price_ticks));
  mix(hash, intent.quantity_units);
  mix(hash, intent.created_process_monotonic_time_ns);
  mix(hash, intent.expire_process_monotonic_time_ns);
  mix_digest(hash, intent.canonical_sha256);
  return nonzero_hash(hash);
}

bool valid_risk_intent(const RiskIntent& intent) noexcept {
  if (!intent.intent_id.valid() || !intent.session_id.valid() ||
      !intent.account_id.valid() || !intent.strategy_id.valid() ||
      !intent.venue_id.valid() || !intent.instrument_id.valid() ||
      !intent.configuration_version.valid() || !valid_intent_action(intent.action) ||
      intent.created_process_monotonic_time_ns == 0U ||
      intent.expire_process_monotonic_time_ns <=
          intent.created_process_monotonic_time_ns ||
      common::is_zero_digest(intent.canonical_sha256) || intent.stable_hash == 0U ||
      intent.stable_hash != stable_risk_intent_hash(intent)) {
    return false;
  }
  if (intent.action == IntentAction::cancel) {
    return intent.target_order_id.valid() && !intent.source_forecast_id.valid() &&
           !intent.feature_snapshot_id.valid() && intent.quantity_units == 0U &&
           intent.limit_price_ticks == 0;
  }
  return !intent.target_order_id.valid() && intent.source_forecast_id.valid() &&
         intent.feature_snapshot_id.valid() && intent.quantity_units != 0U &&
         intent.limit_price_ticks > 0;
}

std::uint64_t stable_risk_context_hash(RiskContext context) noexcept {
  context.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix(hash, context.now_process_monotonic_time_ns);
  mix(hash, static_cast<std::uint64_t>(context.trading_mode));
  mix(hash, context.operator_authorized ? 1U : 0U);
  mix(hash, context.session_authorized ? 1U : 0U);
  mix(hash, context.operator_authorization_valid_until_ns);
  mix(hash, context.authority_epoch);
  mix(hash, context.process_id);
  mix(hash, context.market_state_snapshot.stable_hash);
  mix(hash, static_cast<std::uint64_t>(context.official_trading_status));
  mix(hash, static_cast<std::uint64_t>(context.feed_health));
  mix(hash, static_cast<std::uint64_t>(context.book_validity));
  mix(hash, static_cast<std::uint64_t>(context.clock_quality_snapshot.state));
  mix(hash, static_cast<std::uint64_t>(context.clock_quality_snapshot.operation_mode));
  mix(hash, context.clock_quality_snapshot.observed_at.value());
  mix_identifier(hash, context.clock_quality_snapshot.source_id);
  mix(hash, context.feed_book_observed_process_monotonic_time_ns);
  mix(hash, context.feed_book_state_hash);
  mix(hash, std::bit_cast<std::uint64_t>(context.reference_price_ticks));
  mix(hash, static_cast<std::uint64_t>(context.short_sale_locate));
  mix(hash, static_cast<std::uint64_t>(context.self_trade_prevention));
  return nonzero_hash(hash);
}

bool valid_risk_context(const RiskContext& context) noexcept {
  return context.now_process_monotonic_time_ns != 0U &&
         context.now_process_monotonic_time_ns <
             std::numeric_limits<std::uint64_t>::max() &&
         valid_trading_mode(context.trading_mode) && context.authority_epoch != 0U &&
         context.process_id != 0U && context.market_state_snapshot.valid() &&
         valid_market_state_value(context.market_state_snapshot.state) &&
         valid_official_status(context.official_trading_status) &&
         valid_feed_health(context.feed_health) &&
         valid_book_validity(context.book_validity) &&
         valid_clock_state(context.clock_quality_snapshot.state) &&
         valid_clock_operation(context.clock_quality_snapshot.operation_mode) &&
         context.clock_quality_snapshot.observed_at.value() != 0U &&
         context.clock_quality_snapshot.source_id.valid() &&
         context.feed_book_observed_process_monotonic_time_ns != 0U &&
         context.feed_book_state_hash != 0U && context.reference_price_ticks > 0 &&
         valid_policy_hook_result(context.short_sale_locate) &&
         valid_policy_hook_result(context.self_trade_prevention) &&
         context.stable_hash != 0U &&
         context.stable_hash == stable_risk_context_hash(context);
}

std::uint64_t stable_risk_decision_hash(RiskDecision decision) noexcept {
  decision.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix_identifier(hash, decision.global_event_id);
  mix_identifier(hash, decision.intent_id);
  mix_identifier(hash, decision.session_id);
  mix_identifier(hash, decision.account_id);
  mix_identifier(hash, decision.strategy_id);
  mix_identifier(hash, decision.venue_id);
  mix_identifier(hash, decision.instrument_id);
  mix_identifier(hash, decision.risk_snapshot_id);
  mix_identifier(hash, decision.configuration_version);
  mix(hash, static_cast<std::uint64_t>(decision.decision));
  mix(hash, static_cast<std::uint64_t>(decision.reason));
  mix(hash, static_cast<std::uint64_t>(decision.failed_check));
  mix(hash, static_cast<std::uint64_t>(decision.intent_action));
  mix(hash, static_cast<std::uint64_t>(decision.trading_mode));
  mix(hash, decision.approved_quantity_units);
  mix(hash, std::bit_cast<std::uint64_t>(decision.approved_price_ticks));
  mix(hash, decision.evaluated_process_monotonic_time_ns);
  mix(hash, decision.valid_until_process_monotonic_time_ns);
  mix(hash, decision.intent_hash);
  mix_digest(hash, decision.intent_sha256);
  mix(hash, decision.risk_snapshot_hash);
  mix(hash, decision.risk_context_hash);
  mix(hash, decision.position_generation);
  mix(hash, decision.authority_epoch);
  mix(hash, decision.policy_revision);
  mix(hash, decision.evaluation_ordinal);
  mix(hash, decision.journal_sequence);
  mix(hash, decision.reason_mask);
  mix(hash, decision.completed_check_mask);
  return nonzero_hash(hash);
}

bool valid_risk_decision(const RiskDecision& decision) noexcept {
  if (!decision.global_event_id.valid() || !decision.intent_id.valid() ||
      !decision.session_id.valid() || !decision.account_id.valid() ||
      !decision.strategy_id.valid() || !decision.venue_id.valid() ||
      !decision.instrument_id.valid() || !decision.risk_snapshot_id.valid() ||
      !decision.configuration_version.valid() ||
      !valid_decision_code(decision.decision) || !valid_risk_reason(decision.reason) ||
      !valid_risk_check(decision.failed_check) ||
      !valid_intent_action(decision.intent_action) ||
      !valid_trading_mode(decision.trading_mode) ||
      decision.evaluated_process_monotonic_time_ns == 0U ||
      decision.valid_until_process_monotonic_time_ns <=
          decision.evaluated_process_monotonic_time_ns ||
      decision.intent_hash == 0U || common::is_zero_digest(decision.intent_sha256) ||
      decision.risk_snapshot_hash == 0U || decision.risk_context_hash == 0U ||
      decision.position_generation == 0U || decision.authority_epoch == 0U ||
      decision.policy_revision == 0U || decision.evaluation_ordinal == 0U ||
      decision.journal_sequence == 0U ||
      (decision.reason_mask & reason_bit(decision.reason)) == 0U ||
      decision.stable_hash == 0U ||
      decision.stable_hash != stable_risk_decision_hash(decision)) {
    return false;
  }
  const auto approved = decision.decision == DecisionCode::approved;
  const auto approved_values_valid =
      decision.intent_action == IntentAction::cancel
          ? decision.approved_quantity_units == 0U && decision.approved_price_ticks == 0
          : decision.approved_quantity_units != 0U && decision.approved_price_ticks > 0;
  if (approved) {
    constexpr auto kAllChecks = (std::uint32_t{1U} << 30U) - 1U;
    return decision.reason == RiskReason::within_limits &&
           decision.failed_check == RiskCheck::none && approved_values_valid &&
           decision.completed_check_mask == kAllChecks;
  }
  return decision.reason != RiskReason::within_limits &&
         decision.approved_quantity_units == 0U && decision.approved_price_ticks == 0;
}

} // namespace aegis::risk
