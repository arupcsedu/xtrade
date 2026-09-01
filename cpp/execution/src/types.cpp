#include "aegis/execution/types.hpp"

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

void mix_external(std::uint64_t& hash, const oms::ExternalOrderId& value) noexcept {
  mix(hash, value.high);
  mix(hash, value.low);
}

[[nodiscard]] std::uint64_t nonzero(const std::uint64_t value) noexcept {
  return value == 0U ? 1U : value;
}

[[nodiscard]] bool
valid_paper_configuration(const PaperModelConfiguration& value) noexcept {
  return value.deterministic_seed != 0U && value.acknowledgement_latency_ns != 0U &&
         value.cancel_latency_ns != 0U && value.replace_latency_ns != 0U &&
         value.maximum_fill_chunk_units != 0U &&
         value.impact_ticks_per_million_units <= kPartsPerMillion;
}

[[nodiscard]] bool valid_mapping(const SyntheticInstrumentMapping& value) noexcept {
  return value.venue_id.valid() && value.instrument_id.valid() &&
         value.synthetic_venue_number != 0U && value.synthetic_instrument_number != 0U;
}

[[nodiscard]] bool valid_paper_state(const PaperOrderState value) noexcept {
  return value >= PaperOrderState::pending_ack &&
         value <= PaperOrderState::unknown_recovery;
}

[[nodiscard]] bool valid_gateway_reason(const GatewayReason value) noexcept {
  return value >= GatewayReason::none && value <= GatewayReason::shutdown;
}

[[nodiscard]] bool response_reason_matches_kind(const GatewayEvent& value) noexcept {
  const bool rejection = value.kind == GatewayResponseKind::rejection ||
                         value.kind == GatewayResponseKind::cancel_rejection ||
                         value.kind == GatewayResponseKind::replace_rejection;
  if (rejection) {
    return value.reason != GatewayReason::none &&
           value.reason != GatewayReason::accepted;
  }
  if (value.kind == GatewayResponseKind::session_status) {
    return true;
  }
  return value.reason == GatewayReason::accepted;
}

[[nodiscard]] bool
valid_paper_order_snapshot(const PaperOrderSnapshot& order) noexcept {
  if (!order.source_command_id.valid() || !order.order_id.valid() ||
      (!order.external_order_id.valid() && order.state != PaperOrderState::rejected) ||
      !order.instrument_id.valid() || !valid_paper_state(order.state) ||
      (order.side != risk::IntentAction::buy &&
       order.side != risk::IntentAction::sell) ||
      order.price_ticks <= 0 || order.quantity_units == 0U ||
      order.cumulative_fill_quantity_units > order.quantity_units) {
    return false;
  }
  if (order.state == PaperOrderState::pending_ack &&
      order.acknowledgement_due_ns == 0U) {
    return false;
  }
  if (order.state == PaperOrderState::pending_cancel && order.cancel_due_ns == 0U) {
    return false;
  }
  return order.state != PaperOrderState::pending_replace ||
         (order.pending_replace_price_ticks > 0 &&
          order.pending_replace_quantity_units > order.cumulative_fill_quantity_units &&
          order.replace_due_ns != 0U);
}

[[nodiscard]] bool duplicate_recovery_order(const GatewayRecoverySnapshot& value,
                                            const std::size_t index) noexcept {
  for (std::size_t other = 0U; other < index; ++other) {
    if (value.orders[other].occupied &&
        value.orders[other].order_id == value.orders[index].order_id) {
      return true;
    }
  }
  return false;
}

} // namespace

bool valid_gateway_mode(const GatewayMode mode) noexcept {
  const bool local_mode = mode == GatewayMode::simulation ||
                          mode == GatewayMode::paper || mode == GatewayMode::halted;
#if AEGIS_LIVE_TRADING_COMPILED
  return local_mode || mode == GatewayMode::live_armed ||
         mode == GatewayMode::live_active;
#else
  return local_mode;
#endif
}

