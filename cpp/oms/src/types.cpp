#include "aegis/oms/types.hpp"

#include <bit>
#include <cstdint>
#include <limits>

namespace aegis::oms {
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
void mix_identifier(std::uint64_t& hash, const Identifier value) noexcept {
  mix(hash, value.high());
  mix(hash, value.low());
}

void mix_external(std::uint64_t& hash, const ExternalOrderId value) noexcept {
  mix(hash, value.high);
  mix(hash, value.low);
}

template <std::size_t Size>
void mix_chars(std::uint64_t& hash, const std::array<char, Size>& value) noexcept {
  for (const char item : value) {
    hash ^= static_cast<std::uint8_t>(item);
    hash *= kFnvPrime;
  }
}

[[nodiscard]] std::uint64_t nonzero_hash(const std::uint64_t value) noexcept {
  return value == 0U ? 1U : value;
}

[[nodiscard]] bool valid_input_kind(const InputKind value) noexcept {
  return value >= InputKind::accept_intent && value <= InputKind::authority_update;
}

[[nodiscard]] bool valid_event_source(const EventSource value) noexcept {
  return value >= EventSource::internal && value <= EventSource::recovery;
}

[[nodiscard]] bool
valid_recovery_observation(const RecoveryObservation value) noexcept {
  return value >= RecoveryObservation::working &&
         value <= RecoveryObservation::absent_ambiguous;
}

[[nodiscard]] bool exact_scope(const risk::RiskIntent& intent,
                               const risk::RiskDecision& decision) noexcept {
  return decision.intent_id == intent.intent_id &&
         decision.session_id == intent.session_id &&
         decision.account_id == intent.account_id &&
         decision.strategy_id == intent.strategy_id &&
         decision.venue_id == intent.venue_id &&
         decision.instrument_id == intent.instrument_id &&
         decision.configuration_version == intent.configuration_version &&
         decision.intent_action == intent.action &&
         decision.intent_hash == intent.stable_hash &&
         decision.intent_sha256 == intent.canonical_sha256;
}

[[nodiscard]] constexpr std::uint16_t state_bit(const OrderState value) noexcept {
  return static_cast<std::uint16_t>(std::uint16_t{1U}
                                    << (static_cast<std::uint8_t>(value) - 1U));
}

[[nodiscard]] constexpr std::size_t transition_index(const OrderState from,
                                                     const InputKind input) noexcept {
  return ((static_cast<std::size_t>(input) - 1U) * std::size_t{12U}) +
         (static_cast<std::size_t>(from) - 1U);
}

inline constexpr auto kTransitionTable = [] {
  std::array<std::uint16_t, std::size_t{12U} * std::size_t{16U}> table{};
  const auto allow = [&table](const OrderState from, const InputKind input,
                              const std::uint16_t targets) {
    table[transition_index(from, input)] = targets;
  };
  allow(OrderState::created, InputKind::mark_ready, state_bit(OrderState::ready));
  allow(OrderState::ready, InputKind::dispatch, state_bit(OrderState::pending_ack));
  const auto cancel_target = state_bit(OrderState::pending_cancel);
  allow(OrderState::pending_ack, InputKind::cancel_intent, cancel_target);
  allow(OrderState::working, InputKind::cancel_intent, cancel_target);
  allow(OrderState::partially_filled, InputKind::cancel_intent, cancel_target);
  allow(OrderState::pending_replace, InputKind::cancel_intent, cancel_target);
  allow(OrderState::working, InputKind::replace_intent,
        state_bit(OrderState::pending_replace));
  allow(OrderState::partially_filled, InputKind::replace_intent,
        state_bit(OrderState::pending_replace));
  allow(OrderState::pending_ack, InputKind::acknowledgement,
        state_bit(OrderState::working));
  allow(OrderState::partially_filled, InputKind::acknowledgement,
        state_bit(OrderState::partially_filled));
  allow(OrderState::pending_cancel, InputKind::acknowledgement, cancel_target);
  allow(OrderState::pending_replace, InputKind::acknowledgement,
        state_bit(OrderState::pending_replace));
  allow(OrderState::pending_ack, InputKind::rejection, state_bit(OrderState::rejected));
  const auto partial_or_filled = static_cast<std::uint16_t>(
      state_bit(OrderState::partially_filled) | state_bit(OrderState::filled));
  allow(OrderState::pending_ack, InputKind::fill, partial_or_filled);
  allow(OrderState::working, InputKind::fill, partial_or_filled);
  allow(OrderState::partially_filled, InputKind::fill, partial_or_filled);
  allow(OrderState::pending_cancel, InputKind::fill,
        static_cast<std::uint16_t>(cancel_target | state_bit(OrderState::filled)));
  allow(OrderState::pending_replace, InputKind::fill,
        static_cast<std::uint16_t>(state_bit(OrderState::pending_replace) |
                                   state_bit(OrderState::filled)));
  allow(OrderState::canceled, InputKind::fill,
        static_cast<std::uint16_t>(state_bit(OrderState::canceled) |
                                   state_bit(OrderState::filled)));
  allow(OrderState::unknown_recovery, InputKind::fill,
        state_bit(OrderState::unknown_recovery));
  const auto canceled = state_bit(OrderState::canceled);
  allow(OrderState::pending_ack, InputKind::cancel_acknowledgement, canceled);
  allow(OrderState::working, InputKind::cancel_acknowledgement, canceled);
  allow(OrderState::partially_filled, InputKind::cancel_acknowledgement, canceled);
  allow(OrderState::pending_cancel, InputKind::cancel_acknowledgement, canceled);
  allow(OrderState::pending_replace, InputKind::cancel_acknowledgement, canceled);
  allow(OrderState::filled, InputKind::cancel_acknowledgement,
        state_bit(OrderState::filled));
  const auto open_states = static_cast<std::uint16_t>(
      state_bit(OrderState::working) | state_bit(OrderState::partially_filled));
  allow(OrderState::pending_cancel, InputKind::cancel_rejection, open_states);
  allow(OrderState::pending_replace, InputKind::replace_acknowledgement,
        static_cast<std::uint16_t>(open_states | state_bit(OrderState::filled)));
  allow(OrderState::pending_replace, InputKind::replace_rejection, open_states);
  const auto expired = state_bit(OrderState::expired);
  allow(OrderState::created, InputKind::expire, expired);
  allow(OrderState::ready, InputKind::expire, expired);
  allow(OrderState::pending_ack, InputKind::expire, expired);
  allow(OrderState::working, InputKind::expire, expired);
  allow(OrderState::partially_filled, InputKind::expire, expired);
  const auto unknown = state_bit(OrderState::unknown_recovery);
  allow(OrderState::created, InputKind::recovery_begin, unknown);
  allow(OrderState::ready, InputKind::recovery_begin, unknown);
  allow(OrderState::pending_ack, InputKind::recovery_begin, unknown);
  allow(OrderState::working, InputKind::recovery_begin, unknown);
  allow(OrderState::partially_filled, InputKind::recovery_begin, unknown);
  allow(OrderState::pending_cancel, InputKind::recovery_begin, unknown);
  allow(OrderState::pending_replace, InputKind::recovery_begin, unknown);
  allow(OrderState::unknown_recovery, InputKind::recovery_begin, unknown);
  const auto recovery_states = static_cast<std::uint16_t>(
      state_bit(OrderState::working) | state_bit(OrderState::partially_filled) |
      state_bit(OrderState::filled) | state_bit(OrderState::canceled) |
      state_bit(OrderState::rejected) | state_bit(OrderState::expired) | unknown);
  allow(OrderState::unknown_recovery, InputKind::recovery_observation, recovery_states);
  return table;
}();

[[nodiscard]] bool valid_input_source(const OmsInput& value) noexcept {
  switch (value.kind) {
  case InputKind::accept_intent:
  case InputKind::mark_ready:
  case InputKind::dispatch:
  case InputKind::cancel_intent:
  case InputKind::replace_intent:
  case InputKind::recovery_begin:
  case InputKind::authority_update:
    return value.source == EventSource::internal;
  case InputKind::acknowledgement:
  case InputKind::rejection:
  case InputKind::fill:
  case InputKind::cancel_acknowledgement:
  case InputKind::cancel_rejection:
  case InputKind::replace_acknowledgement:
  case InputKind::replace_rejection:
    return value.source == EventSource::gateway ||
           value.source == EventSource::drop_copy;
  case InputKind::recovery_observation:
    return value.source == EventSource::recovery;
  case InputKind::expire:
    return value.source == EventSource::internal ||
           value.source == EventSource::gateway;
  }
  return false;
}

[[nodiscard]] bool valid_accept_input(const OmsInput& value) noexcept {
  return value.intent.action != risk::IntentAction::cancel &&
         exact_risk_approval(value.intent, value.risk_decision,
                             value.process_monotonic_time_ns);
}

[[nodiscard]] bool valid_cancel_input(const OmsInput& value) noexcept {
  return value.order_id.valid() && value.intent.action == risk::IntentAction::cancel &&
         value.intent.target_order_id == value.order_id && value.authority.valid() &&
         exact_risk_approval(value.intent, value.risk_decision,
                             value.process_monotonic_time_ns);
}

[[nodiscard]] bool valid_replace_input(const OmsInput& value) noexcept {
  return value.order_id.valid() && value.intent.action != risk::IntentAction::cancel &&
         value.authority.valid() &&
         exact_risk_approval(value.intent, value.risk_decision,
                             value.process_monotonic_time_ns);
}

[[nodiscard]] bool valid_fill_input(const OmsInput& value) noexcept {
  return (value.order_id.valid() || value.external_order_id.valid()) &&
         value.execution_id.valid() && value.authority.valid() &&
         value.price_ticks > 0 && value.quantity_units != 0U &&
         value.venue_sequence != 0U && value.exchange_event_time_ns > 0 &&
         value.nic_receive_time_ns > 0;
}

[[nodiscard]] bool valid_ack_input(const OmsInput& value) noexcept {
  return (value.order_id.valid() || value.external_order_id.valid()) &&
         value.external_order_id.valid() && value.authority.valid() &&
         value.venue_sequence != 0U;
}

[[nodiscard]] bool valid_recovery_input(const OmsInput& value) noexcept {
  return value.authority.valid() &&
         (value.order_id.valid() || value.external_order_id.valid()) &&
         valid_recovery_observation(value.recovery_observation);
}

} // namespace

