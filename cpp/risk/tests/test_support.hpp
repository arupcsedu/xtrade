#ifndef AEGIS_RISK_TEST_SUPPORT_HPP
#define AEGIS_RISK_TEST_SUPPORT_HPP

#include "aegis/risk/engine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aegis::risk::test {

inline constexpr std::uint64_t kNow = 1'000'000U;
inline constexpr common::SessionId kSession{1U, 1U};
inline constexpr common::AccountId kAccount{2U, 2U};
inline constexpr common::StrategyId kStrategy{3U, 3U};
inline constexpr common::VenueId kVenue{4U, 4U};
inline constexpr common::InstrumentId kInstrument{5U, 5U};
inline constexpr common::ConfigurationVersion kConfiguration{6U, 6U};
inline constexpr common::RiskSnapshotId kRiskSnapshot{7U, 7U};

[[nodiscard]] inline RiskLimitSnapshot limits() noexcept {
  RiskLimitSnapshot result{
      .risk_snapshot_id = kRiskSnapshot,
      .configuration_version = kConfiguration,
      .policy_version = common::ConfigurationVersion{8U, 8U},
      .session_id = kSession,
      .account_id = kAccount,
      .revision = 1U,
      .authority_epoch = 10U,
      .published_process_monotonic_time_ns = kNow - 1'000U,
      .valid_until_process_monotonic_time_ns = kNow + 1'000'000U,
      .approval_ttl_ns = 10'000U,
      .maximum_position_age_ns = 10'000U,
      .maximum_market_state_age_ns = 1'000U,
      .maximum_clock_state_age_ns = 1'000U,
      .maximum_feed_book_age_ns = 1'000U,
      .maximum_configuration_age_ns = 10'000U,
      .order_rate_window_ns = 1'000U,
      .cancel_rate_window_ns = 1'000U,
      .maximum_orders_per_window = 100U,
      .maximum_cancels_per_window = 100U,
      .allowed_market_state_mask =
          static_cast<std::uint16_t>(std::uint16_t{1U} << static_cast<std::uint8_t>(
                                         market_state::MarketState::normal)),
      .maximum_gross_exposure_currency_nanos = 1'000'000'000'000U,
      .maximum_absolute_net_exposure_currency_nanos = 1'000'000'000'000U,
      .maximum_daily_loss_currency_nanos = 1'000'000'000U,
      .maximum_drawdown_currency_nanos = 1'000'000'000U,
      .credit_capital_limit_currency_nanos = 1'000'000'000'000U,
      .symbol_count = 1U,
      .strategy_count = 1U,
      .venue_count = 1U,
      .sector_count = 1U,
      .factor_count = 1U,
      .allow_simulation = true,
      .allow_paper = true,
      .require_locate_for_short_sale = true,
      .require_self_trade_prevention = true};
  result.maximum_sector_exposure_currency_nanos[0U] = 1'000'000'000'000U;
  result.maximum_factor_exposure_currency_nanos[0U] = 1'000'000'000'000U;
  result.symbols[0U] = {.instrument_id = kInstrument,
                        .tick_value_currency_nanos = 10U,
                        .price_increment_ticks = 1U,
                        .maximum_order_quantity_units = 1'000U,
                        .maximum_order_notional_currency_nanos = 1'000'000'000U,
                        .maximum_price_deviation_ticks = 100U,
                        .maximum_absolute_position_units = 10'000U,
                        .sector_index = 0U,
                        .factor_beta_ppm = {1'000'000},
                        .authorized = true,
                        .restricted = false};
  result.strategies[0U] = {.strategy_id = kStrategy,
                           .maximum_loss_currency_nanos = 500'000'000U,
                           .maximum_drawdown_currency_nanos = 500'000'000U,
                           .authorized = true};
  result.venues[0U] = {.venue_id = kVenue, .authorized = true};
  result.stable_hash = stable_limit_snapshot_hash(result);
  return result;
}

[[nodiscard]] inline common::Sha256Digest digest(const std::uint64_t ordinal) {
  std::array<std::uint8_t, 8> bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(ordinal >> (index * 8U));
  }
  return common::sha256(std::span<const std::uint8_t>{bytes});
}

