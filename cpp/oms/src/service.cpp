#include "aegis/oms/service.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace aegis::oms {
namespace {

constexpr std::size_t kNoOrder = kMaximumOmsOrders;
constexpr std::size_t kNoExecution = kMaximumOmsExecutions;
constexpr std::size_t kNoReceipt = kOmsIdempotencyCapacity;
constexpr std::size_t kMaximumWriterAttempts = 32U;

[[nodiscard]] std::size_t order_bucket(const common::OrderId value) noexcept {
  return static_cast<std::size_t>((value.high() ^ std::rotl(value.low(), 17)) &
                                  (kMaximumOmsOrders - 1U));
}

[[nodiscard]] std::size_t execution_bucket(const common::GlobalEventId value) noexcept {
  return static_cast<std::size_t>((value.high() ^ std::rotl(value.low(), 23)) &
                                  (kMaximumOmsExecutions - 1U));
}

[[nodiscard]] std::size_t receipt_bucket(const common::GlobalEventId value) noexcept {
  return static_cast<std::size_t>((value.high() ^ std::rotl(value.low(), 29)) &
                                  (kOmsIdempotencyCapacity - 1U));
}

[[nodiscard]] std::uint8_t source_bit(const EventSource source) noexcept {
  if (source == EventSource::gateway) {
    return 1U;
  }
  if (source == EventSource::drop_copy) {
    return 2U;
  }
  return 0U;
}

[[nodiscard]] bool order_scope_matches(const OmsOrderSnapshot& order,
                                       const risk::RiskIntent& intent) noexcept {
  return order.session_id == intent.session_id &&
         order.account_id == intent.account_id &&
         order.strategy_id == intent.strategy_id && order.venue_id == intent.venue_id &&
         order.instrument_id == intent.instrument_id && order.side == intent.action;
}

template <typename Execution>
[[nodiscard]] bool same_execution(const Execution& existing, const OmsInput& input,
                                  const common::OrderId order_id) noexcept {
  return existing.order_id == order_id && existing.price_ticks == input.price_ticks &&
         existing.quantity_units == input.quantity_units &&
         (!existing.external_order_id.valid() || !input.external_order_id.valid() ||
          existing.external_order_id == input.external_order_id);
}

[[nodiscard]] OrderState
state_for_open_quantity(const OmsOrderSnapshot& order) noexcept {
  return order.cumulative_fill_quantity_units == 0U ? OrderState::working
                                                    : OrderState::partially_filled;
}

[[nodiscard]] OmsReason invalid_reason(const OmsInput& input) noexcept {
  if (input.kind == InputKind::accept_intent ||
      input.kind == InputKind::cancel_intent ||
      input.kind == InputKind::replace_intent) {
    return OmsReason::exact_risk_approval_required;
  }
  return OmsReason::configuration_invalid;
}

[[nodiscard]] bool same_replay_result(const ApplyResult& actual,
                                      const ApplyResult& expected) noexcept {
  return actual.status == expected.status && actual.reason == expected.reason &&
         actual.order_id == expected.order_id &&
         actual.previous_state == expected.previous_state &&
         actual.current_state == expected.current_state &&
         actual.state_version == expected.state_version &&
         actual.journal_sequence == expected.journal_sequence &&
         actual.journal_record_hash == expected.journal_record_hash &&
         actual.snapshot_sequence == expected.snapshot_sequence &&
         actual.snapshot_hash == expected.snapshot_hash &&
         actual.gateway_command.stable_hash == expected.gateway_command.stable_hash &&
         actual.stable_hash == expected.stable_hash;
}

} // namespace

DeterministicOms::DeterministicOms(OmsConfiguration configuration,
                                   OmsJournal& journal) noexcept
    : configuration_(configuration), journal_(journal),
      active_fencing_token_(configuration.initial_fencing_token) {
  const bool store_initialized =
      snapshot_store_.initialize([](const void* value) noexcept {
        return stable_snapshot_hash(*static_cast<const OmsSnapshot*>(value));
      });
  if (!store_initialized || !valid_configuration(configuration_)) {
    health_ = OmsHealth::unsafe;
    invariant_ = OmsInvariant::configuration_invalid;
    return;
  }
  health_ = OmsHealth::healthy;
  initialized_.store(true, std::memory_order_release);
  if (!recompute_snapshot(1U, 0U, 0U) || !publish_snapshot(1U)) {
    set_unsafe(OmsInvariant::snapshot_publication_failed);
  }
}

bool DeterministicOms::initialized() const noexcept {
  return initialized_.load(std::memory_order_acquire);
}

bool DeterministicOms::ready() const noexcept {
  return initialized() && !stopped_.load(std::memory_order_acquire) &&
         health_ == OmsHealth::healthy && snapshot_.ready;
}

OmsHealth DeterministicOms::health() const noexcept { return health_; }

OmsInvariant DeterministicOms::invariant() const noexcept { return invariant_; }

const common::BuildInfo& DeterministicOms::build_info() noexcept {
  return common::current_build_info();
}

std::uint64_t DeterministicOms::configuration_hash() const noexcept {
  return configuration_.stable_hash;
}