bool valid_order_state(const OrderState value) noexcept {
  return value >= OrderState::created && value <= OrderState::unknown_recovery;
}

bool terminal_state(const OrderState value) noexcept {
  return value == OrderState::filled || value == OrderState::canceled ||
         value == OrderState::rejected || value == OrderState::expired;
}

bool live_state(const OrderState value) noexcept {
  return valid_order_state(value) && !terminal_state(value) &&
         value != OrderState::unknown_recovery;
}

std::uint64_t stable_configuration_hash(OmsConfiguration value) noexcept {
  value.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix_identifier(hash, value.session_id);
  mix_identifier(hash, value.account_id);
  mix_identifier(hash, value.configuration_version);
  mix(hash, static_cast<std::uint8_t>(value.trading_mode));
  mix(hash, value.exchange_session_epoch);
  mix(hash, value.initial_fencing_token);
  mix(hash, value.maximum_order_age_ns);
  mix(hash, value.require_drop_copy ? 1U : 0U);
  return nonzero_hash(hash);
}

bool valid_configuration(const OmsConfiguration& value) noexcept {
  return value.session_id.valid() && value.account_id.valid() &&
         value.configuration_version.valid() &&
         (value.trading_mode == risk::TradingMode::simulation ||
          value.trading_mode == risk::TradingMode::paper) &&
         value.exchange_session_epoch != 0U && value.initial_fencing_token != 0U &&
         value.maximum_order_age_ns != 0U && value.stable_hash != 0U &&
         value.stable_hash == stable_configuration_hash(value);
}

