#ifndef AEGIS_EXECUTION_TEST_SUPPORT_HPP
#define AEGIS_EXECUTION_TEST_SUPPORT_HPP

#include "aegis/execution/simulated_gateway.hpp"

#include "../../risk/tests/test_support.hpp"

#include <cstdint>

namespace aegis::execution::test {

inline constexpr std::uint64_t kNow = risk::test::kNow;
inline constexpr oms::LeaderAuthority kAuthority{.exchange_session_epoch = 77U,
                                                 .fencing_token = 88U};

struct ApprovedIntent {
  risk::RiskIntent intent;
  risk::RiskDecision decision;
};

[[nodiscard]] inline GatewayConfiguration
configuration(const GatewayMode mode = GatewayMode::simulation,
              const std::uint32_t reject_every = 0U) noexcept {
  GatewayConfiguration result{
      .session_id = risk::test::kSession,
      .account_id = risk::test::kAccount,
      .venue_id = risk::test::kVenue,
      .configuration_version = risk::test::kConfiguration,
      .startup_mode = mode,
      .exchange_session_epoch = kAuthority.exchange_session_epoch,
      .initial_fencing_token = kAuthority.fencing_token,
      .heartbeat_interval_ns = 100'000U,
      .heartbeat_timeout_ns = 500'000U,
      .maximum_clock_age_ns = 100'000U,
      .command_rate_window_ns = 1'000U,
      .maximum_new_orders_per_window = 100U,
      .maximum_cancels_per_window = 100U,
      .maximum_replaces_per_window = 100U,
      .mapping_count = 1U,
      .paper_model = {.deterministic_seed = 20'260'831U,
                      .acknowledgement_latency_ns = 100U,
                      .cancel_latency_ns = 200U,
                      .replace_latency_ns = 200U,
                      .initial_queue_ahead_units = 2U,
                      .maximum_fill_chunk_units = 3U,
                      .fee_per_unit_currency_nanos = 5U,
                      .slippage_ticks = 1U,
                      .impact_ticks_per_million_units = 1U,
                      .reject_every_nth_new_order = reject_every,
                      .allow_auction_orders = true}};
  result.mappings[0U] = {.venue_id = risk::test::kVenue,
                         .instrument_id = risk::test::kInstrument,
                         .synthetic_venue_number = 1U,
                         .synthetic_instrument_number = 1U};
  result.stable_hash = stable_gateway_configuration_hash(result);
  return result;
}

// Test builders use named defaults to make individual malformed fields explicit.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] inline ApprovedIntent
approval(const GatewayMode mode = GatewayMode::simulation,
         const risk::IntentAction action = risk::IntentAction::buy,
         const std::uint64_t ordinal = 1U, const std::uint64_t now_ns = kNow,
         const common::OrderId target = {}, const std::int64_t price_ticks = 100,
         const std::uint64_t quantity_units = 10U) {
  risk::RiskDecisionJournal journal;
  risk::DeterministicPreTradeRiskEngine engine{risk::test::limits(), journal};
  risk::test::initialize_state(engine, now_ns);
  auto request = risk::test::request(ordinal, action, now_ns);
  request.context.trading_mode = mode == GatewayMode::paper
                                     ? risk::TradingMode::paper
                                     : risk::TradingMode::simulation;
  request.context.stable_hash = risk::stable_risk_context_hash(request.context);
  request.intent.limit_price_ticks = price_ticks;
  request.intent.quantity_units = quantity_units;
  if (action == risk::IntentAction::cancel) {
    request.intent.target_order_id = target;
    request.intent.source_forecast_id = {};
    request.intent.feature_snapshot_id = {};
    request.intent.limit_price_ticks = 0;
    request.intent.quantity_units = 0U;
  }
  request.intent.created_process_monotonic_time_ns = now_ns - 100U;
  request.intent.expire_process_monotonic_time_ns = now_ns + 100'000U;
  request.intent.stable_hash = risk::stable_risk_intent_hash(request.intent);
  const auto evaluated = engine.evaluate(request);
  return {.intent = request.intent, .decision = evaluated.decision};
}
// NOLINTEND(bugprone-easily-swappable-parameters)

[[nodiscard]] inline FinalSafetyState
safety(const std::uint64_t now_ns = kNow,
       const GatewayMode mode = GatewayMode::simulation) noexcept {
  auto context = risk::test::context(now_ns);
  context.trading_mode = mode == GatewayMode::paper ? risk::TradingMode::paper
                                                    : risk::TradingMode::simulation;
  FinalSafetyState result{.observed_process_monotonic_time_ns = now_ns,
                          .market_state_snapshot = context.market_state_snapshot,
                          .official_trading_status = context.official_trading_status,
                          .feed_health = context.feed_health,
                          .book_validity = context.book_validity,
                          .clock_quality_snapshot = context.clock_quality_snapshot,
                          .authority = kAuthority,
                          .effective_configuration_hash =
                              configuration(mode).stable_hash,
                          .kill_switch_engaged = false,
                          .journal_ready = true};
  result.stable_hash = stable_final_safety_state_hash(result);
  return result;
}

// Test builders use named defaults to make individual malformed fields explicit.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] inline GatewayRequest
request(const GatewayMode mode = GatewayMode::simulation,
        const oms::GatewayCommandKind kind = oms::GatewayCommandKind::new_order,
        const std::uint64_t ordinal = 1U, const std::uint64_t now_ns = kNow,
        const common::OrderId order_id = common::OrderId{50U, 1U},
        const oms::ExternalOrderId external = {}, const std::int64_t price_ticks = 100,
        const std::uint64_t quantity_units = 10U) {
  const auto action = kind == oms::GatewayCommandKind::cancel
                          ? risk::IntentAction::cancel
                          : risk::IntentAction::buy;
  const auto approved =
      approval(mode, action, ordinal, now_ns, order_id, price_ticks, quantity_units);
  oms::GatewayCommand command{.command_id = common::GlobalEventId{60U, ordinal},
                              .kind = kind,
                              .order_id = order_id,
                              .external_order_id = external,
                              .client_order_id =
                                  oms::deterministic_client_order_id(order_id),
                              .session_id = risk::test::kSession,
                              .account_id = risk::test::kAccount,
                              .strategy_id = risk::test::kStrategy,
                              .venue_id = risk::test::kVenue,
                              .instrument_id = risk::test::kInstrument,
                              .configuration_version = risk::test::kConfiguration,
                              .side = risk::IntentAction::buy,
                              .price_ticks = price_ticks,
                              .quantity_units = quantity_units,
                              .authority = kAuthority,
                              .intent_hash = approved.intent.stable_hash,
                              .risk_decision_hash = approved.decision.stable_hash,
                              .generated_process_monotonic_time_ns = now_ns - 1U};
  command.stable_hash = oms::stable_gateway_command_hash(command);
  GatewayRequest result{.command = command,
                        .risk_decision = approved.decision,
                        .safety = safety(now_ns, mode)};
  result.stable_hash = stable_gateway_request_hash(result);
  return result;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

[[nodiscard]] inline market_data::synthetic::SyntheticEvent
status_event(const market_data::synthetic::TradingStatus status,
             const std::uint64_t ordinal, const std::uint64_t now_ns) noexcept {
  market_data::synthetic::SyntheticEvent event{
      .type = market_data::synthetic::NativeMessageType::trading_status,
      .side = market_data::synthetic::Side::none,
      .action = market_data::synthetic::BookAction::none,
      .status = status,
      .data_quality = market_data::synthetic::DataQuality::valid,
      .venue_number = 1U,
      .channel_number = 1U,
      .instrument_number = 1U,
      .channel_sequence = ordinal,
      .global_ordinal = ordinal,
      .exchange_event_time_ns =
          1'800'000'000'000'000'000LL + static_cast<std::int64_t>(ordinal),
      .nic_receive_time_ns =
          1'800'000'000'000'000'100LL + static_cast<std::int64_t>(ordinal),
      .process_monotonic_time_ns = now_ns};
  event.event_hash = market_data::synthetic::calculate_event_hash(event);
  return event;
}

[[nodiscard]] inline market_data::synthetic::SyntheticEvent
trade_event(const std::uint64_t ordinal, const std::uint64_t now_ns,
            const std::int64_t price_ticks = 98,
            const std::uint64_t quantity_units = 10U) noexcept {
  market_data::synthetic::SyntheticEvent event{
      .type = market_data::synthetic::NativeMessageType::trade,
      .side = market_data::synthetic::Side::ask,
      .action = market_data::synthetic::BookAction::none,
      .status = market_data::synthetic::TradingStatus::none,
      .data_quality = market_data::synthetic::DataQuality::valid,
      .venue_number = 1U,
      .channel_number = 1U,
      .instrument_number = 1U,
      .channel_sequence = ordinal,
      .global_ordinal = ordinal,
      .exchange_event_time_ns =
          1'800'000'000'000'000'000LL + static_cast<std::int64_t>(ordinal),
      .nic_receive_time_ns =
          1'800'000'000'000'000'100LL + static_cast<std::int64_t>(ordinal),
      .process_monotonic_time_ns = now_ns,
      .price_ticks = price_ticks,
      .quantity_units = quantity_units,
      .auxiliary_value = ordinal};
  event.event_hash = market_data::synthetic::calculate_event_hash(event);
  return event;
}

[[nodiscard]] inline market_data::synthetic::SyntheticEvent
auction_event(const std::uint64_t ordinal, const std::uint64_t now_ns,
              const std::int64_t price_ticks = 99,
              const std::uint64_t quantity_units = 10U) noexcept {
  auto event = trade_event(ordinal, now_ns, price_ticks, quantity_units);
  event.type = market_data::synthetic::NativeMessageType::auction_imbalance;
  event.side = market_data::synthetic::Side::none;
  event.auxiliary_value = 0U;
  event.event_hash = market_data::synthetic::calculate_event_hash(event);
  return event;
}

} // namespace aegis::execution::test

#endif // AEGIS_EXECUTION_TEST_SUPPORT_HPP
