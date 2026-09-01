#include "aegis/risk/serialization.hpp"

#include <cstdint>

namespace aegis::risk {
namespace {

namespace wire = common::wire;

[[nodiscard]] wire::RiskReasonCode to_wire(const RiskReason reason) noexcept {
  switch (reason) {
  case RiskReason::within_limits:
    return wire::RiskReasonCode::WITHIN_LIMITS;
  case RiskReason::invalid_intent:
    return wire::RiskReasonCode::INVALID_INTENT;
  case RiskReason::expired_intent:
    return wire::RiskReasonCode::EXPIRED_INTENT;
  case RiskReason::trading_mode_unauthorized:
    return wire::RiskReasonCode::TRADING_MODE_UNAUTHORIZED;
  case RiskReason::operator_session_unauthorized:
    return wire::RiskReasonCode::OPERATOR_SESSION_UNAUTHORIZED;
  case RiskReason::strategy_unauthorized:
    return wire::RiskReasonCode::STRATEGY_UNAUTHORIZED;
  case RiskReason::symbol_unauthorized:
    return wire::RiskReasonCode::SYMBOL_UNAUTHORIZED;
  case RiskReason::restricted_instrument:
    return wire::RiskReasonCode::RESTRICTED_INSTRUMENT;
  case RiskReason::market_state_unsafe:
    return wire::RiskReasonCode::MARKET_STATE_UNSAFE;
  case RiskReason::trading_halted:
    return wire::RiskReasonCode::TRADING_HALTED;
  case RiskReason::clock_unhealthy:
    return wire::RiskReasonCode::CLOCK_UNSYNCHRONIZED;
  case RiskReason::feed_book_unhealthy:
    return wire::RiskReasonCode::FEED_BOOK_UNHEALTHY;
  case RiskReason::order_quantity_exceeded:
    return wire::RiskReasonCode::ORDER_QUANTITY_EXCEEDED;
  case RiskReason::order_notional_exceeded:
    return wire::RiskReasonCode::ORDER_NOTIONAL_EXCEEDED;
  case RiskReason::price_collar_exceeded:
    return wire::RiskReasonCode::PRICE_COLLAR_EXCEEDED;
  case RiskReason::invalid_tick_size:
    return wire::RiskReasonCode::INVALID_TICK_SIZE;
  case RiskReason::duplicate_intent:
    return wire::RiskReasonCode::DUPLICATE_INTENT;
  case RiskReason::order_rate_exceeded:
    return wire::RiskReasonCode::ORDER_RATE_EXCEEDED;
  case RiskReason::cancel_rate_exceeded:
    return wire::RiskReasonCode::CANCEL_RATE_EXCEEDED;
  case RiskReason::symbol_position_exceeded:
    return wire::RiskReasonCode::SYMBOL_POSITION_EXCEEDED;
  case RiskReason::gross_exposure_exceeded:
    return wire::RiskReasonCode::GROSS_EXPOSURE_EXCEEDED;
  case RiskReason::net_exposure_exceeded:
    return wire::RiskReasonCode::NET_EXPOSURE_EXCEEDED;
  case RiskReason::sector_concentration_exceeded:
    return wire::RiskReasonCode::SECTOR_CONCENTRATION_EXCEEDED;
  case RiskReason::factor_exposure_exceeded:
    return wire::RiskReasonCode::FACTOR_EXPOSURE_EXCEEDED;
  case RiskReason::daily_loss_exceeded:
    return wire::RiskReasonCode::DAILY_LOSS_EXCEEDED;
  case RiskReason::strategy_loss_exceeded:
    return wire::RiskReasonCode::STRATEGY_LOSS_EXCEEDED;
  case RiskReason::drawdown_exceeded:
    return wire::RiskReasonCode::DRAWDOWN_EXCEEDED;
  case RiskReason::credit_capital_exceeded:
    return wire::RiskReasonCode::CREDIT_CAPITAL_EXCEEDED;
  case RiskReason::short_sale_locate_denied:
    return wire::RiskReasonCode::SHORT_SALE_LOCATE_DENIED;
  case RiskReason::self_trade_prevention_denied:
    return wire::RiskReasonCode::SELF_TRADE_PREVENTION_DENIED;
  case RiskReason::venue_unauthorized:
    return wire::RiskReasonCode::VENUE_UNAUTHORIZED;
  case RiskReason::kill_switch_engaged:
    return wire::RiskReasonCode::KILL_SWITCH_ENGAGED;
  case RiskReason::configuration_stale:
    return wire::RiskReasonCode::CONFIGURATION_STALE;
  case RiskReason::stale_position:
    return wire::RiskReasonCode::STALE_POSITION;
  case RiskReason::risk_state_unavailable:
    return wire::RiskReasonCode::RISK_STATE_UNAVAILABLE;
  case RiskReason::configuration_rollback:
    return wire::RiskReasonCode::CONFIGURATION_ROLLBACK;
  case RiskReason::split_brain_epoch:
    return wire::RiskReasonCode::SPLIT_BRAIN_EPOCH;
  case RiskReason::arithmetic_overflow:
    return wire::RiskReasonCode::ARITHMETIC_OVERFLOW;
  case RiskReason::journal_unavailable:
    return wire::RiskReasonCode::JOURNAL_UNAVAILABLE;
  case RiskReason::engine_busy:
    return wire::RiskReasonCode::ENGINE_BUSY;
  }
  return wire::RiskReasonCode::UNKNOWN;
}

[[nodiscard]] wire::Sha256Digest to_wire(const common::Sha256Digest& digest) noexcept {
  return wire::Sha256Digest{
      flatbuffers::span<const std::uint8_t, 32>{digest.data(), digest.size()}};
}

[[nodiscard]] wire::PortfolioHealthCode
to_wire(const portfolio::PortfolioHealth value) noexcept {
  return static_cast<wire::PortfolioHealthCode>(value);
}

[[nodiscard]] wire::PortfolioInvariantCode
to_wire(const portfolio::InvariantCode value) noexcept {
  if (value == portfolio::InvariantCode::none) {
    return wire::PortfolioInvariantCode::NONE;
  }
  return static_cast<wire::PortfolioInvariantCode>(static_cast<std::uint8_t>(value) +
                                                   1U);
}

} // namespace

flatbuffers::DetachedBuffer build_risk_decision_contract(const RiskDecision& decision) {
  if (!valid_risk_decision(decision)) {
    return {};
  }
  flatbuffers::FlatBufferBuilder builder{1024U};
  const wire::SchemaVersion version{common::kCurrentSchemaMajor,
                                    common::kCurrentSchemaMinor,
                                    common::kCurrentSchemaPatch};
  const wire::GlobalEventId event_id{decision.global_event_id.high(),
                                     decision.global_event_id.low()};
  const wire::IntentId intent_id{decision.intent_id.high(), decision.intent_id.low()};
  const wire::SessionId session_id{decision.session_id.high(),
                                   decision.session_id.low()};
  const wire::RiskSnapshotId snapshot_id{decision.risk_snapshot_id.high(),
                                         decision.risk_snapshot_id.low()};
  const wire::ConfigurationVersion configuration_version{
      decision.configuration_version.high(), decision.configuration_version.low()};
  const wire::AccountId account_id{decision.account_id.high(),
                                   decision.account_id.low()};
  const wire::StrategyId strategy_id{decision.strategy_id.high(),
                                     decision.strategy_id.low()};
  const wire::VenueId venue_id{decision.venue_id.high(), decision.venue_id.low()};
  const wire::InstrumentId instrument_id{decision.instrument_id.high(),
                                         decision.instrument_id.low()};
  const auto intent_sha256 = to_wire(decision.intent_sha256);
  const wire::ProcessMonotonicTimeNs evaluated_time{
      decision.evaluated_process_monotonic_time_ns};
  const wire::ProcessMonotonicTimeNs valid_until_time{
      decision.valid_until_process_monotonic_time_ns};
  const auto wire_reason = to_wire(decision.reason);
  const auto wire_reason_mask = std::uint64_t{1U}
                                << static_cast<std::uint16_t>(wire_reason);
  const auto risk_decision = wire::CreateRiskDecision(
      builder, &version, &event_id, &intent_id, &session_id, &snapshot_id,
      &configuration_version, static_cast<wire::RiskDecisionCode>(decision.decision),
      wire_reason, wire::QuantityUnit::INSTRUMENT_UNITS,
      decision.approved_quantity_units, &intent_sha256, &evaluated_time,
      &valid_until_time, &account_id, &strategy_id, &venue_id, &instrument_id,
      static_cast<wire::IntentAction>(decision.intent_action),
      static_cast<wire::TradingModeCode>(decision.trading_mode),
      static_cast<wire::RiskCheckCode>(decision.failed_check), wire::PriceUnit::TICKS,
      decision.approved_price_ticks, decision.intent_hash, decision.risk_snapshot_hash,
      decision.risk_context_hash, decision.position_generation,
      decision.authority_epoch, decision.policy_revision, decision.evaluation_ordinal,
      decision.journal_sequence, wire_reason_mask, decision.completed_check_mask,
      decision.stable_hash);
  const auto record = wire::CreateContractRecord(
      builder, &version, &event_id, wire::RecordType::RISK_DECISION,
      wire::ContractPayload::RiskDecision, risk_decision.Union());
  wire::FinishContractRecordBuffer(builder, record);
  return builder.Release();
}

flatbuffers::DetachedBuffer
build_position_snapshot_contract(const portfolio::PortfolioSnapshot& snapshot,
                                 const std::size_t position_index,
                                 const common::GlobalEventId global_event_id) {
  if (!global_event_id.valid() || !snapshot.session_id.valid() ||
      !snapshot.configuration_version.valid() || !snapshot.risk_snapshot_id.valid() ||
      !snapshot.ready || snapshot.health != portfolio::PortfolioHealth::healthy ||
      snapshot.source_journal_sequence == 0U ||
      position_index >= snapshot.position_count ||
      position_index >= snapshot.positions.size() || snapshot.stable_hash == 0U ||
      portfolio::stable_snapshot_hash(snapshot) != snapshot.stable_hash) {
    return {};
  }
  const auto& position = snapshot.positions[position_index];
  if (!position.account_id.valid() || !position.strategy_id.valid() ||
      !position.instrument_id.valid()) {
    return {};
  }
  const portfolio::InstrumentExposure* instrument = nullptr;
  for (std::size_t index = 0U; index < snapshot.instrument_count; ++index) {
    if (snapshot.instruments[index].instrument_id == position.instrument_id) {
      instrument = &snapshot.instruments[index];
      break;
    }
  }
  if (instrument == nullptr) {
    return {};
  }
  flatbuffers::FlatBufferBuilder builder{1024U};
  const wire::SchemaVersion version{common::kCurrentSchemaMajor,
                                    common::kCurrentSchemaMinor,
                                    common::kCurrentSchemaPatch};
  const wire::GlobalEventId event_id{global_event_id.high(), global_event_id.low()};
  const wire::SessionId session_id{snapshot.session_id.high(),
                                   snapshot.session_id.low()};
  const wire::AccountId account_id{position.account_id.high(),
                                   position.account_id.low()};
  const wire::StrategyId strategy_id{position.strategy_id.high(),
                                     position.strategy_id.low()};
  const wire::InstrumentId instrument_id{position.instrument_id.high(),
                                         position.instrument_id.low()};
  const wire::RiskSnapshotId risk_snapshot_id{snapshot.risk_snapshot_id.high(),
                                              snapshot.risk_snapshot_id.low()};
  const wire::ConfigurationVersion configuration_version{
      snapshot.configuration_version.high(), snapshot.configuration_version.low()};
  const wire::ProcessMonotonicTimeNs as_of_time{
      snapshot.as_of_process_monotonic_time_ns};
  const auto position_snapshot = wire::CreatePositionSnapshot(
      builder, &version, &event_id, &session_id, &strategy_id, &instrument_id,
      &risk_snapshot_id, &configuration_version, wire::QuantityUnit::INSTRUMENT_UNITS,
      position.net_quantity_units, wire::PriceUnit::TICKS, position.average_price_ticks,
      wire::CurrencyUnit::CURRENCY_NANOS, position.realized_pnl_currency_nanos,
      position.unrealized_pnl_currency_nanos, &as_of_time, &account_id,
      snapshot.sequence, snapshot.source_journal_sequence, snapshot.stable_hash,
      position.open_cost_currency_nanos, instrument->pending_buy_quantity_units,
      instrument->pending_sell_quantity_units, snapshot.gross_exposure_currency_nanos,
      snapshot.net_exposure_currency_nanos, snapshot.beta_exposure_currency_nanos,
      snapshot.liquidity_adjusted_exposure_currency_nanos,
      snapshot.event_exposure_currency_nanos, snapshot.drawdown_currency_nanos,
      to_wire(snapshot.health), to_wire(snapshot.invariant), snapshot.ready);
  const auto record = wire::CreateContractRecord(
      builder, &version, &event_id, wire::RecordType::POSITION_SNAPSHOT,
      wire::ContractPayload::PositionSnapshot, position_snapshot.Union());
  wire::FinishContractRecordBuffer(builder, record);
  return builder.Release();
}

} // namespace aegis::risk