bool exact_risk_approval(const risk::RiskIntent& intent,
                         const risk::RiskDecision& decision,
                         const std::uint64_t now_ns) noexcept {
  if (!risk::valid_risk_intent(intent) || !risk::valid_risk_decision(decision) ||
      decision.decision != risk::DecisionCode::approved ||
      decision.reason != risk::RiskReason::within_limits ||
      !exact_scope(intent, decision) ||
      now_ns < decision.evaluated_process_monotonic_time_ns ||
      now_ns > decision.valid_until_process_monotonic_time_ns ||
      now_ns > intent.expire_process_monotonic_time_ns) {
    return false;
  }
  if (intent.action == risk::IntentAction::cancel) {
    return decision.approved_quantity_units == 0U && decision.approved_price_ticks == 0;
  }
  return decision.approved_quantity_units == intent.quantity_units &&
         decision.approved_price_ticks == intent.limit_price_ticks;
}

std::uint64_t stable_input_hash(OmsInput value) noexcept {
  value.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix_identifier(hash, value.receipt_id);
  mix(hash, static_cast<std::uint8_t>(value.kind));
  mix(hash, static_cast<std::uint8_t>(value.source));
  mix_identifier(hash, value.order_id);
  mix_external(hash, value.external_order_id);
  mix_identifier(hash, value.execution_id);
  mix(hash, value.intent.stable_hash);
  mix(hash, value.risk_decision.stable_hash);
  mix(hash, value.authority.exchange_session_epoch);
  mix(hash, value.authority.fencing_token);
  mix(hash, static_cast<std::uint8_t>(value.recovery_observation));
  mix(hash, std::bit_cast<std::uint64_t>(value.price_ticks));
  mix(hash, value.quantity_units);
  mix(hash, value.venue_sequence);
  mix(hash, std::bit_cast<std::uint64_t>(value.exchange_event_time_ns));
  mix(hash, std::bit_cast<std::uint64_t>(value.nic_receive_time_ns));
  mix(hash, value.process_monotonic_time_ns);
  return nonzero_hash(hash);
}