std::uint64_t
stable_gateway_configuration_hash(const GatewayConfiguration& value) noexcept {
  std::uint64_t hash = kFnvOffset;
  mix_identifier(hash, value.session_id);
  mix_identifier(hash, value.account_id);
  mix_identifier(hash, value.venue_id);
  mix_identifier(hash, value.configuration_version);
  mix(hash, static_cast<std::uint8_t>(value.startup_mode));
  mix(hash, value.exchange_session_epoch);
  mix(hash, value.initial_fencing_token);
  mix(hash, value.heartbeat_interval_ns);
  mix(hash, value.heartbeat_timeout_ns);
  mix(hash, value.maximum_clock_age_ns);
  mix(hash, value.command_rate_window_ns);
  mix(hash, value.maximum_new_orders_per_window);
  mix(hash, value.maximum_cancels_per_window);
  mix(hash, value.maximum_replaces_per_window);
  mix(hash, value.mapping_count);
  for (std::size_t index = 0U;
       index < value.mapping_count && index < value.mappings.size(); ++index) {
    mix_identifier(hash, value.mappings[index].venue_id);
    mix_identifier(hash, value.mappings[index].instrument_id);
    mix(hash, value.mappings[index].synthetic_venue_number);
    mix(hash, value.mappings[index].synthetic_instrument_number);
  }
  mix(hash, value.paper_model.deterministic_seed);
  mix(hash, value.paper_model.acknowledgement_latency_ns);
  mix(hash, value.paper_model.cancel_latency_ns);
  mix(hash, value.paper_model.replace_latency_ns);
  mix(hash, value.paper_model.initial_queue_ahead_units);
  mix(hash, value.paper_model.maximum_fill_chunk_units);
  mix(hash, value.paper_model.fee_per_unit_currency_nanos);
  mix(hash, value.paper_model.slippage_ticks);
  mix(hash, value.paper_model.impact_ticks_per_million_units);
  mix(hash, value.paper_model.reject_every_nth_new_order);
  mix(hash, value.paper_model.allow_auction_orders ? 1U : 0U);
  return nonzero(hash);
}

bool valid_gateway_configuration(const GatewayConfiguration& value) noexcept {
  if (!value.session_id.valid() || !value.account_id.valid() ||
      !value.venue_id.valid() || !value.configuration_version.valid() ||
      (value.startup_mode != GatewayMode::simulation &&
       value.startup_mode != GatewayMode::paper) ||
      value.exchange_session_epoch == 0U || value.initial_fencing_token == 0U ||
      value.heartbeat_interval_ns == 0U ||
      value.heartbeat_timeout_ns <= value.heartbeat_interval_ns ||
      value.maximum_clock_age_ns == 0U || value.command_rate_window_ns == 0U ||
      value.maximum_new_orders_per_window == 0U ||
      value.maximum_new_orders_per_window > kMaximumGatewayCommands ||
      value.maximum_cancels_per_window == 0U ||
      value.maximum_cancels_per_window > kMaximumGatewayCommands ||
      value.maximum_replaces_per_window == 0U || value.mapping_count == 0U ||
      value.maximum_replaces_per_window > kMaximumGatewayCommands ||
      value.mapping_count > value.mappings.size() ||
      !valid_paper_configuration(value.paper_model) || value.stable_hash == 0U ||
      value.stable_hash != stable_gateway_configuration_hash(value)) {
    return false;
  }
  for (std::size_t index = 0U; index < value.mapping_count; ++index) {
    if (!valid_mapping(value.mappings[index]) ||
        value.mappings[index].venue_id != value.venue_id) {
      return false;
    }
    for (std::size_t other = 0U; other < index; ++other) {
      if (value.mappings[index].instrument_id == value.mappings[other].instrument_id ||
          (value.mappings[index].synthetic_venue_number ==
               value.mappings[other].synthetic_venue_number &&
           value.mappings[index].synthetic_instrument_number ==
               value.mappings[other].synthetic_instrument_number)) {
        return false;
      }
    }
  }
  return true;
}