bool DeterministicOms::try_enter() noexcept {
  for (std::size_t attempt = 0U; attempt < kMaximumWriterAttempts; ++attempt) {
    if (!writer_gate_.test_and_set(std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

void DeterministicOms::leave() noexcept {
  writer_gate_.clear(std::memory_order_release);
}

std::size_t
DeterministicOms::find_order(const common::OrderId order_id) const noexcept {
  if (!order_id.valid()) {
    return kNoOrder;
  }
  const auto start = order_bucket(order_id);
  for (std::size_t offset = 0U; offset < orders_.size(); ++offset) {
    const auto index = (start + offset) & (orders_.size() - 1U);
    if (!orders_[index].value.occupied) {
      return kNoOrder;
    }
    if (orders_[index].value.order_id == order_id) {
      return index;
    }
  }
  return kNoOrder;
}

std::size_t DeterministicOms::find_order_by_intent(
    const common::IntentId intent_id) const noexcept {
  if (!intent_id.valid()) {
    return kNoOrder;
  }
  for (std::size_t index = 0U; index < orders_.size(); ++index) {
    if (orders_[index].value.occupied && orders_[index].value.intent_id == intent_id) {
      return index;
    }
  }
  return kNoOrder;
}

std::size_t DeterministicOms::find_order_by_external(
    const ExternalOrderId external_order_id) const noexcept {
  if (!external_order_id.valid()) {
    return kNoOrder;
  }
  for (std::size_t index = 0U; index < orders_.size(); ++index) {
    if (orders_[index].value.occupied &&
        orders_[index].value.external_order_id == external_order_id) {
      return index;
    }
  }
  return kNoOrder;
}

std::size_t
DeterministicOms::find_free_order(const common::OrderId order_id) const noexcept {
  const auto start = order_bucket(order_id);
  for (std::size_t offset = 0U; offset < orders_.size(); ++offset) {
    const auto index = (start + offset) & (orders_.size() - 1U);
    if (!orders_[index].value.occupied) {
      return index;
    }
  }
  return kNoOrder;
}

std::size_t DeterministicOms::find_execution(
    const common::GlobalEventId execution_id) const noexcept {
  if (!execution_id.valid()) {
    return kNoExecution;
  }
  const auto start = execution_bucket(execution_id);
  for (std::size_t offset = 0U; offset < executions_.size(); ++offset) {
    const auto index = (start + offset) & (executions_.size() - 1U);
    if (!executions_[index].occupied) {
      return kNoExecution;
    }
    if (executions_[index].execution_id == execution_id) {
      return index;
    }
  }
  return kNoExecution;
}

std::size_t DeterministicOms::find_free_execution(
    const common::GlobalEventId execution_id) const noexcept {
  const auto start = execution_bucket(execution_id);
  for (std::size_t offset = 0U; offset < executions_.size(); ++offset) {
    const auto index = (start + offset) & (executions_.size() - 1U);
    if (!executions_[index].occupied) {
      return index;
    }
  }
  return kNoExecution;
}

std::size_t
DeterministicOms::find_receipt(const common::GlobalEventId receipt_id) const noexcept {
  if (!receipt_id.valid()) {
    return kNoReceipt;
  }
  const auto start = receipt_bucket(receipt_id);
  for (std::size_t offset = 0U; offset < receipts_.size(); ++offset) {
    const auto index = (start + offset) & (receipts_.size() - 1U);
    if (!receipts_[index].occupied) {
      return kNoReceipt;
    }
    if (receipts_[index].receipt_id == receipt_id) {
      return index;
    }
  }
  return kNoReceipt;
}

std::size_t DeterministicOms::find_free_receipt(
    const common::GlobalEventId receipt_id) const noexcept {
  const auto start = receipt_bucket(receipt_id);
  for (std::size_t offset = 0U; offset < receipts_.size(); ++offset) {
    const auto index = (start + offset) & (receipts_.size() - 1U);
    if (!receipts_[index].occupied) {
      return index;
    }
  }
  return kNoReceipt;
}

bool DeterministicOms::record_receipt(const OmsInput& input) noexcept {
  const auto index = find_free_receipt(input.receipt_id);
  if (index == kNoReceipt) {
    return false;
  }
  receipts_[index] = {.receipt_id = input.receipt_id,
                      .input_hash = input.stable_hash,
                      .occupied = true};
  return true;
}

bool DeterministicOms::authority_matches(
    const LeaderAuthority authority) const noexcept {
  return authority.exchange_session_epoch == configuration_.exchange_session_epoch &&
         authority.fencing_token == active_fencing_token_;
}

std::size_t DeterministicOms::resolve_order(const OmsInput& input) const noexcept {
  const auto by_internal = find_order(input.order_id);
  if (by_internal != kNoOrder) {
    return by_internal;
  }
  return find_order_by_external(input.external_order_id);
}

bool DeterministicOms::bind_external_id(
    const std::size_t order_index, const ExternalOrderId external_order_id) noexcept {
  if (order_index >= orders_.size() || !external_order_id.valid()) {
    return false;
  }
  const auto other = find_order_by_external(external_order_id);
  if (other != kNoOrder && other != order_index) {
    return false;
  }
  auto& current = orders_[order_index].value.external_order_id;
  if (current.valid() && current != external_order_id) {
    return false;
  }
  current = external_order_id;
  return true;
}

void DeterministicOms::touch_order(const std::size_t order_index,
                                   const OmsInput& input) noexcept {
  auto& order = orders_[order_index].value;
  if (input.authority.valid()) {
    order.authority = input.authority;
  }
  ++order.state_version;
  order.last_update_process_monotonic_time_ns = input.process_monotonic_time_ns;
  order.last_venue_sequence = std::max(order.last_venue_sequence, input.venue_sequence);
}

bool DeterministicOms::transition(const std::size_t order_index, const OmsInput& input,
                                  const OrderState next) noexcept {
  if (order_index >= orders_.size() ||
      !transition_allowed(orders_[order_index].value.state, input.kind, next)) {
    return false;
  }
  orders_[order_index].value.state = next;
  touch_order(order_index, input);
  return true;
}

GatewayCommand DeterministicOms::make_gateway_command(
    const OmsInput& input, const OrderSlot& order, const GatewayCommandKind kind,
    const std::int64_t price_ticks, const std::uint64_t quantity_units) noexcept {
  GatewayCommand command{
      .command_id = input.receipt_id,
      .kind = kind,
      .order_id = order.value.order_id,
      .external_order_id = order.value.external_order_id,
      .client_order_id = order.value.client_order_id,
      .session_id = order.value.session_id,
      .account_id = order.value.account_id,
      .strategy_id = order.value.strategy_id,
      .venue_id = order.value.venue_id,
      .instrument_id = order.value.instrument_id,
      .configuration_version = order.value.configuration_version,
      .side = order.value.side,
      .price_ticks = price_ticks,
      .quantity_units = quantity_units,
      .cumulative_fill_quantity_units = order.value.cumulative_fill_quantity_units,
      .authority = input.authority,
      .intent_hash = order.value.intent_hash,
      .risk_decision_hash = order.value.risk_decision_hash,
      .generated_process_monotonic_time_ns = input.process_monotonic_time_ns};
  command.stable_hash = stable_gateway_command_hash(command);
  return command;
}

DeterministicOms::InternalResult
DeterministicOms::apply_accept(const OmsInput& input) noexcept {
  if (input.intent.session_id != configuration_.session_id ||
      input.intent.account_id != configuration_.account_id ||
      input.intent.configuration_version != configuration_.configuration_version) {
    return {.status = ApplyStatus::invalid, .reason = OmsReason::scope_mismatch};
  }
  const auto existing_intent = find_order_by_intent(input.intent.intent_id);
  if (existing_intent != kNoOrder) {
    const auto& existing = orders_[existing_intent].value;
    return {.status = existing.intent_hash == input.intent.stable_hash
                          ? ApplyStatus::duplicate
                          : ApplyStatus::invalid,
            .reason = existing.intent_hash == input.intent.stable_hash
                          ? OmsReason::duplicate_intent
                          : OmsReason::receipt_conflict,
            .order_index = existing_intent,
            .previous_state = existing.state};
  }
  const auto order_id = deterministic_order_id(configuration_, input.intent);
  if (find_order(order_id) != kNoOrder) {
    set_unsafe(OmsInvariant::receipt_conflict);
    return {.status = ApplyStatus::invalid, .reason = OmsReason::receipt_conflict};
  }
  const auto index = find_free_order(order_id);
  if (index == kNoOrder) {
    return {.status = ApplyStatus::capacity_exhausted,
            .reason = OmsReason::order_capacity};
  }
  auto& order = orders_[index].value;
  order = {
      .order_id = order_id,
      .intent_id = input.intent.intent_id,
      .session_id = input.intent.session_id,
      .account_id = input.intent.account_id,
      .strategy_id = input.intent.strategy_id,
      .venue_id = input.intent.venue_id,
      .instrument_id = input.intent.instrument_id,
      .configuration_version = input.intent.configuration_version,
      .client_order_id = deterministic_client_order_id(order_id),
      .authority = {.exchange_session_epoch = configuration_.exchange_session_epoch,
                    .fencing_token = active_fencing_token_},
      .state = OrderState::created,
      .state_before_recovery = OrderState::created,
      .side = input.intent.action,
      .price_ticks = input.intent.limit_price_ticks,
      .quantity_units = input.intent.quantity_units,
      .remaining_quantity_units = input.intent.quantity_units,
      .state_version = 1U,
      .last_update_process_monotonic_time_ns = input.process_monotonic_time_ns,
      .intent_hash = input.intent.stable_hash,
      .risk_decision_hash = input.risk_decision.stable_hash,
      .occupied = true};
  return {.status = ApplyStatus::applied,
          .reason = OmsReason::accepted,
          .order_index = index,
          .previous_state = OrderState::created,
          .state_changed = true,
          .snapshot_changed = true};
}

DeterministicOms::InternalResult
DeterministicOms::apply_mark_ready(const OmsInput& input) noexcept {
  if (!authority_matches(input.authority)) {
    return {.status = ApplyStatus::fenced, .reason = OmsReason::stale_fencing_token};
  }
  const auto index = find_order(input.order_id);
  if (index == kNoOrder) {
    return {.status = ApplyStatus::not_found, .reason = OmsReason::order_not_found};
  }
  const auto previous = orders_[index].value.state;
  if (!transition(index, input, OrderState::ready)) {
    return {.status = ApplyStatus::forbidden_transition,
            .reason = OmsReason::forbidden_transition,
            .order_index = index,
            .previous_state = previous};
  }
  return {.status = ApplyStatus::applied,
          .reason = OmsReason::accepted,
          .order_index = index,
          .previous_state = previous,
          .state_changed = true,
          .snapshot_changed = true};
}

DeterministicOms::InternalResult
DeterministicOms::apply_dispatch(const OmsInput& input) noexcept {
  if (!authority_matches(input.authority)) {
    return {.status = ApplyStatus::fenced, .reason = OmsReason::stale_fencing_token};
  }
  if (health_ != OmsHealth::healthy) {
    return {.status = ApplyStatus::inhibited, .reason = OmsReason::recovery_required};
  }
  const auto index = find_order(input.order_id);
  if (index == kNoOrder) {
    return {.status = ApplyStatus::not_found, .reason = OmsReason::order_not_found};
  }
  const auto previous = orders_[index].value.state;
  if (!transition(index, input, OrderState::pending_ack)) {
    return {.status = ApplyStatus::forbidden_transition,
            .reason = OmsReason::forbidden_transition,
            .order_index = index,
            .previous_state = previous};
  }
  const auto command = make_gateway_command(
      input, orders_[index], GatewayCommandKind::new_order,
      orders_[index].value.price_ticks, orders_[index].value.quantity_units);
  return {.status = ApplyStatus::applied,
          .reason = OmsReason::accepted,
          .order_index = index,
          .previous_state = previous,
          .state_changed = true,
          .snapshot_changed = true,
          .gateway_command = command};
}

DeterministicOms::InternalResult
DeterministicOms::apply_cancel(const OmsInput& input) noexcept {
  if (!authority_matches(input.authority)) {
    return {.status = ApplyStatus::fenced, .reason = OmsReason::stale_fencing_token};
  }
  if (health_ != OmsHealth::healthy) {
    return {.status = ApplyStatus::inhibited, .reason = OmsReason::recovery_required};
  }
  const auto index = find_order(input.order_id);
  if (index == kNoOrder) {
    return {.status = ApplyStatus::not_found, .reason = OmsReason::order_not_found};
  }
  auto& order = orders_[index];
  const auto previous = order.value.state;
  if (input.intent.target_order_id != order.value.order_id ||
      input.intent.session_id != order.value.session_id ||
      input.intent.account_id != order.value.account_id ||
      input.intent.strategy_id != order.value.strategy_id ||
      input.intent.venue_id != order.value.venue_id ||
      input.intent.instrument_id != order.value.instrument_id) {
    return {.status = ApplyStatus::invalid,
            .reason = OmsReason::scope_mismatch,
            .order_index = index,
            .previous_state = previous};
  }
  if (!transition(index, input, OrderState::pending_cancel)) {
    return {.status = ApplyStatus::forbidden_transition,
            .reason = OmsReason::forbidden_transition,
            .order_index = index,
            .previous_state = previous};
  }
  if (previous != OrderState::pending_replace) {
    order.state_before_pending = previous;
  }
  order.value.pending_replace_price_ticks = 0;
  order.value.pending_replace_quantity_units = 0U;
  auto command =
      make_gateway_command(input, order, GatewayCommandKind::cancel,
                           order.value.price_ticks, order.value.quantity_units);
  command.intent_hash = input.intent.stable_hash;
  command.risk_decision_hash = input.risk_decision.stable_hash;
  command.stable_hash = stable_gateway_command_hash(command);
  return {.status = ApplyStatus::applied,
          .reason = OmsReason::accepted,
          .order_index = index,
          .previous_state = previous,
          .state_changed = true,
          .snapshot_changed = true,
          .gateway_command = command};
}

DeterministicOms::InternalResult
DeterministicOms::apply_replace(const OmsInput& input) noexcept {
  if (!authority_matches(input.authority)) {
    return {.status = ApplyStatus::fenced, .reason = OmsReason::stale_fencing_token};
  }
  if (health_ != OmsHealth::healthy) {
    return {.status = ApplyStatus::inhibited, .reason = OmsReason::recovery_required};
  }
  const auto index = find_order(input.order_id);
  if (index == kNoOrder) {
    return {.status = ApplyStatus::not_found, .reason = OmsReason::order_not_found};
  }
  auto& order = orders_[index];
  const auto previous = order.value.state;
  if (!order_scope_matches(order.value, input.intent)) {
    return {.status = ApplyStatus::invalid,
            .reason = OmsReason::scope_mismatch,
            .order_index = index,
            .previous_state = previous};
  }
  if (input.intent.quantity_units <= order.value.cumulative_fill_quantity_units) {
    return {.status = ApplyStatus::invalid,
            .reason = OmsReason::invalid_quantity,
            .order_index = index,
            .previous_state = previous};
  }
  if (!transition(index, input, OrderState::pending_replace)) {
    return {.status = ApplyStatus::forbidden_transition,
            .reason = OmsReason::forbidden_transition,
            .order_index = index,
            .previous_state = previous};
  }
  order.state_before_pending = previous;
  order.value.pending_replace_price_ticks = input.intent.limit_price_ticks;
  order.value.pending_replace_quantity_units = input.intent.quantity_units;
  auto command =
      make_gateway_command(input, order, GatewayCommandKind::replace,
                           input.intent.limit_price_ticks, input.intent.quantity_units);
  command.intent_hash = input.intent.stable_hash;
  command.risk_decision_hash = input.risk_decision.stable_hash;
  command.stable_hash = stable_gateway_command_hash(command);
  return {.status = ApplyStatus::applied,
          .reason = OmsReason::accepted,
          .order_index = index,
          .previous_state = previous,
          .state_changed = true,
          .snapshot_changed = true,
          .gateway_command = command};
}

DeterministicOms::InternalResult
DeterministicOms::unknown_order(const OmsInput& input) noexcept {
  common::OrderId order_id = input.order_id;
  if (!order_id.valid() && input.external_order_id.valid()) {
    order_id = {input.external_order_id.high ^ configuration_.exchange_session_epoch,
                input.external_order_id.low ^ 0x4F4D535245434F56ULL};
  }
  if (!order_id.valid()) {
    return {.status = ApplyStatus::not_found, .reason = OmsReason::order_not_found};
  }
  const auto index = find_free_order(order_id);
  if (index == kNoOrder) {
    return {.status = ApplyStatus::capacity_exhausted,
            .reason = OmsReason::order_capacity};
  }
  auto& order = orders_[index].value;
  order = {.order_id = order_id,
           .session_id = configuration_.session_id,
           .account_id = configuration_.account_id,
           .configuration_version = configuration_.configuration_version,
           .external_order_id = input.external_order_id,
           .client_order_id = deterministic_client_order_id(order_id),
           .authority = input.authority,
           .state = OrderState::unknown_recovery,
           .state_before_recovery = OrderState::unknown_recovery,
           .price_ticks = input.price_ticks,
           .quantity_units = input.quantity_units,
           .cumulative_fill_quantity_units =
               input.kind == InputKind::fill ? input.quantity_units : 0U,
           .state_version = 1U,
           .last_venue_sequence = input.venue_sequence,
           .last_update_process_monotonic_time_ns = input.process_monotonic_time_ns,
           .fill_source_mask = source_bit(input.source),
           .occupied = true};
  health_ = OmsHealth::recovering;
  return {.status = ApplyStatus::applied,
          .reason = OmsReason::recovery_required,
          .order_index = index,
          .previous_state = OrderState::unknown_recovery,
          .state_changed = true,
          .snapshot_changed = true};
}

DeterministicOms::InternalResult
DeterministicOms::apply_acknowledgement(const OmsInput& input) noexcept {
  if (!authority_matches(input.authority)) {
    return {.status = ApplyStatus::fenced, .reason = OmsReason::stale_fencing_token};
  }
  const auto index = resolve_order(input);
  if (index == kNoOrder) {
    return unknown_order(input);
  }
  auto& order = orders_[index].value;
  const auto previous = order.state;
  if (previous == OrderState::working || terminal_state(previous)) {
    return {.status = ApplyStatus::duplicate,
            .reason = OmsReason::duplicate_receipt,
            .order_index = index,
            .previous_state = previous};
  }
  const auto next =
      previous == OrderState::pending_ack ? OrderState::working : previous;
  if (!transition_allowed(previous, input.kind, next)) {
    return {.status = ApplyStatus::forbidden_transition,
            .reason = OmsReason::forbidden_transition,
            .order_index = index,
            .previous_state = previous};
  }
  if (!bind_external_id(index, input.external_order_id)) {
    set_unsafe(OmsInvariant::external_mapping_conflict);
    return {.status = ApplyStatus::invalid,
            .reason = OmsReason::external_id_conflict,
            .order_index = index,
            .previous_state = previous,
            .snapshot_changed = true};
  }
  const bool out_of_order = input.venue_sequence <= order.last_venue_sequence;
  if (!transition(index, input, next)) {
    return {.status = ApplyStatus::forbidden_transition,
            .reason = OmsReason::forbidden_transition,
            .order_index = index,
            .previous_state = previous};
  }
  return {.status =
              out_of_order ? ApplyStatus::out_of_order_applied : ApplyStatus::applied,
          .reason = OmsReason::accepted,
          .order_index = index,
          .previous_state = previous,
          .state_changed = previous != next,
          .snapshot_changed = true};
}

DeterministicOms::InternalResult
DeterministicOms::apply_rejection(const OmsInput& input) noexcept {
  if (!authority_matches(input.authority)) {
    return {.status = ApplyStatus::fenced, .reason = OmsReason::stale_fencing_token};
  }
  const auto index = resolve_order(input);
  if (index == kNoOrder) {
    return unknown_order(input);
  }
  auto& order = orders_[index].value;
  const auto previous = order.state;
  if (!transition(index, input, OrderState::rejected)) {
    order.state_before_recovery = previous;
    order.state = OrderState::unknown_recovery;
    touch_order(index, input);
    return {.status = ApplyStatus::forbidden_transition,
            .reason = OmsReason::forbidden_transition,
            .order_index = index,
            .previous_state = previous,
            .state_changed = previous != OrderState::unknown_recovery,
            .snapshot_changed = true};
  }
  order.remaining_quantity_units = 0U;
  return {.status = ApplyStatus::applied,
          .reason = OmsReason::accepted,
          .order_index = index,
          .previous_state = previous,
          .state_changed = true,
          .snapshot_changed = true};
}

DeterministicOms::InternalResult
DeterministicOms::apply_existing_fill(const OmsInput& input,
                                      const FillLocation location) noexcept {
  const auto order_index = location.order_index;
  const auto execution_index = location.execution_index;
  auto& order = orders_[order_index].value;
  const auto previous = order.state;
  auto& execution = executions_[execution_index];
  if (!same_execution(execution, input, order.order_id)) {
    order.state_before_recovery = previous;
    order.state = OrderState::unknown_recovery;
    touch_order(order_index, input);
    set_unsafe(OmsInvariant::execution_conflict);
    return {.status = ApplyStatus::invalid,
            .reason = OmsReason::execution_conflict,
            .order_index = order_index,
            .previous_state = previous,
            .state_changed = true,
            .snapshot_changed = true};
  }
  const bool mapped_external =
      input.external_order_id.valid() && !order.external_order_id.valid();
  if (input.external_order_id.valid() &&
      !bind_external_id(order_index, input.external_order_id)) {
    set_unsafe(OmsInvariant::external_mapping_conflict);
    return {.status = ApplyStatus::invalid,
            .reason = OmsReason::external_id_conflict,
            .order_index = order_index,
            .previous_state = previous,
            .snapshot_changed = true};
  }
  const auto bit = source_bit(input.source);
  if ((execution.source_mask & bit) != 0U) {
    if (mapped_external) {
      touch_order(order_index, input);
    }
    return {.status = ApplyStatus::duplicate,
            .reason = OmsReason::duplicate_receipt,
            .order_index = order_index,
            .previous_state = previous,
            .snapshot_changed = mapped_external};
  }
  execution.source_mask = static_cast<std::uint8_t>(execution.source_mask | bit);
  order.fill_source_mask = static_cast<std::uint8_t>(order.fill_source_mask | bit);
  touch_order(order_index, input);
  return {.status = ApplyStatus::reconciled,
          .reason = OmsReason::accepted,
          .order_index = order_index,
          .previous_state = previous,
          .snapshot_changed = true};
}

DeterministicOms::InternalResult
DeterministicOms::apply_new_fill(const OmsInput& input,
                                 const FillLocation location) noexcept {
  const auto order_index = location.order_index;
  const auto execution_index = location.execution_index;
  auto& order = orders_[order_index].value;
  const auto previous = order.state;
  if (previous != OrderState::unknown_recovery &&
      (input.quantity_units > order.quantity_units ||
       order.cumulative_fill_quantity_units >
           order.quantity_units - input.quantity_units)) {
    order.state_before_recovery = previous;
    order.state = OrderState::unknown_recovery;
    touch_order(order_index, input);
    set_unsafe(OmsInvariant::quantity_mismatch);
    return {.status = ApplyStatus::invalid,
            .reason = OmsReason::invalid_quantity,
            .order_index = order_index,
            .previous_state = previous,
            .state_changed = true,
            .snapshot_changed = true};
  }
  const auto bit = source_bit(input.source);
  if (previous == OrderState::unknown_recovery) {
    if (input.external_order_id.valid() &&
        !bind_external_id(order_index, input.external_order_id)) {
      set_unsafe(OmsInvariant::external_mapping_conflict);
      return {.status = ApplyStatus::invalid,
              .reason = OmsReason::external_id_conflict,
              .order_index = order_index,
              .previous_state = previous,
              .snapshot_changed = true};
    }
    executions_[execution_index] = {.execution_id = input.execution_id,
                                    .order_id = order.order_id,
                                    .external_order_id = input.external_order_id,
                                    .price_ticks = input.price_ticks,
                                    .quantity_units = input.quantity_units,
                                    .venue_sequence = input.venue_sequence,
                                    .source_mask = bit,
                                    .occupied = true};
    order.fill_source_mask = static_cast<std::uint8_t>(order.fill_source_mask | bit);
    touch_order(order_index, input);
    return {.status = ApplyStatus::applied,
            .reason = OmsReason::recovery_required,
            .order_index = order_index,
            .previous_state = previous,
            .snapshot_changed = true};
  }
  const bool out_of_order = input.venue_sequence <= order.last_venue_sequence;
  const auto cumulative = order.cumulative_fill_quantity_units + input.quantity_units;
  const auto remaining = order.quantity_units - cumulative;
  OrderState next = OrderState::partially_filled;
  if (remaining == 0U) {
    next = OrderState::filled;
  } else if (previous == OrderState::pending_cancel ||
             previous == OrderState::pending_replace ||
             previous == OrderState::canceled) {
    next = previous;
  }
  if (!transition_allowed(previous, input.kind, next)) {
    return {.status = ApplyStatus::forbidden_transition,
            .reason = OmsReason::forbidden_transition,
            .order_index = order_index,
            .previous_state = previous};
  }
  if (input.external_order_id.valid() &&
      !bind_external_id(order_index, input.external_order_id)) {
    set_unsafe(OmsInvariant::external_mapping_conflict);
    return {.status = ApplyStatus::invalid,
            .reason = OmsReason::external_id_conflict,
            .order_index = order_index,
            .previous_state = previous,
            .snapshot_changed = true};
  }
  executions_[execution_index] = {.execution_id = input.execution_id,
                                  .order_id = order.order_id,
                                  .external_order_id = input.external_order_id,
                                  .price_ticks = input.price_ticks,
                                  .quantity_units = input.quantity_units,
                                  .venue_sequence = input.venue_sequence,
                                  .source_mask = bit,
                                  .occupied = true};
  order.cumulative_fill_quantity_units = cumulative;
  order.remaining_quantity_units = terminal_state(next) ? 0U : remaining;
  order.fill_source_mask = static_cast<std::uint8_t>(order.fill_source_mask | bit);
  (void)transition(order_index, input, next);
  return {.status =
              out_of_order ? ApplyStatus::out_of_order_applied : ApplyStatus::applied,
          .reason = OmsReason::accepted,
          .order_index = order_index,
          .previous_state = previous,
          .state_changed = previous != next,
          .snapshot_changed = true};
}

DeterministicOms::InternalResult
DeterministicOms::apply_fill(const OmsInput& input) noexcept {
  if (!authority_matches(input.authority)) {
    return {.status = ApplyStatus::fenced, .reason = OmsReason::stale_fencing_token};
  }
  auto order_index = resolve_order(input);
  if (order_index == kNoOrder) {
    auto created = unknown_order(input);
    order_index = created.order_index;
    if (order_index == kNoOrder) {
      return created;
    }
  }
  const auto execution_index = find_execution(input.execution_id);
  if (execution_index != kNoExecution) {
    return apply_existing_fill(
        input, {.order_index = order_index, .execution_index = execution_index});
  }
  const auto free_execution = find_free_execution(input.execution_id);
  if (free_execution == kNoExecution) {
    return {.status = ApplyStatus::capacity_exhausted,
            .reason = OmsReason::execution_capacity,
            .order_index = order_index,
            .previous_state = orders_[order_index].value.state};
  }
  return apply_new_fill(
      input, {.order_index = order_index, .execution_index = free_execution});
}

DeterministicOms::InternalResult
DeterministicOms::apply_cancel_response(const OmsInput& input) noexcept {
  if (!authority_matches(input.authority)) {
    return {.status = ApplyStatus::fenced, .reason = OmsReason::stale_fencing_token};
  }
  const auto index = resolve_order(input);
  if (index == kNoOrder) {
    return unknown_order(input);
  }
  auto& order = orders_[index];
  const auto previous = order.value.state;
  const bool out_of_order = input.venue_sequence <= order.value.last_venue_sequence;
  if (input.kind == InputKind::cancel_acknowledgement) {
    if (previous == OrderState::canceled || previous == OrderState::filled) {
      return {.status = ApplyStatus::duplicate,
              .reason = OmsReason::duplicate_receipt,
              .order_index = index,
              .previous_state = previous};
    }
    if (!transition(index, input, OrderState::canceled)) {
      return {.status = ApplyStatus::forbidden_transition,
              .reason = OmsReason::forbidden_transition,
              .order_index = index,
              .previous_state = previous};
    }
    order.value.remaining_quantity_units = 0U;
  } else {
    const auto next = state_for_open_quantity(order.value);
    if (!transition(index, input, next)) {
      return {.status = ApplyStatus::forbidden_transition,
              .reason = OmsReason::forbidden_transition,
              .order_index = index,
              .previous_state = previous};
    }
  }
  return {.status =
              out_of_order ? ApplyStatus::out_of_order_applied : ApplyStatus::applied,
          .reason = OmsReason::accepted,
          .order_index = index,
          .previous_state = previous,
          .state_changed = true,
          .snapshot_changed = true};
}

DeterministicOms::InternalResult
DeterministicOms::apply_replace_response(const OmsInput& input) noexcept {
  if (!authority_matches(input.authority)) {
    return {.status = ApplyStatus::fenced, .reason = OmsReason::stale_fencing_token};
  }
  const auto index = resolve_order(input);
  if (index == kNoOrder) {
    return unknown_order(input);
  }
  auto& order = orders_[index].value;
  const auto previous = order.state;
  const bool out_of_order = input.venue_sequence <= order.last_venue_sequence;
  OrderState next = state_for_open_quantity(order);
  if (input.kind == InputKind::replace_acknowledgement) {
    if (order.pending_replace_price_ticks <= 0 ||
        order.pending_replace_quantity_units <= order.cumulative_fill_quantity_units) {
      set_unsafe(OmsInvariant::quantity_mismatch);
      return {.status = ApplyStatus::invalid,
              .reason = OmsReason::invalid_quantity,
              .order_index = index,
              .previous_state = previous,
              .snapshot_changed = true};
    }
    next = order.pending_replace_quantity_units == order.cumulative_fill_quantity_units
               ? OrderState::filled
               : state_for_open_quantity(order);
    if (!transition_allowed(previous, input.kind, next)) {
      return {.status = ApplyStatus::forbidden_transition,
              .reason = OmsReason::forbidden_transition,
              .order_index = index,
              .previous_state = previous};
    }
    order.price_ticks = order.pending_replace_price_ticks;
    order.quantity_units = order.pending_replace_quantity_units;
    order.remaining_quantity_units =
        order.quantity_units - order.cumulative_fill_quantity_units;
    next = state_for_open_quantity(order);
  }
  if (!transition(index, input, next)) {
    return {.status = ApplyStatus::forbidden_transition,
            .reason = OmsReason::forbidden_transition,
            .order_index = index,
            .previous_state = previous};
  }
  order.pending_replace_price_ticks = 0;
  order.pending_replace_quantity_units = 0U;
  return {.status =
              out_of_order ? ApplyStatus::out_of_order_applied : ApplyStatus::applied,
          .reason = OmsReason::accepted,
          .order_index = index,
          .previous_state = previous,
          .state_changed = true,
          .snapshot_changed = true};
}

DeterministicOms::InternalResult
DeterministicOms::apply_expire(const OmsInput& input) noexcept {
  if (!authority_matches(input.authority)) {
    return {.status = ApplyStatus::fenced, .reason = OmsReason::stale_fencing_token};
  }
  const auto index = find_order(input.order_id);
  if (index == kNoOrder) {
    return {.status = ApplyStatus::not_found, .reason = OmsReason::order_not_found};
  }
  auto& order = orders_[index].value;
  const auto previous = order.state;
  const bool source_allowed =
      (input.source == EventSource::internal &&
       (previous == OrderState::created || previous == OrderState::ready)) ||
      (input.source == EventSource::gateway && previous != OrderState::created &&
       previous != OrderState::ready);
  if (!source_allowed || !transition(index, input, OrderState::expired)) {
    return {.status = ApplyStatus::forbidden_transition,
            .reason = OmsReason::forbidden_transition,
            .order_index = index,
            .previous_state = previous};
  }
  order.remaining_quantity_units = 0U;
  return {.status = ApplyStatus::applied,
          .reason = OmsReason::accepted,
          .order_index = index,
          .previous_state = previous,
          .state_changed = true,
          .snapshot_changed = true};
}

DeterministicOms::InternalResult
DeterministicOms::apply_recovery_begin(const OmsInput& input) noexcept {
  if (!authority_matches(input.authority)) {
    return {.status = ApplyStatus::fenced, .reason = OmsReason::stale_fencing_token};
  }
  const auto index = find_order(input.order_id);
  if (index == kNoOrder) {
    return {.status = ApplyStatus::not_found, .reason = OmsReason::order_not_found};
  }
  auto& order = orders_[index].value;
  const auto previous = order.state;
  if (!transition(index, input, OrderState::unknown_recovery)) {
    return {.status = ApplyStatus::forbidden_transition,
            .reason = OmsReason::forbidden_transition,
            .order_index = index,
            .previous_state = previous};
  }
  order.state_before_recovery = previous;
  health_ = OmsHealth::recovering;
  return {.status = ApplyStatus::applied,
          .reason = OmsReason::recovery_required,
          .order_index = index,
          .previous_state = previous,
          .state_changed = previous != OrderState::unknown_recovery,
          .snapshot_changed = true};
}

DeterministicOms::InternalResult
DeterministicOms::apply_recovery_observation(const OmsInput& input) noexcept {
  if (!authority_matches(input.authority)) {
    return {.status = ApplyStatus::fenced, .reason = OmsReason::stale_fencing_token};
  }
  const auto index = resolve_order(input);
  if (index == kNoOrder) {
    return unknown_order(input);
  }
  auto& order = orders_[index].value;
  const auto previous = order.state;
  if (previous != OrderState::unknown_recovery || !order.intent_id.valid()) {
    return {.status = ApplyStatus::inhibited,
            .reason = OmsReason::recovery_required,
            .order_index = index,
            .previous_state = previous};
  }
  if (input.external_order_id.valid() &&
      !bind_external_id(index, input.external_order_id)) {
    set_unsafe(OmsInvariant::external_mapping_conflict);
    return {.status = ApplyStatus::invalid,
            .reason = OmsReason::external_id_conflict,
            .order_index = index,
            .previous_state = previous,
            .snapshot_changed = true};
  }
  if (input.recovery_observation == RecoveryObservation::absent_ambiguous) {
    touch_order(index, input);
    return {.status = ApplyStatus::inhibited,
            .reason = OmsReason::ambiguous_absence,
            .order_index = index,
            .previous_state = previous,
            .snapshot_changed = true};
  }
  OrderState next = OrderState::unknown_recovery;
  switch (input.recovery_observation) {
  case RecoveryObservation::working:
    next = state_for_open_quantity(order);
    break;
  case RecoveryObservation::partially_filled:
    if (input.quantity_units == 0U || input.quantity_units >= order.quantity_units) {
      return {.status = ApplyStatus::invalid,
              .reason = OmsReason::invalid_quantity,
              .order_index = index,
              .previous_state = previous};
    }
    order.cumulative_fill_quantity_units = input.quantity_units;
    order.remaining_quantity_units = order.quantity_units - input.quantity_units;
    next = OrderState::partially_filled;
    break;
  case RecoveryObservation::filled:
    order.cumulative_fill_quantity_units = order.quantity_units;
    order.remaining_quantity_units = 0U;
    next = OrderState::filled;
    break;
  case RecoveryObservation::canceled:
    order.remaining_quantity_units = 0U;
    next = OrderState::canceled;
    break;
  case RecoveryObservation::rejected:
    order.remaining_quantity_units = 0U;
    next = OrderState::rejected;
    break;
  case RecoveryObservation::expired:
    order.remaining_quantity_units = 0U;
    next = OrderState::expired;
    break;
  case RecoveryObservation::none:
  case RecoveryObservation::absent_ambiguous:
    break;
  }
  if (!transition(index, input, next)) {
    return {.status = ApplyStatus::forbidden_transition,
            .reason = OmsReason::forbidden_transition,
            .order_index = index,
            .previous_state = previous};
  }
  update_health_from_state();
  return {.status = ApplyStatus::reconciled,
          .reason = OmsReason::accepted,
          .order_index = index,
          .previous_state = previous,
          .state_changed = true,
          .snapshot_changed = true};
}

DeterministicOms::InternalResult
DeterministicOms::apply_authority_update(const OmsInput& input) noexcept {
  if (input.authority.exchange_session_epoch != configuration_.exchange_session_epoch) {
    set_unsafe(OmsInvariant::split_brain);
    return {.status = ApplyStatus::fenced,
            .reason = OmsReason::exchange_epoch_mismatch,
            .snapshot_changed = true};
  }
  if (input.authority.fencing_token < active_fencing_token_) {
    set_unsafe(OmsInvariant::split_brain);
    return {.status = ApplyStatus::fenced,
            .reason = OmsReason::stale_fencing_token,
            .snapshot_changed = true};
  }
  if (input.authority.fencing_token == active_fencing_token_) {
    return {.status = ApplyStatus::duplicate, .reason = OmsReason::duplicate_receipt};
  }
  active_fencing_token_ = input.authority.fencing_token;
  bool changed = false;
  for (auto& slot : orders_) {
    if (!slot.value.occupied || !live_state(slot.value.state)) {
      continue;
    }
    slot.value.state_before_recovery = slot.value.state;
    slot.value.state = OrderState::unknown_recovery;
    slot.value.authority = input.authority;
    ++slot.value.state_version;
    slot.value.last_update_process_monotonic_time_ns = input.process_monotonic_time_ns;
    changed = true;
  }
  health_ = changed ? OmsHealth::recovering : OmsHealth::healthy;
  return {.status = ApplyStatus::applied,
          .reason = changed ? OmsReason::recovery_required : OmsReason::accepted,
          .state_changed = changed,
          .snapshot_changed = true};
}

DeterministicOms::InternalResult
DeterministicOms::apply_locked(const OmsInput& input) noexcept {
  switch (input.kind) {
  case InputKind::accept_intent:
    return apply_accept(input);
  case InputKind::mark_ready:
    return apply_mark_ready(input);
  case InputKind::dispatch:
    return apply_dispatch(input);
  case InputKind::cancel_intent:
    return apply_cancel(input);
  case InputKind::replace_intent:
    return apply_replace(input);
  case InputKind::acknowledgement:
    return apply_acknowledgement(input);
  case InputKind::rejection:
    return apply_rejection(input);
  case InputKind::fill:
    return apply_fill(input);
  case InputKind::cancel_acknowledgement:
  case InputKind::cancel_rejection:
    return apply_cancel_response(input);
  case InputKind::replace_acknowledgement:
  case InputKind::replace_rejection:
    return apply_replace_response(input);
  case InputKind::expire:
    return apply_expire(input);
  case InputKind::recovery_begin:
    return apply_recovery_begin(input);
  case InputKind::recovery_observation:
    return apply_recovery_observation(input);
  case InputKind::authority_update:
    return apply_authority_update(input);
  }
  return {.status = ApplyStatus::invalid, .reason = OmsReason::configuration_invalid};
}

void DeterministicOms::update_health_from_state() noexcept {
  if (invariant_ != OmsInvariant::none) {
    health_ = OmsHealth::unsafe;
    return;
  }
  bool recovery = false;
  for (const auto& order : orders_) {
    recovery = recovery || (order.value.occupied &&
                            order.value.state == OrderState::unknown_recovery);
  }
  if (configuration_.require_drop_copy) {
    for (const auto& execution : executions_) {
      recovery = recovery || (execution.occupied && execution.source_mask != 3U);
    }
  }
  health_ = recovery ? OmsHealth::recovering : OmsHealth::healthy;
}

bool DeterministicOms::recompute_snapshot(const std::uint64_t event_time_ns,
                                          const std::uint64_t journal_sequence,
                                          const std::uint64_t journal_hash) noexcept {
  OmsSnapshot output{.session_id = configuration_.session_id,
                     .account_id = configuration_.account_id,
                     .configuration_version = configuration_.configuration_version,
                     .configuration_hash = configuration_.stable_hash,
                     .exchange_session_epoch = configuration_.exchange_session_epoch,
                     .active_fencing_token = active_fencing_token_,
                     .snapshot_sequence = snapshot_sequence_ + 1U,
                     .source_journal_sequence = journal_sequence,
                     .source_journal_hash = journal_hash,
                     .as_of_process_monotonic_time_ns = event_time_ns,
                     .health = health_,
                     .invariant = invariant_};
  for (std::size_t index = 0U; index < orders_.size(); ++index) {
    if (!orders_[index].value.occupied) {
      continue;
    }
    output.orders[index] = orders_[index].value;
    ++output.order_count;
    output.live_order_count += live_state(orders_[index].value.state) ? 1U : 0U;
    output.unknown_recovery_count +=
        orders_[index].value.state == OrderState::unknown_recovery ? 1U : 0U;
  }
  if (configuration_.require_drop_copy) {
    for (const auto& execution : executions_) {
      output.unreconciled_fill_count +=
          execution.occupied && execution.source_mask != 3U ? 1U : 0U;
    }
  }
  output.ready = health_ == OmsHealth::healthy && invariant_ == OmsInvariant::none &&
                 output.unknown_recovery_count == 0U &&
                 output.unreconciled_fill_count == 0U &&
                 !stopped_.load(std::memory_order_relaxed);
  output.stable_hash = stable_snapshot_hash(output);
  if (!valid_snapshot(output)) {
    return false;
  }
  snapshot_ = output;
  snapshot_sequence_ = output.snapshot_sequence;
  return true;
}

bool DeterministicOms::publish_snapshot(const std::uint64_t event_time_ns) noexcept {
  const auto status = snapshot_store_.publish(snapshot_, event_time_ns);
  if (status != event_bus::SnapshotPublishStatus::published) {
    snapshot_publish_failure_count_.fetch_add(1U, std::memory_order_relaxed);
    health_ = OmsHealth::recovering;
    invariant_ = OmsInvariant::snapshot_publication_failed;
    return false;
  }
  snapshot_publication_count_.fetch_add(1U, std::memory_order_relaxed);
  return true;
}

ApplyResult DeterministicOms::apply(const OmsInput& input) noexcept {
  input_count_.fetch_add(1U, std::memory_order_relaxed);
  if (stopped_.load(std::memory_order_acquire)) {
    ApplyResult result{.status = ApplyStatus::stopped, .reason = OmsReason::shutdown};
    result.stable_hash = stable_apply_result_hash(result);
    return result;
  }
  if (!initialized()) {
    ApplyResult result{.status = ApplyStatus::invalid,
                       .reason = OmsReason::configuration_invalid};
    result.stable_hash = stable_apply_result_hash(result);
    return result;
  }
  if (!try_enter()) {
    ApplyResult result{.status = ApplyStatus::busy,
                       .reason = OmsReason::recovery_required};
    result.stable_hash = stable_apply_result_hash(result);
    return result;
  }
  const auto reservation = journal_.try_reserve();
  if (!reservation.valid) {
    journal_full_count_.fetch_add(1U, std::memory_order_relaxed);
    set_unsafe(OmsInvariant::journal_exhausted);
    leave();
    ApplyResult result{.status = ApplyStatus::journal_unavailable,
                       .reason = OmsReason::journal_capacity};
    result.stable_hash = stable_apply_result_hash(result);
    return result;
  }

  InternalResult internal{};
  const auto receipt_index = find_receipt(input.receipt_id);
  if (!valid_input(input)) {
    internal = {.status = ApplyStatus::invalid, .reason = invalid_reason(input)};
  } else if (receipt_index != kNoReceipt) {
    if (receipts_[receipt_index].input_hash == input.stable_hash) {
      internal = {.status = ApplyStatus::duplicate,
                  .reason = OmsReason::duplicate_receipt,
                  .order_index = resolve_order(input)};
    } else {
      set_unsafe(OmsInvariant::receipt_conflict);
      internal = {.status = ApplyStatus::invalid,
                  .reason = OmsReason::receipt_conflict,
                  .order_index = resolve_order(input),
                  .snapshot_changed = true};
    }
  } else if (find_free_receipt(input.receipt_id) == kNoReceipt) {
    set_unsafe(OmsInvariant::receipt_conflict);
    internal = {.status = ApplyStatus::capacity_exhausted,
                .reason = OmsReason::idempotency_capacity,
                .snapshot_changed = true};
  } else {
    internal = apply_locked(input);
    if (!record_receipt(input)) {
      set_unsafe(OmsInvariant::receipt_conflict);
      internal = {.status = ApplyStatus::capacity_exhausted,
                  .reason = OmsReason::idempotency_capacity,
                  .snapshot_changed = true};
    }
  }

  update_health_from_state();
  if (internal.snapshot_changed &&
      !recompute_snapshot(input.process_monotonic_time_ns, reservation.sequence,
                          journal_.last_record_hash())) {
    set_unsafe(OmsInvariant::quantity_mismatch);
    internal.status = ApplyStatus::invalid;
    internal.reason = OmsReason::arithmetic_overflow;
  }

  ApplyResult result{.status = internal.status,
                     .reason = internal.reason,
                     .journal_sequence = reservation.sequence,
                     .snapshot_sequence = snapshot_.snapshot_sequence,
                     .snapshot_hash = snapshot_.stable_hash,
                     .gateway_command = internal.gateway_command};
  if (internal.order_index < orders_.size()) {
    const auto& order = orders_[internal.order_index].value;
    result.order_id = order.order_id;
    result.previous_state = internal.previous_state;
    result.current_state = order.state;
    result.state_version = order.state_version;
  }
  result.stable_hash = stable_apply_result_hash(result);
  journal_.commit(reservation, input, result);
  result.journal_record_hash = journal_.last_record_hash();

  if (internal.snapshot_changed) {
    (void)publish_snapshot(input.process_monotonic_time_ns);
  }
  applied_count_.fetch_add(internal.status == ApplyStatus::applied ? 1U : 0U,
                           std::memory_order_relaxed);
  duplicate_count_.fetch_add(internal.status == ApplyStatus::duplicate ? 1U : 0U,
                             std::memory_order_relaxed);
  forbidden_count_.fetch_add(internal.status == ApplyStatus::forbidden_transition ? 1U
                                                                                  : 0U,
                             std::memory_order_relaxed);
  out_of_order_count_.fetch_add(
      internal.status == ApplyStatus::out_of_order_applied ? 1U : 0U,
      std::memory_order_relaxed);
  reconciliation_count_.fetch_add(internal.status == ApplyStatus::reconciled ? 1U : 0U,
                                  std::memory_order_relaxed);
  command_count_.fetch_add(
      internal.gateway_command.kind != GatewayCommandKind::none ? 1U : 0U,
      std::memory_order_relaxed);
  fenced_count_.fetch_add(internal.status == ApplyStatus::fenced ? 1U : 0U,
                          std::memory_order_relaxed);
  leave();
  return result;
}

bool DeterministicOms::lookup_order(const common::OrderId order_id,
                                    OmsOrderSnapshot& output) const noexcept {
  const auto index = find_order(order_id);
  if (index == kNoOrder) {
    return false;
  }
  output = orders_[index].value;
  return true;
}

OmsSnapshot DeterministicOms::snapshot_for_quiescent_inspection() const noexcept {
  return snapshot_;
}

event_bus::SnapshotReadStatus
DeterministicOms::acquire_snapshot(const std::uint64_t acquired_at_ns,
                                   OmsSnapshotStore::ReadHandle& output) noexcept {
  return snapshot_store_.acquire(acquired_at_ns, output);
}

bool DeterministicOms::verify_invariants_locked() const noexcept {
  for (std::size_t index = 0U; index < orders_.size(); ++index) {
    const auto& order = orders_[index].value;
    if (!order.occupied) {
      continue;
    }
    if (!valid_order_snapshot(order)) {
      return false;
    }
    for (std::size_t other = index + 1U; other < orders_.size(); ++other) {
      const auto& candidate = orders_[other].value;
      if (!candidate.occupied || order.order_id == candidate.order_id ||
          (order.intent_id.valid() && order.intent_id == candidate.intent_id) ||
          (order.external_order_id.valid() &&
           order.external_order_id == candidate.external_order_id) ||
          order.client_order_id == candidate.client_order_id) {
        if (candidate.occupied) {
          return false;
        }
      }
    }
  }
  for (const auto& execution : executions_) {
    if (!execution.occupied) {
      continue;
    }
    if (!execution.execution_id.valid() || find_order(execution.order_id) == kNoOrder ||
        execution.price_ticks <= 0 || execution.quantity_units == 0U ||
        execution.source_mask == 0U || execution.source_mask > 3U) {
      return false;
    }
  }
  return valid_snapshot(snapshot_) && snapshot_.health == health_ &&
         snapshot_.invariant == invariant_;
}

bool DeterministicOms::verify_invariants() noexcept {
  if (!try_enter()) {
    return false;
  }
  const bool valid = verify_invariants_locked();
  if (!valid) {
    set_unsafe(OmsInvariant::quantity_mismatch);
  }
  leave();
  return valid;
}

common::GlobalEventId
DeterministicOms::recovery_receipt(const common::OrderId order_id,
                                   const std::uint64_t journal_sequence) noexcept {
  const auto high = order_id.high() ^ 0x5245434F56455259ULL;
  const auto low = order_id.low() ^ journal_sequence ^ 0x4F4D535245535441ULL;
  return {high == 0U ? 1U : high, low == 0U ? 1U : low};
}

RecoveryStatus
DeterministicOms::recover_from(const OmsJournal& source,
                               const std::uint64_t recovery_time_ns) noexcept {
  if (&source == &journal_ || journal_.size() != 0U || snapshot_.order_count != 0U) {
    return RecoveryStatus::target_not_empty;
  }
  OmsJournalRecord record{};
  for (std::uint64_t sequence = 1U; sequence <= source.size(); ++sequence) {
    if (!source.read(sequence, record)) {
      set_unsafe(OmsInvariant::replay_diverged);
      return RecoveryStatus::source_corrupt;
    }
    const auto actual = apply(record.input);
    if (!same_replay_result(actual, record.result)) {
      set_unsafe(OmsInvariant::replay_diverged);
      return actual.status == ApplyStatus::journal_unavailable
                 ? RecoveryStatus::target_journal_full
                 : RecoveryStatus::replay_diverged;
    }
  }
  std::uint64_t recovery_ordinal{};
  for (const auto& slot : orders_) {
    if (!slot.value.occupied || !live_state(slot.value.state)) {
      continue;
    }
    ++recovery_ordinal;
    OmsInput input{
        .receipt_id =
            recovery_receipt(slot.value.order_id, source.size() + recovery_ordinal),
        .kind = InputKind::recovery_begin,
        .source = EventSource::internal,
        .order_id = slot.value.order_id,
        .authority = {.exchange_session_epoch = configuration_.exchange_session_epoch,
                      .fencing_token = active_fencing_token_},
        .process_monotonic_time_ns = recovery_time_ns + recovery_ordinal};
    input.stable_hash = stable_input_hash(input);
    const auto result = apply(input);
    if (result.status != ApplyStatus::applied) {
      set_unsafe(OmsInvariant::replay_diverged);
      return result.status == ApplyStatus::journal_unavailable
                 ? RecoveryStatus::target_journal_full
                 : RecoveryStatus::replay_diverged;
    }
  }
  update_health_from_state();
  return ready() ? RecoveryStatus::recovered_ready
                 : RecoveryStatus::recovered_inhibited;
}

OmsMetrics DeterministicOms::metrics() const noexcept {
  return {.inputs = input_count_.load(std::memory_order_relaxed),
          .applied = applied_count_.load(std::memory_order_relaxed),
          .duplicates = duplicate_count_.load(std::memory_order_relaxed),
          .forbidden_transitions = forbidden_count_.load(std::memory_order_relaxed),
          .out_of_order_events = out_of_order_count_.load(std::memory_order_relaxed),
          .reconciliations = reconciliation_count_.load(std::memory_order_relaxed),
          .emitted_commands = command_count_.load(std::memory_order_relaxed),
          .fenced_commands = fenced_count_.load(std::memory_order_relaxed),
          .journal_full_results = journal_full_count_.load(std::memory_order_relaxed),
          .snapshot_publications =
              snapshot_publication_count_.load(std::memory_order_relaxed),
          .snapshot_publish_failures =
              snapshot_publish_failure_count_.load(std::memory_order_relaxed)};
}

void DeterministicOms::set_unsafe(const OmsInvariant invariant) noexcept {
  invariant_ = invariant;
  health_ = OmsHealth::unsafe;
}

void DeterministicOms::shutdown(
    const std::uint64_t process_monotonic_time_ns) noexcept {
  if (!try_enter()) {
    return;
  }
  stopped_.store(true, std::memory_order_release);
  health_ = OmsHealth::stopped;
  (void)recompute_snapshot(process_monotonic_time_ns, journal_.size(),
                           journal_.last_record_hash());
  (void)publish_snapshot(process_monotonic_time_ns);
  leave();
}

} // namespace aegis::oms