bool valid_input(const OmsInput& value) noexcept {
  if (!value.receipt_id.valid() || !valid_input_kind(value.kind) ||
      !valid_event_source(value.source) || value.process_monotonic_time_ns == 0U ||
      value.stable_hash == 0U || value.stable_hash != stable_input_hash(value) ||
      !valid_input_source(value)) {
    return false;
  }
  switch (value.kind) {
  case InputKind::accept_intent:
    return valid_accept_input(value);
  case InputKind::cancel_intent:
    return valid_cancel_input(value);
  case InputKind::replace_intent:
    return valid_replace_input(value);
  case InputKind::mark_ready:
  case InputKind::dispatch:
  case InputKind::recovery_begin:
    return value.order_id.valid() && value.authority.valid();
  case InputKind::authority_update:
    return value.authority.valid() && !value.order_id.valid();
  case InputKind::fill:
    return valid_fill_input(value);
  case InputKind::acknowledgement:
    return valid_ack_input(value);
  case InputKind::recovery_observation:
    return valid_recovery_input(value);
  case InputKind::expire:
    return value.order_id.valid() && value.authority.valid() &&
           (value.source == EventSource::internal ||
            value.source == EventSource::gateway);
  case InputKind::rejection:
  case InputKind::cancel_acknowledgement:
  case InputKind::cancel_rejection:
  case InputKind::replace_acknowledgement:
  case InputKind::replace_rejection:
    return (value.order_id.valid() || value.external_order_id.valid()) &&
           value.authority.valid() && value.venue_sequence != 0U;
  }
  return false;
}

common::OrderId deterministic_order_id(const OmsConfiguration& configuration,
                                       const risk::RiskIntent& intent) noexcept {
  std::uint64_t first = kFnvOffset;
  mix_identifier(first, configuration.session_id);
  mix_identifier(first, intent.intent_id);
  mix(first, configuration.exchange_session_epoch);
  mix(first, intent.stable_hash);
  std::uint64_t second = kFnvOffset;
  mix_identifier(second, configuration.account_id);
  mix_identifier(second, intent.strategy_id);
  mix_identifier(second, intent.instrument_id);
  mix(second, first);
  return {nonzero_hash(first), nonzero_hash(second)};
}

std::array<char, kClientOrderIdBytes>
deterministic_client_order_id(const common::OrderId order_id) noexcept {
  return common::to_hex(order_id);
}