std::uint64_t stable_final_safety_state_hash(const FinalSafetyState& value) noexcept {
  std::uint64_t hash = kFnvOffset;
  mix(hash, value.observed_process_monotonic_time_ns);
  mix(hash, value.market_state_snapshot.stable_hash);
  mix(hash, static_cast<std::uint8_t>(value.official_trading_status));
  mix(hash, static_cast<std::uint8_t>(value.feed_health));
  mix(hash, static_cast<std::uint8_t>(value.book_validity));
  mix(hash, static_cast<std::uint8_t>(value.clock_quality_snapshot.state));
  mix(hash, static_cast<std::uint8_t>(value.clock_quality_snapshot.operation_mode));
  mix(hash, value.clock_quality_snapshot.observed_at.value());
  mix(hash, value.clock_quality_snapshot.last_synchronization_time.value());
  mix(hash, value.clock_quality_snapshot.source_id.high());
  mix(hash, value.clock_quality_snapshot.source_id.low());
  mix(hash, value.clock_quality_snapshot.hardware_timestamp_available ? 1U : 0U);
  mix(hash, value.authority.exchange_session_epoch);
  mix(hash, value.authority.fencing_token);
  mix(hash, value.effective_configuration_hash);
  mix(hash, value.activation_record_hash);
  mix(hash, value.operator_authorization_valid_until_ns);
  mix(hash, value.signed_configuration_valid ? 1U : 0U);
  mix(hash, value.operator_authorized ? 1U : 0U);
  mix(hash, value.activation_record_durable ? 1U : 0U);
  mix(hash, value.kill_switch_engaged ? 1U : 0U);
  mix(hash, value.journal_ready ? 1U : 0U);
  return nonzero(hash);
}

bool valid_final_safety_state(const FinalSafetyState& value) noexcept {
  return value.observed_process_monotonic_time_ns != 0U &&
         value.market_state_snapshot.valid() && value.authority.valid() &&
         value.effective_configuration_hash != 0U && value.stable_hash != 0U &&
         value.stable_hash == stable_final_safety_state_hash(value);
}

std::uint64_t stable_gateway_request_hash(const GatewayRequest& value) noexcept {
  std::uint64_t hash = kFnvOffset;
  mix(hash, value.command.stable_hash);
  mix(hash, value.risk_decision.stable_hash);
  mix(hash, value.safety.stable_hash);
  return nonzero(hash);
}

bool valid_gateway_request(const GatewayRequest& value) noexcept {
  return oms::valid_gateway_command(value.command) &&
         risk::valid_risk_decision(value.risk_decision) &&
         valid_final_safety_state(value.safety) && value.stable_hash != 0U &&
         value.stable_hash == stable_gateway_request_hash(value);
}

std::uint64_t
stable_gateway_submit_result_hash(const GatewaySubmitResult& value) noexcept {
  std::uint64_t hash = kFnvOffset;
  mix(hash, static_cast<std::uint8_t>(value.status));
  mix(hash, static_cast<std::uint8_t>(value.reason));
  mix(hash, value.outbound_sequence);
  mix(hash, value.journal_sequence);
  mix(hash, value.journal_record_hash);
  return nonzero(hash);
}

std::uint64_t stable_gateway_event_hash(const GatewayEvent& value) noexcept {
  std::uint64_t hash = kFnvOffset;
  mix_identifier(hash, value.event_id);
  mix_identifier(hash, value.source_command_id);
  mix_identifier(hash, value.execution_id);
  mix_identifier(hash, value.order_id);
  mix_external(hash, value.external_order_id);
  mix_identifier(hash, value.session_id);
  mix_identifier(hash, value.account_id);
  mix_identifier(hash, value.venue_id);
  mix_identifier(hash, value.instrument_id);
  mix_identifier(hash, value.configuration_version);
  mix(hash, value.authority.exchange_session_epoch);
  mix(hash, value.authority.fencing_token);
  mix(hash, static_cast<std::uint8_t>(value.mode));
  mix(hash, static_cast<std::uint8_t>(value.kind));
  mix(hash, static_cast<std::uint8_t>(value.reason));
  mix(hash, value.venue_sequence);
  mix(hash, std::bit_cast<std::uint64_t>(value.exchange_event_time_ns));
  mix(hash, std::bit_cast<std::uint64_t>(value.nic_receive_time_ns));
  mix(hash, value.process_monotonic_time_ns);
  mix(hash, std::bit_cast<std::uint64_t>(value.price_ticks));
  mix(hash, value.quantity_units);
  mix(hash, value.cumulative_fill_quantity_units);
  mix(hash, value.remaining_quantity_units);
  mix(hash, value.fee_currency_nanos);
  mix(hash, value.queue_ahead_before_units);
  mix(hash, value.queue_ahead_after_units);
  mix(hash, value.modeled_slippage_ticks);
  mix(hash, value.modeled_impact_ticks);
  mix(hash, value.source_market_event_hash);
  return nonzero(hash);
}