[[nodiscard]] inline RiskIntent intent(const std::uint64_t ordinal = 1U,
                                       const IntentAction action = IntentAction::buy) {
  RiskIntent result{.intent_id = common::IntentId{20U, ordinal},
                    .session_id = kSession,
                    .account_id = kAccount,
                    .strategy_id = kStrategy,
                    .venue_id = kVenue,
                    .instrument_id = kInstrument,
                    .source_forecast_id = common::ForecastId{21U, ordinal},
                    .feature_snapshot_id = common::FeatureSnapshotId{22U, ordinal},
                    .target_order_id = {},
                    .configuration_version = kConfiguration,
                    .action = action,
                    .limit_price_ticks = 100,
                    .quantity_units = 10U,
                    .created_process_monotonic_time_ns = kNow - 100U,
                    .expire_process_monotonic_time_ns = kNow + 100'000U,
                    .canonical_sha256 = digest(ordinal)};
  if (action == IntentAction::cancel) {
    result.source_forecast_id = {};
    result.feature_snapshot_id = {};
    result.target_order_id = common::OrderId{23U, ordinal};
    result.limit_price_ticks = 0;
    result.quantity_units = 0U;
  }
  result.stable_hash = stable_risk_intent_hash(result);
  return result;
}

[[nodiscard]] inline market_state::MarketStateSnapshot
market_snapshot(const std::uint64_t now = kNow) noexcept {
  market_state::MarketStateSnapshot snapshot{
      .state = market_state::MarketState::normal,
      .primary_reason = market_state::MarketStateReason::normal_conditions_stable,
      .reason_mask = market_state::reason_bit(
          market_state::MarketStateReason::normal_conditions_stable),
      .input_sequence = 1U,
      .transition_sequence = 1U,
      .observed_process_monotonic_time_ns = now - 10U,
      .observed_wall_clock_utc_ns = 1'800'000'000'000'000'000ULL,
      .state_entered_process_monotonic_time_ns = now - 100U};
  snapshot.stable_hash = market_state::stable_market_state_hash(snapshot);
  return snapshot;
}

[[nodiscard]] inline RiskContext context(const std::uint64_t now = kNow) noexcept {
  RiskContext result{
      .now_process_monotonic_time_ns = now,
      .trading_mode = TradingMode::simulation,
      .operator_authorized = true,
      .session_authorized = true,
      .operator_authorization_valid_until_ns = now + 100'000U,
      .authority_epoch = 10U,
      .process_id = 11U,
      .market_state_snapshot = market_snapshot(now),
      .official_trading_status = market_state::OfficialTradingStatus::open,
      .feed_health = market_state::FeedHealth::healthy,
      .book_validity = market_state::BookValidity::valid,
      .clock_quality_snapshot = {.state = time::ClockQualityState::healthy,
                                 .reason = time::ClockQualityReason::within_thresholds,
                                 .operation_mode = time::ClockOperationMode::normal,
                                 .observed_at = time::MonotonicTimeNs{now - 10U},
                                 .ptp_offset = time::DurationNs{10},
                                 .drift_ppb = 1,
                                 .source_id = time::ClockSourceId{12U, 12U},
                                 .last_synchronization_time =
                                     time::MonotonicTimeNs{now - 20U},
                                 .synchronization_age = time::DurationNs{20},
                                 .hardware_timestamp_available = true,
                                 .transition_count = 1U,
                                 .observation_count = 2U},
      .feed_book_observed_process_monotonic_time_ns = now - 10U,
      .feed_book_state_hash = 13U,
      .reference_price_ticks = 100,
      .short_sale_locate = PolicyHookResult::allowed,
      .self_trade_prevention = PolicyHookResult::allowed};
  result.stable_hash = stable_risk_context_hash(result);
  return result;
}

[[nodiscard]] inline RiskEvaluationRequest
request(const std::uint64_t ordinal = 1U, const IntentAction action = IntentAction::buy,
        const std::uint64_t now = kNow) {
  return {.global_event_id = common::GlobalEventId{30U, ordinal},
          .intent = intent(ordinal, action),
          .context = context(now)};
}

inline void initialize_state(DeterministicPreTradeRiskEngine& engine,
                             const std::uint64_t now = kNow) {
  const auto seeded =
      engine.seed_position({.symbol_index = 0U,
                            .net_quantity_units = 0,
                            .mark_price_ticks = 100,
                            .as_of_process_monotonic_time_ns = now - 10U});
  const auto pnl =
      engine.update_profit_loss({.strategy_index = 0U,
                                 .firm_daily_pnl_currency_nanos = 0,
                                 .firm_peak_pnl_currency_nanos = 0,
                                 .strategy_pnl_currency_nanos = 0,
                                 .strategy_peak_pnl_currency_nanos = 0,
                                 .as_of_process_monotonic_time_ns = now - 10U});
  if (seeded != StateUpdateStatus::applied || pnl != StateUpdateStatus::applied) {
    engine.set_state_available(false);
  }
}

} // namespace aegis::risk::test

#endif // AEGIS_RISK_TEST_SUPPORT_HPP