std::uint64_t stable_gateway_command_hash(GatewayCommand value) noexcept {
  value.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix_identifier(hash, value.command_id);
  mix(hash, static_cast<std::uint8_t>(value.kind));
  mix_identifier(hash, value.order_id);
  mix_external(hash, value.external_order_id);
  mix_chars(hash, value.client_order_id);
  mix(hash, value.authority.exchange_session_epoch);
  mix(hash, value.authority.fencing_token);
  mix_identifier(hash, value.session_id);
  mix_identifier(hash, value.account_id);
  mix_identifier(hash, value.strategy_id);
  mix_identifier(hash, value.venue_id);
  mix_identifier(hash, value.instrument_id);
  mix_identifier(hash, value.configuration_version);
  mix(hash, static_cast<std::uint8_t>(value.side));
  mix(hash, std::bit_cast<std::uint64_t>(value.price_ticks));
  mix(hash, value.quantity_units);
  mix(hash, value.cumulative_fill_quantity_units);
  mix(hash, value.authority.exchange_session_epoch);
  mix(hash, value.authority.fencing_token);
  mix(hash, value.intent_hash);
  mix(hash, value.risk_decision_hash);
  mix(hash, value.generated_process_monotonic_time_ns);
  return nonzero_hash(hash);
}

bool valid_gateway_command(const GatewayCommand& value) noexcept {
  // The provider-neutral command always carries the deterministic client ID;
  // an external ID is unavailable before acknowledgement and is not invented.
  return value.kind >= GatewayCommandKind::new_order &&
         value.kind <= GatewayCommandKind::replace && value.command_id.valid() &&
         value.order_id.valid() && value.session_id.valid() &&
         value.account_id.valid() && value.strategy_id.valid() &&
         value.venue_id.valid() && value.instrument_id.valid() &&
         value.configuration_version.valid() && value.authority.valid() &&
         value.price_ticks > 0 && value.quantity_units != 0U &&
         value.intent_hash != 0U && value.risk_decision_hash != 0U &&
         value.generated_process_monotonic_time_ns != 0U &&
         value.client_order_id.back() == '\0' && value.stable_hash != 0U &&
         value.stable_hash == stable_gateway_command_hash(value);
}

bool transition_allowed(const OrderState from, const InputKind input,
                        const OrderState to) noexcept {
  if (!valid_order_state(from) || !valid_input_kind(input) || !valid_order_state(to)) {
    return false;
  }
  return (kTransitionTable[transition_index(from, input)] & state_bit(to)) != 0U;
}

std::uint64_t stable_order_snapshot_hash(OmsOrderSnapshot value) noexcept {
  std::uint64_t hash = kFnvOffset;
  mix_identifier(hash, value.order_id);
  mix_identifier(hash, value.intent_id);
  mix_identifier(hash, value.session_id);
  mix_identifier(hash, value.account_id);
  mix_identifier(hash, value.strategy_id);
  mix_identifier(hash, value.venue_id);
  mix_identifier(hash, value.instrument_id);
  mix_identifier(hash, value.configuration_version);
  mix_external(hash, value.external_order_id);
  mix_chars(hash, value.client_order_id);
  mix(hash, static_cast<std::uint8_t>(value.state));
  mix(hash, static_cast<std::uint8_t>(value.state_before_recovery));
  mix(hash, static_cast<std::uint8_t>(value.side));
  mix(hash, std::bit_cast<std::uint64_t>(value.price_ticks));
  mix(hash, value.quantity_units);
  mix(hash, value.cumulative_fill_quantity_units);
  mix(hash, value.remaining_quantity_units);
  mix(hash, std::bit_cast<std::uint64_t>(value.pending_replace_price_ticks));
  mix(hash, value.pending_replace_quantity_units);
  mix(hash, value.state_version);
  mix(hash, value.last_venue_sequence);
  mix(hash, value.last_update_process_monotonic_time_ns);
  mix(hash, value.intent_hash);
  mix(hash, value.risk_decision_hash);
  mix(hash, value.fill_source_mask);
  mix(hash, value.occupied ? 1U : 0U);
  return nonzero_hash(hash);
}