bool valid_gateway_event(const GatewayEvent& value) noexcept {
  if (!value.event_id.valid() || !value.session_id.valid() ||
      !value.account_id.valid() || !value.venue_id.valid() ||
      !value.configuration_version.valid() || !value.authority.valid() ||
      !valid_gateway_mode(value.mode) || value.mode == GatewayMode::halted ||
      value.kind < GatewayResponseKind::acknowledgement ||
      value.kind > GatewayResponseKind::session_status || value.venue_sequence == 0U ||
      !valid_gateway_reason(value.reason) || !response_reason_matches_kind(value) ||
      value.process_monotonic_time_ns == 0U || value.stable_hash == 0U ||
      value.stable_hash != stable_gateway_event_hash(value)) {
    return false;
  }
  if (value.kind == GatewayResponseKind::heartbeat ||
      value.kind == GatewayResponseKind::session_status) {
    return true;
  }
  if (!value.source_command_id.valid() || !value.order_id.valid() ||
      !value.instrument_id.valid()) {
    return false;
  }
  if (value.kind == GatewayResponseKind::fill) {
    return value.execution_id.valid() && value.external_order_id.valid() &&
           value.price_ticks > 0 && value.quantity_units != 0U &&
           value.cumulative_fill_quantity_units >= value.quantity_units;
  }
  return true;
}

std::uint64_t stable_session_snapshot_hash(const SessionSnapshot& value) noexcept {
  std::uint64_t hash = kFnvOffset;
  mix(hash, static_cast<std::uint8_t>(value.state));
  mix(hash, static_cast<std::uint8_t>(value.reason));
  mix(hash, value.exchange_session_epoch);
  mix(hash, value.fencing_token);
  mix(hash, value.next_outbound_sequence);
  mix(hash, value.expected_inbound_sequence);
  mix(hash, value.last_inbound_process_monotonic_time_ns);
  mix(hash, value.last_outbound_process_monotonic_time_ns);
  mix(hash, value.transition_count);
  return nonzero(hash);
}

std::uint64_t
stable_recovery_snapshot_hash(const GatewayRecoverySnapshot& value) noexcept {
  std::uint64_t hash = kFnvOffset;
  mix_identifier(hash, value.session_id);
  mix_identifier(hash, value.account_id);
  mix_identifier(hash, value.venue_id);
  mix_identifier(hash, value.configuration_version);
  mix(hash, value.configuration_hash);
  mix(hash, value.exchange_session_epoch);
  mix(hash, value.next_inbound_sequence);
  mix(hash, value.order_count);
  for (std::size_t index = 0U; index < value.orders.size(); ++index) {
    const auto& order = value.orders[index];
    mix(hash, index);
    mix(hash, order.occupied ? 1U : 0U);
    if (!order.occupied) {
      continue;
    }
    mix_identifier(hash, order.source_command_id);
    mix_identifier(hash, order.order_id);
    mix_external(hash, order.external_order_id);
    mix_identifier(hash, order.instrument_id);
    mix(hash, static_cast<std::uint8_t>(order.side));
    mix(hash, static_cast<std::uint8_t>(order.state));
    mix(hash, std::bit_cast<std::uint64_t>(order.price_ticks));
    mix(hash, order.quantity_units);
    mix(hash, order.cumulative_fill_quantity_units);
    mix(hash, order.queue_ahead_units);
    mix(hash, std::bit_cast<std::uint64_t>(order.pending_replace_price_ticks));
    mix(hash, order.pending_replace_quantity_units);
    mix(hash, order.acknowledgement_due_ns);
    mix(hash, order.cancel_due_ns);
    mix(hash, order.replace_due_ns);
  }
  return nonzero(hash);
}

bool valid_recovery_snapshot(const GatewayRecoverySnapshot& value) noexcept {
  if (!value.session_id.valid() || !value.account_id.valid() ||
      !value.venue_id.valid() || !value.configuration_version.valid() ||
      value.configuration_hash == 0U || value.exchange_session_epoch == 0U ||
      value.next_inbound_sequence == 0U || value.order_count > value.orders.size() ||
      value.stable_hash == 0U ||
      value.stable_hash != stable_recovery_snapshot_hash(value)) {
    return false;
  }
  std::uint32_t count{};
  for (std::size_t index = 0U; index < value.orders.size(); ++index) {
    const auto& order = value.orders[index];
    if (!order.occupied) {
      continue;
    }
    if (!valid_paper_order_snapshot(order) || duplicate_recovery_order(value, index)) {
      return false;
    }
    ++count;
  }
  return count == value.order_count;
}

