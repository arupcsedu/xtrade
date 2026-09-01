#include "aegis/oms/serialization.hpp"

#include <bit>
#include <cstdint>

namespace aegis::oms {
namespace {

namespace wire = common::wire;

[[nodiscard]] wire::OrderEventCode broad_event(const OmsInput& input,
                                               const ApplyResult& result) noexcept {
  switch (input.kind) {
  case InputKind::accept_intent:
  case InputKind::mark_ready:
  case InputKind::recovery_begin:
  case InputKind::recovery_observation:
  case InputKind::authority_update:
    return wire::OrderEventCode::ACCEPTED_BY_OMS;
  case InputKind::dispatch:
  case InputKind::replace_intent:
    return wire::OrderEventCode::SUBMITTED_TO_GATEWAY;
  case InputKind::acknowledgement:
  case InputKind::replace_acknowledgement:
    return wire::OrderEventCode::ACKNOWLEDGED;
  case InputKind::fill:
    return result.current_state == OrderState::filled
               ? wire::OrderEventCode::FILLED
               : wire::OrderEventCode::PARTIALLY_FILLED;
  case InputKind::cancel_intent:
    return wire::OrderEventCode::CANCEL_PENDING;
  case InputKind::cancel_acknowledgement:
    return wire::OrderEventCode::CANCELLED;
  case InputKind::rejection:
  case InputKind::cancel_rejection:
  case InputKind::replace_rejection:
  case InputKind::expire:
    return wire::OrderEventCode::REJECTED;
  }
  return wire::OrderEventCode::UNKNOWN;
}

[[nodiscard]] std::uint64_t event_hash(const OmsInput& input, const ApplyResult& result,
                                       const OmsOrderSnapshot& order) noexcept {
  auto value = input.stable_hash ^ std::rotl(result.stable_hash, 17) ^
               std::rotl(stable_order_snapshot_hash(order), 31);
  return value == 0U ? 1U : value;
}

} // namespace

flatbuffers::DetachedBuffer build_order_event_contract(const OmsInput& input,
                                                       const ApplyResult& result,
                                                       const OmsOrderSnapshot& order) {
  if (!valid_input(input) || result.stable_hash == 0U ||
      result.stable_hash != stable_apply_result_hash(result) ||
      result.order_id != order.order_id || !valid_order_snapshot(order) ||
      order.quantity_units == 0U || order.price_ticks <= 0 ||
      result.journal_sequence == 0U || result.snapshot_sequence == 0U ||
      result.journal_record_hash == 0U || result.snapshot_hash == 0U) {
    return {};
  }
  flatbuffers::FlatBufferBuilder builder{2048U};
  const wire::SchemaVersion version{common::kCurrentSchemaMajor,
                                    common::kCurrentSchemaMinor,
                                    common::kCurrentSchemaPatch};
  const wire::GlobalEventId event_id{input.receipt_id.high(), input.receipt_id.low()};
  const wire::OrderId order_id{order.order_id.high(), order.order_id.low()};
  const wire::IntentId intent_id{order.intent_id.high(), order.intent_id.low()};
  const wire::SessionId session_id{order.session_id.high(), order.session_id.low()};
  const wire::VenueId venue_id{order.venue_id.high(), order.venue_id.low()};
  const wire::InstrumentId instrument_id{order.instrument_id.high(),
                                         order.instrument_id.low()};
  const wire::ConfigurationVersion configuration_version{
      order.configuration_version.high(), order.configuration_version.low()};
  const wire::AccountId account_id{order.account_id.high(), order.account_id.low()};
  const wire::StrategyId strategy_id{order.strategy_id.high(), order.strategy_id.low()};
  const wire::ProcessMonotonicTimeNs process_time{input.process_monotonic_time_ns};
  const wire::ExchangeEventTimeNs exchange_time{input.exchange_event_time_ns};
  const wire::NicReceiveTimeNs nic_time{input.nic_receive_time_ns};
  const auto client_order_id =
      builder.CreateString(order.client_order_id.data(), kClientOrderIdBytes - 1U);

  wire::OrderEventBuilder event_builder(builder);
  event_builder.add_schema_version(&version);
  event_builder.add_global_event_id(&event_id);
  event_builder.add_order_id(&order_id);
  event_builder.add_intent_id(&intent_id);
  event_builder.add_session_id(&session_id);
  event_builder.add_venue_id(&venue_id);
  event_builder.add_instrument_id(&instrument_id);
  event_builder.add_configuration_version(&configuration_version);
  event_builder.add_event(broad_event(input, result));
  event_builder.add_price_unit(wire::PriceUnit::TICKS);
  event_builder.add_price_ticks(order.price_ticks);
  event_builder.add_quantity_unit(wire::QuantityUnit::INSTRUMENT_UNITS);
  event_builder.add_order_quantity_units(order.quantity_units);
  event_builder.add_cumulative_fill_quantity_units(
      order.cumulative_fill_quantity_units);
  event_builder.add_remaining_quantity_units(order.remaining_quantity_units);
  if (input.exchange_event_time_ns > 0 && input.nic_receive_time_ns > 0) {
    event_builder.add_exchange_event_time(&exchange_time);
    event_builder.add_nic_receive_time(&nic_time);
  }
  event_builder.add_process_monotonic_time(&process_time);
  event_builder.add_reason_code(static_cast<std::uint32_t>(result.reason));
  event_builder.add_account_id(&account_id);
  event_builder.add_strategy_id(&strategy_id);
  event_builder.add_input_kind(static_cast<wire::OmsInputKindCode>(input.kind));
  event_builder.add_source(static_cast<wire::OmsEventSourceCode>(input.source));
  event_builder.add_outcome(static_cast<wire::OmsApplyStatusCode>(result.status));
  event_builder.add_previous_state(
      static_cast<wire::OmsOrderStateCode>(result.previous_state));
  event_builder.add_current_state(
      static_cast<wire::OmsOrderStateCode>(result.current_state));
  event_builder.add_state_version(result.state_version);
  event_builder.add_exchange_session_epoch(order.authority.exchange_session_epoch);
  event_builder.add_active_fencing_token(order.authority.fencing_token);
  event_builder.add_external_order_id_high(order.external_order_id.high);
  event_builder.add_external_order_id_low(order.external_order_id.low);
  event_builder.add_client_order_id(client_order_id);
  event_builder.add_risk_decision_hash(order.risk_decision_hash);
  event_builder.add_journal_sequence(result.journal_sequence);
  event_builder.add_journal_record_hash(result.journal_record_hash);
  event_builder.add_snapshot_sequence(result.snapshot_sequence);
  event_builder.add_snapshot_hash(result.snapshot_hash);
  event_builder.add_order_event_hash(event_hash(input, result, order));
  const auto order_event = event_builder.Finish();
  const auto record = wire::CreateContractRecord(
      builder, &version, &event_id, wire::RecordType::ORDER_EVENT,
      wire::ContractPayload::OrderEvent, order_event.Union());
  wire::FinishContractRecordBuffer(builder, record);
  return builder.Release();
}

} // namespace aegis::oms