bool valid_order_snapshot(const OmsOrderSnapshot& value) noexcept {
  if (!value.occupied || !value.order_id.valid() || !value.session_id.valid() ||
      !value.authority.valid() || !valid_order_state(value.state) ||
      value.state_version == 0U || value.last_update_process_monotonic_time_ns == 0U ||
      value.client_order_id.back() != '\0') {
    return false;
  }
  if (value.state == OrderState::unknown_recovery && !value.intent_id.valid()) {
    return value.configuration_version.valid();
  }
  if (!value.intent_id.valid() || !value.account_id.valid() ||
      !value.strategy_id.valid() || !value.venue_id.valid() ||
      !value.instrument_id.valid() || !value.configuration_version.valid() ||
      value.price_ticks <= 0 || value.quantity_units == 0U || value.intent_hash == 0U ||
      value.risk_decision_hash == 0U ||
      value.cumulative_fill_quantity_units > value.quantity_units) {
    return false;
  }
  if (value.state == OrderState::filled) {
    return value.cumulative_fill_quantity_units == value.quantity_units &&
           value.remaining_quantity_units == 0U;
  }
  if (terminal_state(value.state)) {
    return value.remaining_quantity_units == 0U;
  }
  return value.remaining_quantity_units ==
         value.quantity_units - value.cumulative_fill_quantity_units;
}

std::uint64_t stable_snapshot_hash(const OmsSnapshot& value) noexcept {
  std::uint64_t hash = kFnvOffset;
  mix_identifier(hash, value.session_id);
  mix_identifier(hash, value.account_id);
  mix_identifier(hash, value.configuration_version);
  mix(hash, value.configuration_hash);
  mix(hash, value.exchange_session_epoch);
  mix(hash, value.active_fencing_token);
  mix(hash, value.snapshot_sequence);
  mix(hash, value.source_journal_sequence);
  mix(hash, value.source_journal_hash);
  mix(hash, value.as_of_process_monotonic_time_ns);
  mix(hash, value.order_count);
  mix(hash, value.live_order_count);
  mix(hash, value.unknown_recovery_count);
  mix(hash, value.unreconciled_fill_count);
  mix(hash, static_cast<std::uint8_t>(value.health));
  mix(hash, static_cast<std::uint8_t>(value.invariant));
  mix(hash, value.ready ? 1U : 0U);
  for (const auto& order : value.orders) {
    if (order.occupied) {
      mix(hash, stable_order_snapshot_hash(order));
    }
  }
  return nonzero_hash(hash);
}

bool valid_snapshot(const OmsSnapshot& value) noexcept {
  if (!value.session_id.valid() || !value.account_id.valid() ||
      !value.configuration_version.valid() || value.configuration_hash == 0U ||
      value.exchange_session_epoch == 0U || value.active_fencing_token == 0U ||
      value.snapshot_sequence == 0U || value.as_of_process_monotonic_time_ns == 0U ||
      value.order_count > value.orders.size() ||
      value.live_order_count > value.order_count ||
      value.unknown_recovery_count > value.order_count ||
      value.health < OmsHealth::starting || value.health > OmsHealth::stopped ||
      value.invariant > OmsInvariant::replay_diverged || value.stable_hash == 0U ||
      value.stable_hash != stable_snapshot_hash(value)) {
    return false;
  }
  std::uint32_t count{};
  std::uint32_t live{};
  std::uint32_t unknown{};
  for (const auto& order : value.orders) {
    if (!order.occupied) {
      continue;
    }
    if (!valid_order_snapshot(order)) {
      return false;
    }
    ++count;
    live += live_state(order.state) ? 1U : 0U;
    unknown += order.state == OrderState::unknown_recovery ? 1U : 0U;
  }
  const bool counts_match = count == value.order_count &&
                            live == value.live_order_count &&
                            unknown == value.unknown_recovery_count;
  const bool readiness_matches =
      !value.ready || (value.health == OmsHealth::healthy && unknown == 0U &&
                       value.invariant == OmsInvariant::none);
  return counts_match && readiness_matches;
}

std::uint64_t stable_apply_result_hash(ApplyResult value) noexcept {
  value.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix(hash, static_cast<std::uint8_t>(value.status));
  mix(hash, static_cast<std::uint8_t>(value.reason));
  mix_identifier(hash, value.order_id);
  mix(hash, static_cast<std::uint8_t>(value.previous_state));
  mix(hash, static_cast<std::uint8_t>(value.current_state));
  mix(hash, value.state_version);
  mix(hash, value.journal_sequence);
  mix(hash, value.snapshot_sequence);
  mix(hash, value.snapshot_hash);
  mix(hash, value.gateway_command.stable_hash);
  return nonzero_hash(hash);
}

} // namespace aegis::oms