std::uint64_t
stable_gateway_audit_record_hash(const GatewayAuditRecord& value) noexcept {
  std::uint64_t hash = kFnvOffset;
  mix(hash, value.sequence);
  mix(hash, static_cast<std::uint8_t>(value.kind));
  mix(hash, static_cast<std::uint8_t>(value.mode));
  mix(hash, static_cast<std::uint8_t>(value.session_state));
  mix(hash, static_cast<std::uint8_t>(value.reason));
  mix_identifier(hash, value.correlation_id);
  mix_identifier(hash, value.order_id);
  mix(hash, value.command_hash);
  mix(hash, value.risk_decision_hash);
  mix(hash, value.safety_state_hash);
  mix(hash, value.gateway_event_hash);
  mix(hash, value.process_monotonic_time_ns);
  mix(hash, value.previous_record_hash);
  return nonzero(hash);
}

std::string_view gateway_mode_name(const GatewayMode mode) noexcept {
  switch (mode) {
  case GatewayMode::simulation:
    return "SIMULATION";
  case GatewayMode::paper:
    return "PAPER";
  case GatewayMode::halted:
    return "HALTED";
#if AEGIS_LIVE_TRADING_COMPILED
  case GatewayMode::live_armed:
    return "LIVE_ARMED";
  case GatewayMode::live_active:
    return "LIVE_ACTIVE";
#endif
  }
  return "INVALID";
}

std::string_view gateway_reason_name(const GatewayReason reason) noexcept {
  switch (reason) {
  case GatewayReason::none:
    return "none";
  case GatewayReason::accepted:
    return "accepted";
  case GatewayReason::invalid_configuration:
    return "invalid_configuration";
  case GatewayReason::invalid_request:
    return "invalid_request";
  case GatewayReason::invalid_command:
    return "invalid_command";
  case GatewayReason::command_kind_mismatch:
    return "command_kind_mismatch";
  case GatewayReason::duplicate_command:
    return "duplicate_command";
  case GatewayReason::command_identity_conflict:
    return "command_identity_conflict";
  case GatewayReason::session_not_active:
    return "session_not_active";
  case GatewayReason::mode_mismatch:
    return "mode_mismatch";
  case GatewayReason::scope_mismatch:
    return "scope_mismatch";
  case GatewayReason::stale_fencing_token:
    return "stale_fencing_token";
  case GatewayReason::risk_decision_invalid:
    return "risk_decision_invalid";
  case GatewayReason::risk_decision_rejected:
    return "risk_decision_rejected";
  case GatewayReason::risk_decision_expired:
    return "risk_decision_expired";
  case GatewayReason::risk_hash_mismatch:
    return "risk_hash_mismatch";
  case GatewayReason::market_state_unsafe:
    return "market_state_unsafe";
  case GatewayReason::clock_unhealthy:
    return "clock_unhealthy";
  case GatewayReason::feed_unhealthy:
    return "feed_unhealthy";
  case GatewayReason::book_invalid:
    return "book_invalid";
  case GatewayReason::trading_halted:
    return "trading_halted";
  case GatewayReason::kill_switch_engaged:
    return "kill_switch_engaged";
  case GatewayReason::signed_configuration_required:
    return "signed_configuration_required";
  case GatewayReason::operator_authorization_required:
    return "operator_authorization_required";
  case GatewayReason::activation_record_required:
    return "activation_record_required";
  case GatewayReason::rate_limited:
    return "rate_limited";
  case GatewayReason::order_capacity:
    return "order_capacity";
  case GatewayReason::event_capacity:
    return "event_capacity";
  case GatewayReason::journal_capacity:
    return "journal_capacity";
  case GatewayReason::order_not_found:
    return "order_not_found";
  case GatewayReason::paper_policy_reject:
    return "paper_policy_reject";
  case GatewayReason::sequence_gap:
    return "sequence_gap";
  case GatewayReason::heartbeat_timeout:
    return "heartbeat_timeout";
  case GatewayReason::recovery_required:
    return "recovery_required";
  case GatewayReason::recovery_ambiguous:
    return "recovery_ambiguous";
  case GatewayReason::decode_failure:
    return "decode_failure";
  case GatewayReason::unsupported_protocol:
    return "unsupported_protocol";
  case GatewayReason::arithmetic_overflow:
    return "arithmetic_overflow";
  case GatewayReason::shutdown:
    return "shutdown";
  }
  return "invalid";
}

} // namespace aegis::execution
