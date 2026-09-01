#include "aegis/risk/portfolio_service.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace aegis::risk::portfolio {
namespace {

constexpr std::size_t kNotFound = std::numeric_limits<std::size_t>::max();
constexpr std::uint8_t kPrimarySourceMask = 1U;
constexpr std::uint8_t kDropCopySourceMask = 2U;

template <typename Tag>
[[nodiscard]] std::size_t id_hash(const common::Identifier128<Tag> value,
                                  const std::size_t mask) noexcept {
  const auto rotated = std::rotl(value.high(), 23);
  return static_cast<std::size_t>((rotated ^ value.low()) & mask);
}

[[nodiscard]] bool checked_add(const std::int64_t left, const std::int64_t right,
                               std::int64_t& output) noexcept {
  return !__builtin_add_overflow(left, right, &output);
}

[[nodiscard]] bool checked_subtract(const std::int64_t left, const std::int64_t right,
                                    std::int64_t& output) noexcept {
  return !__builtin_sub_overflow(left, right, &output);
}

[[nodiscard]] bool checked_add_unsigned(const std::uint64_t left,
                                        const std::uint64_t right,
                                        std::uint64_t& output) noexcept {
  return !__builtin_add_overflow(left, right, &output);
}

[[nodiscard]] bool checked_multiply(const std::uint64_t left, const std::uint64_t right,
                                    std::uint64_t& output) noexcept {
  return !__builtin_mul_overflow(left, right, &output);
}

[[nodiscard]] bool checked_abs(const std::int64_t value,
                               std::uint64_t& output) noexcept {
  if (value == std::numeric_limits<std::int64_t>::min()) {
    return false;
  }
  output = static_cast<std::uint64_t>(value < 0 ? -value : value);
  return true;
}

[[nodiscard]] bool checked_notional(const std::int64_t price_ticks,
                                    const std::uint64_t tick_value,
                                    const std::uint64_t quantity,
                                    std::int64_t& output) noexcept {
  if (price_ticks <= 0) {
    return false;
  }
  std::uint64_t unit{};
  std::uint64_t total{};
  if (!checked_multiply(static_cast<std::uint64_t>(price_ticks), tick_value, unit) ||
      !checked_multiply(unit, quantity, total) ||
      total > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  output = static_cast<std::int64_t>(total);
  return true;
}

[[nodiscard]] bool add_unsigned(std::uint64_t& target,
                                const std::uint64_t value) noexcept {
  std::uint64_t next{};
  if (!checked_add_unsigned(target, value, next)) {
    return false;
  }
  target = next;
  return true;
}

[[nodiscard]] bool add_signed(std::int64_t& target, const std::int64_t value) noexcept {
  std::int64_t next{};
  if (!checked_add(target, value, next)) {
    return false;
  }
  target = next;
  return true;
}

[[nodiscard]] bool scaled_signed(const std::int64_t value, const std::int32_t ppm,
                                 std::int64_t& output) noexcept {
  std::int64_t product{};
  if (__builtin_mul_overflow(value, static_cast<std::int64_t>(ppm), &product)) {
    return false;
  }
  output = product / static_cast<std::int64_t>(kPortfolioPpm);
  return true;
}

[[nodiscard]] bool scaled_unsigned(const std::uint64_t value, const std::uint32_t ppm,
                                   std::uint64_t& output) noexcept {
  std::uint64_t product{};
  if (!checked_multiply(value, ppm, product)) {
    return false;
  }
  output = product / kPortfolioPpm;
  return true;
}

[[nodiscard]] bool active_order_state(const OrderLifecycleState state) noexcept {
  return state == OrderLifecycleState::accepted || state == OrderLifecycleState::open ||
         state == OrderLifecycleState::partially_filled ||
         state == OrderLifecycleState::cancel_pending;
}

[[nodiscard]] bool valid_order_transition(const OrderLifecycleState previous,
                                          const OrderLifecycleState next) noexcept {
  if (previous == next) {
    return true;
  }
  switch (previous) {
  case OrderLifecycleState::accepted:
    return next == OrderLifecycleState::open ||
           next == OrderLifecycleState::partially_filled ||
           next == OrderLifecycleState::cancel_pending ||
           next == OrderLifecycleState::cancelled ||
           next == OrderLifecycleState::filled || next == OrderLifecycleState::rejected;
  case OrderLifecycleState::open:
    return next == OrderLifecycleState::partially_filled ||
           next == OrderLifecycleState::cancel_pending ||
           next == OrderLifecycleState::cancelled ||
           next == OrderLifecycleState::filled;
  case OrderLifecycleState::partially_filled:
    return next == OrderLifecycleState::cancel_pending ||
           next == OrderLifecycleState::cancelled ||
           next == OrderLifecycleState::filled;
  case OrderLifecycleState::cancel_pending:
    return next == OrderLifecycleState::cancelled ||
           next == OrderLifecycleState::partially_filled ||
           next == OrderLifecycleState::filled;
  case OrderLifecycleState::cancelled:
  case OrderLifecycleState::filled:
  case OrderLifecycleState::rejected:
    return false;
  }
  return false;
}

[[nodiscard]] std::uint8_t source_mask(const FillSource source) noexcept {
  return source == FillSource::primary ? kPrimarySourceMask : kDropCopySourceMask;
}

} // namespace

PortfolioRiskService::PortfolioRiskService(PortfolioConfiguration configuration,
                                           PortfolioJournal& journal) noexcept
    : configuration_(configuration), journal_(journal) {
  if (!valid_configuration(configuration_) ||
      !snapshot_store_.initialize(portfolio_snapshot_checksum)) {
    invariant_ = InvariantCode::invalid_configuration;
    health_ = PortfolioHealth::unsafe;
    return;
  }
  snapshot_sequence_ = 1U;
  snapshot_.schema_major = kPortfolioSchemaMajor;
  snapshot_.schema_minor = kPortfolioSchemaMinor;
  snapshot_.risk_snapshot_id =
      common::RiskSnapshotId{configuration_.stable_hash, snapshot_sequence_};
  snapshot_.session_id = configuration_.session_id;
  snapshot_.configuration_version = configuration_.configuration_version;
  snapshot_.sequence = snapshot_sequence_;
  snapshot_.health = PortfolioHealth::starting;
  snapshot_.instrument_count = configuration_.instrument_count;
  snapshot_.strategy_count = configuration_.strategy_count;
  snapshot_.account_count = configuration_.account_count;
  snapshot_.sector_count = configuration_.sector_count;
  for (std::size_t index = 0U; index < configuration_.instrument_count; ++index) {
    snapshot_.instruments[index].instrument_id =
        configuration_.instruments[index].instrument_id;
  }
  for (std::size_t index = 0U; index < configuration_.strategy_count; ++index) {
    snapshot_.strategies[index].strategy_id = configuration_.strategies[index];
  }
  for (std::size_t index = 0U; index < configuration_.account_count; ++index) {
    snapshot_.accounts[index].account_id = configuration_.accounts[index];
  }
  for (std::size_t index = 0U; index < configuration_.sector_count; ++index) {
    snapshot_.sectors[index].sector_index = static_cast<std::uint32_t>(index);
  }
  snapshot_.stable_hash = stable_snapshot_hash(snapshot_);
  const auto published = snapshot_store_.publish(snapshot_, 0U);
  if (published != event_bus::SnapshotPublishStatus::published) {
    invariant_ = InvariantCode::invalid_configuration;
    health_ = PortfolioHealth::unsafe;
    return;
  }
  snapshot_publication_count_.store(1U, std::memory_order_relaxed);
  initialized_.store(true, std::memory_order_release);
}

bool PortfolioRiskService::initialized() const noexcept {
  return initialized_.load(std::memory_order_acquire);
}

bool PortfolioRiskService::ready() const noexcept {
  return initialized() && !stopped_.load(std::memory_order_acquire) &&
         health_ == PortfolioHealth::healthy && snapshot_.ready;
}

PortfolioHealth PortfolioRiskService::health() const noexcept { return health_; }

const common::BuildInfo& PortfolioRiskService::build_info() noexcept {
  return common::current_build_info();
}

std::uint64_t PortfolioRiskService::configuration_hash() const noexcept {
  return configuration_.stable_hash;
}

bool PortfolioRiskService::try_enter() noexcept {
  return !writer_gate_.test_and_set(std::memory_order_acquire);
}

void PortfolioRiskService::leave() noexcept {
  writer_gate_.clear(std::memory_order_release);
}

std::size_t
PortfolioRiskService::find_account(const common::AccountId value) const noexcept {
  for (std::size_t index = 0U; index < configuration_.account_count; ++index) {
    if (configuration_.accounts[index] == value) {
      return index;
    }
  }
  return kNotFound;
}

std::size_t
PortfolioRiskService::find_strategy(const common::StrategyId value) const noexcept {
  for (std::size_t index = 0U; index < configuration_.strategy_count; ++index) {
    if (configuration_.strategies[index] == value) {
      return index;
    }
  }
  return kNotFound;
}

std::size_t
PortfolioRiskService::find_instrument(const common::InstrumentId value) const noexcept {
  for (std::size_t index = 0U; index < configuration_.instrument_count; ++index) {
    if (configuration_.instruments[index].instrument_id == value) {
      return index;
    }
  }
  return kNotFound;
}

std::size_t
PortfolioRiskService::find_order(const common::OrderId value) const noexcept {
  constexpr auto mask = kMaximumPortfolioOrders - 1U;
  auto index = id_hash(value, mask);
  for (std::size_t probe = 0U; probe < orders_.size(); ++probe) {
    const auto& slot = orders_[index];
    if (!slot.occupied) {
      return kNotFound;
    }
    if (slot.order_id == value) {
      return index;
    }
    index = (index + 1U) & mask;
  }
  return kNotFound;
}

std::size_t
PortfolioRiskService::find_fill(const common::GlobalEventId value) const noexcept {
  constexpr auto mask = kMaximumPortfolioFills - 1U;
  auto index = id_hash(value, mask);
  for (std::size_t probe = 0U; probe < fills_.size(); ++probe) {
    const auto& slot = fills_[index];
    if (!slot.occupied) {
      return kNotFound;
    }
    if (slot.logical_fill_id == value) {
      return index;
    }
    index = (index + 1U) & mask;
  }
  return kNotFound;
}

std::size_t PortfolioRiskService::find_position(
    const common::AccountId account, const common::StrategyId strategy,
    const common::InstrumentId instrument) const noexcept {
  for (std::size_t index = 0U; index < positions_.size(); ++index) {
    const auto& slot = positions_[index];
    if (slot.occupied && slot.account_id == account && slot.strategy_id == strategy &&
        slot.instrument_id == instrument) {
      return index;
    }
  }
  return kNotFound;
}

std::size_t PortfolioRiskService::find_or_create_position(
    const common::AccountId account, const common::StrategyId strategy,
    const common::InstrumentId instrument) noexcept {
  const auto existing = find_position(account, strategy, instrument);
  if (existing != kNotFound) {
    return existing;
  }
  const auto instrument_index = find_instrument(instrument);
  if (instrument_index == kNotFound) {
    return kNotFound;
  }
  for (std::size_t index = 0U; index < positions_.size(); ++index) {
    auto& slot = positions_[index];
    if (!slot.occupied) {
      slot = {};
      slot.account_id = account;
      slot.strategy_id = strategy;
      slot.instrument_id = instrument;
      slot.action_generation = corporate_action_generations_[instrument_index];
      slot.occupied = true;
      return index;
    }
  }
  return kNotFound;
}

bool PortfolioRiskService::seen_event(
    const common::GlobalEventId value) const noexcept {
  constexpr auto mask = kPortfolioIdempotencyCapacity - 1U;
  auto index = id_hash(value, mask);
  for (std::size_t probe = 0U; probe < idempotency_.size(); ++probe) {
    const auto& slot = idempotency_[index];
    if (!slot.occupied) {
      return false;
    }
    if (slot.event_id == value) {
      return true;
    }
    index = (index + 1U) & mask;
  }
  return false;
}

bool PortfolioRiskService::record_event(const common::GlobalEventId value) noexcept {
  constexpr auto mask = kPortfolioIdempotencyCapacity - 1U;
  auto index = id_hash(value, mask);
  for (std::size_t probe = 0U; probe < idempotency_.size(); ++probe) {
    auto& slot = idempotency_[index];
    if (!slot.occupied) {
      slot.event_id = value;
      slot.occupied = true;
      return true;
    }
    if (slot.event_id == value) {
      return true;
    }
    index = (index + 1U) & mask;
  }
  return false;
}

ApplyResult PortfolioRiskService::apply(const PortfolioEvent& event) noexcept {
  if (!initialized()) {
    return {.status = ApplyStatus::invalid,
            .invariant = InvariantCode::invalid_configuration};
  }
  if (stopped_.load(std::memory_order_acquire)) {
    return {.status = ApplyStatus::stopped, .invariant = invariant_};
  }
  if (!try_enter()) {
    return {.status = ApplyStatus::busy, .invariant = invariant_};
  }
  event_count_.fetch_add(1U, std::memory_order_relaxed);
  const auto reservation = journal_.try_reserve();
  if (!reservation.valid) {
    journal_full_count_.fetch_add(1U, std::memory_order_relaxed);
    set_unsafe(InvariantCode::journal_exhausted);
    leave();
    return {.status = ApplyStatus::journal_unavailable,
            .invariant = InvariantCode::journal_exhausted};
  }

  InternalApplyResult internal;
  const bool invalid_envelope =
      !valid_event(event) ||
      event.configuration_version != configuration_.configuration_version ||
      event.session_id != configuration_.session_id;
  if (invalid_envelope) {
    internal = {.status = ApplyStatus::invalid,
                .invariant = InvariantCode::stale_event};
  } else if (seen_event(event.event_id)) {
    internal = {.status = ApplyStatus::duplicate, .invariant = InvariantCode::none};
  } else {
    const bool regressing_time =
        snapshot_.as_of_process_monotonic_time_ns != 0U &&
        event.process_monotonic_time_ns < snapshot_.as_of_process_monotonic_time_ns;
    if (regressing_time) {
      internal = {.status = ApplyStatus::invalid,
                  .invariant = InvariantCode::stale_event};
    } else if (!record_event(event.event_id)) {
      internal = {.status = ApplyStatus::capacity_exhausted,
                  .invariant = InvariantCode::accounting_mismatch};
      set_unsafe(internal.invariant);
      internal.state_changed = true;
    } else {
      internal = apply_locked(event);
    }
  }

  PortfolioSnapshot next = snapshot_;
  if (internal.state_changed) {
    ++snapshot_sequence_;
    const SnapshotBuildContext context{.event_time_ns = event.process_monotonic_time_ns,
                                       .journal_sequence = reservation.sequence};
    if (!recompute_snapshot(next, context)) {
      internal.status = ApplyStatus::arithmetic_overflow;
      internal.invariant = InvariantCode::arithmetic_overflow;
      set_unsafe(internal.invariant);
      static_cast<void>(recompute_snapshot(next, context));
    }
  }
  ApplyResult result{.status = internal.status,
                     .invariant = internal.invariant,
                     .journal_sequence = reservation.sequence,
                     .snapshot_sequence = next.sequence,
                     .snapshot_hash = next.stable_hash};
  journal_.commit(reservation, event, result);

  if (internal.state_changed) {
    snapshot_ = next;
    health_ = snapshot_.health;
    invariant_ = snapshot_.invariant;
    for (std::size_t index = 0U; index < snapshot_.strategy_count; ++index) {
      strategy_peaks_[index] = snapshot_.strategies[index].peak_pnl_currency_nanos;
    }
    const auto publish_status =
        snapshot_store_.publish(snapshot_, event.process_monotonic_time_ns);
    if (publish_status == event_bus::SnapshotPublishStatus::published) {
      snapshot_publication_count_.fetch_add(1U, std::memory_order_relaxed);
    } else {
      snapshot_publish_failure_count_.fetch_add(1U, std::memory_order_relaxed);
      health_ = PortfolioHealth::reconciling;
    }
  }

  switch (internal.status) {
  case ApplyStatus::applied:
    applied_count_.fetch_add(1U, std::memory_order_relaxed);
    break;
  case ApplyStatus::duplicate:
    duplicate_count_.fetch_add(1U, std::memory_order_relaxed);
    break;
  case ApplyStatus::reconciled:
    reconciled_count_.fetch_add(1U, std::memory_order_relaxed);
    break;
  default:
    rejected_count_.fetch_add(1U, std::memory_order_relaxed);
    break;
  }
  leave();
  return result;
}

PortfolioRiskService::InternalApplyResult
PortfolioRiskService::apply_locked(const PortfolioEvent& event) noexcept {
  switch (event.kind) {
  case PortfolioEventKind::order:
    return apply_order(event);
  case PortfolioEventKind::fill:
    return apply_fill(event);
  case PortfolioEventKind::fill_correction:
    return apply_fill_correction(event);
  case PortfolioEventKind::fill_bust:
    return apply_fill_bust(event);
  case PortfolioEventKind::mark:
    return apply_mark(event);
  case PortfolioEventKind::session_rollover:
    return apply_rollover(event);
  case PortfolioEventKind::corporate_action:
    return apply_corporate_action(event);
  }
  return {.status = ApplyStatus::invalid, .invariant = InvariantCode::stale_event};
}

PortfolioRiskService::InternalApplyResult
PortfolioRiskService::apply_order(const PortfolioEvent& event) noexcept {
  if (find_account(event.account_id) == kNotFound ||
      find_strategy(event.strategy_id) == kNotFound ||
      find_instrument(event.instrument_id) == kNotFound) {
    return {.status = ApplyStatus::invalid,
            .invariant = InvariantCode::accounting_mismatch};
  }
  constexpr auto mask = kMaximumPortfolioOrders - 1U;
  auto index = id_hash(event.order_id, mask);
  for (std::size_t probe = 0U; probe < orders_.size(); ++probe) {
    auto& slot = orders_[index];
    if (!slot.occupied) {
      slot.order_id = event.order_id;
      slot.account_id = event.account_id;
      slot.strategy_id = event.strategy_id;
      slot.venue_id = event.venue_id;
      slot.instrument_id = event.instrument_id;
      slot.side = event.side;
      slot.state = event.order_state;
      slot.price_ticks = event.price_ticks;
      slot.quantity_units = event.quantity_units;
      slot.cumulative_fill_quantity_units = event.cumulative_fill_quantity_units;
      slot.remaining_quantity_units = event.remaining_quantity_units;
      slot.last_update_time_ns = event.process_monotonic_time_ns;
      slot.occupied = true;
      return {.status = ApplyStatus::applied,
              .invariant = InvariantCode::none,
              .state_changed = true};
    }
    if (slot.order_id == event.order_id) {
      const bool identity_matches =
          slot.account_id == event.account_id &&
          slot.strategy_id == event.strategy_id && slot.venue_id == event.venue_id &&
          slot.instrument_id == event.instrument_id && slot.side == event.side;
      if (!identity_matches || !valid_order_transition(slot.state, event.order_state) ||
          event.cumulative_fill_quantity_units < slot.cumulative_fill_quantity_units ||
          event.process_monotonic_time_ns < slot.last_update_time_ns) {
        set_unsafe(InvariantCode::accounting_mismatch);
        return {.status = ApplyStatus::conflict,
                .invariant = InvariantCode::accounting_mismatch,
                .state_changed = true};
      }
      slot.state = event.order_state;
      slot.price_ticks = event.price_ticks;
      slot.quantity_units = event.quantity_units;
      slot.cumulative_fill_quantity_units = event.cumulative_fill_quantity_units;
      slot.remaining_quantity_units = event.remaining_quantity_units;
      slot.last_update_time_ns = event.process_monotonic_time_ns;
      return {.status = ApplyStatus::applied,
              .invariant = InvariantCode::none,
              .state_changed = true};
    }
    index = (index + 1U) & mask;
  }
  return {.status = ApplyStatus::capacity_exhausted,
          .invariant = InvariantCode::accounting_mismatch};
}

bool PortfolioRiskService::apply_fill_accounting(PositionSlot& position,
                                                 const FillSlot& fill) noexcept {
  const auto instrument_index = find_instrument(fill.instrument_id);
  if (instrument_index == kNotFound) {
    return false;
  }
  std::int64_t notional{};
  if (!checked_notional(
          fill.price_ticks,
          configuration_.instruments[instrument_index].tick_value_currency_nanos,
          fill.quantity_units, notional)) {
    return false;
  }
  const auto signed_fill_quantity =
      fill.side == Side::buy ? static_cast<std::int64_t>(fill.quantity_units)
                             : -static_cast<std::int64_t>(fill.quantity_units);
  const bool same_direction =
      position.quantity_units == 0 ||
      (position.quantity_units > 0 && signed_fill_quantity > 0) ||
      (position.quantity_units < 0 && signed_fill_quantity < 0);
  const FillAccountingValues values{
      .notional = notional,
      .signed_quantity = signed_fill_quantity,
      .tick_value_currency_nanos =
          configuration_.instruments[instrument_index].tick_value_currency_nanos};
  if (same_direction) {
    return apply_same_direction_fill(position, fill, values);
  }
  return apply_opposing_fill(position, fill, values);
}

bool PortfolioRiskService::apply_same_direction_fill(
    PositionSlot& position, const FillSlot& fill,
    const FillAccountingValues values) noexcept {
  std::int64_t next_quantity{};
  const auto signed_notional =
      fill.side == Side::buy ? values.notional : -values.notional;
  std::int64_t next_cost{};
  std::int64_t fee_adjusted_realized{};
  if (!checked_add(position.quantity_units, values.signed_quantity, next_quantity) ||
      !checked_add(position.open_cost_currency_nanos, signed_notional, next_cost) ||
      !checked_subtract(position.realized_pnl_currency_nanos, fill.fee_currency_nanos,
                        fee_adjusted_realized)) {
    return false;
  }
  position.quantity_units = next_quantity;
  position.open_cost_currency_nanos = next_cost;
  position.realized_pnl_currency_nanos = fee_adjusted_realized;
  return true;
}

bool PortfolioRiskService::apply_opposing_fill(
    PositionSlot& position, const FillSlot& fill,
    const FillAccountingValues values) noexcept {
  std::uint64_t existing_absolute{};
  if (!checked_abs(position.quantity_units, existing_absolute)) {
    return false;
  }
  const auto closing = std::min(existing_absolute, fill.quantity_units);
  std::uint64_t absolute_cost{};
  if (!checked_abs(position.open_cost_currency_nanos, absolute_cost)) {
    return false;
  }
  std::uint64_t allocated_product{};
  if (!checked_multiply(absolute_cost, closing, allocated_product)) {
    return false;
  }
  const auto allocated_cost = allocated_product / existing_absolute;
  std::int64_t close_notional{};
  if (!checked_notional(fill.price_ticks, values.tick_value_currency_nanos, closing,
                        close_notional)) {
    return false;
  }
  const auto price_difference =
      position.quantity_units > 0
          ? close_notional - static_cast<std::int64_t>(allocated_cost)
          : static_cast<std::int64_t>(allocated_cost) - close_notional;
  std::int64_t next_realized{};
  std::int64_t fee_adjusted{};
  if (!checked_add(position.realized_pnl_currency_nanos, price_difference,
                   next_realized) ||
      !checked_subtract(next_realized, fill.fee_currency_nanos, fee_adjusted)) {
    return false;
  }
  const auto existing_cost_sign = position.open_cost_currency_nanos > 0 ? 1 : -1;
  const auto allocated_signed =
      static_cast<std::int64_t>(allocated_cost) * existing_cost_sign;
  std::int64_t residual_cost{};
  if (!checked_subtract(position.open_cost_currency_nanos, allocated_signed,
                        residual_cost)) {
    return false;
  }
  const auto remainder = fill.quantity_units - closing;
  std::int64_t next_quantity{};
  if (!checked_add(position.quantity_units, values.signed_quantity, next_quantity)) {
    return false;
  }
  if (remainder > 0U) {
    std::int64_t opening_notional{};
    if (!checked_notional(fill.price_ticks, values.tick_value_currency_nanos, remainder,
                          opening_notional)) {
      return false;
    }
    residual_cost = fill.side == Side::buy ? opening_notional : -opening_notional;
  } else if (next_quantity == 0) {
    residual_cost = 0;
  }
  position.quantity_units = next_quantity;
  position.open_cost_currency_nanos = residual_cost;
  position.realized_pnl_currency_nanos = fee_adjusted;
  return true;
}

PortfolioRiskService::InternalApplyResult
PortfolioRiskService::apply_fill(const PortfolioEvent& event) noexcept {
  if (find_account(event.account_id) == kNotFound ||
      find_strategy(event.strategy_id) == kNotFound ||
      find_instrument(event.instrument_id) == kNotFound) {
    return {.status = ApplyStatus::invalid,
            .invariant = InvariantCode::accounting_mismatch};
  }
  const auto existing_index = find_fill(event.logical_fill_id);
  if (existing_index != kNotFound) {
    auto& existing = fills_[existing_index];
    const bool economics_match =
        existing.order_id == event.order_id &&
        existing.account_id == event.account_id &&
        existing.strategy_id == event.strategy_id &&
        existing.venue_id == event.venue_id &&
        existing.instrument_id == event.instrument_id && existing.side == event.side &&
        existing.price_ticks == event.price_ticks &&
        existing.quantity_units == event.quantity_units &&
        existing.fee_currency_nanos == event.fee_currency_nanos;
    if (!economics_match) {
      set_unsafe(InvariantCode::conflicting_fill);
      return {.status = ApplyStatus::conflict,
              .invariant = InvariantCode::conflicting_fill,
              .state_changed = true};
    }
    const auto mask = source_mask(event.fill_source);
    if ((existing.source_mask & mask) != 0U) {
      return {.status = ApplyStatus::duplicate, .invariant = InvariantCode::none};
    }
    existing.source_mask = static_cast<std::uint8_t>(existing.source_mask | mask);
    return {.status = ApplyStatus::reconciled,
            .invariant = InvariantCode::none,
            .state_changed = true};
  }

  const auto order_index = find_order(event.order_id);
  if (event.fill_source == FillSource::primary && order_index == kNotFound) {
    health_ = PortfolioHealth::reconciling;
    invariant_ = InvariantCode::orphan_fill;
    ++invariant_failure_count_;
    return {.status = ApplyStatus::requires_reconciliation,
            .invariant = InvariantCode::orphan_fill,
            .state_changed = true};
  }
  if (order_index != kNotFound) {
    const auto& order = orders_[order_index];
    if (order.account_id != event.account_id ||
        order.strategy_id != event.strategy_id || order.venue_id != event.venue_id ||
        order.instrument_id != event.instrument_id || order.side != event.side ||
        event.quantity_units > order.remaining_quantity_units) {
      set_unsafe(InvariantCode::conflicting_fill);
      return {.status = ApplyStatus::conflict,
              .invariant = InvariantCode::conflicting_fill,
              .state_changed = true};
    }
  }

  constexpr auto fill_mask = kMaximumPortfolioFills - 1U;
  auto fill_index = id_hash(event.logical_fill_id, fill_mask);
  for (std::size_t probe = 0U; probe < fills_.size(); ++probe) {
    if (!fills_[fill_index].occupied) {
      break;
    }
    fill_index = (fill_index + 1U) & fill_mask;
  }
  if (fills_[fill_index].occupied) {
    set_unsafe(InvariantCode::accounting_mismatch);
    return {.status = ApplyStatus::capacity_exhausted,
            .invariant = InvariantCode::accounting_mismatch,
            .state_changed = true};
  }
  const auto position_index =
      find_or_create_position(event.account_id, event.strategy_id, event.instrument_id);
  if (position_index == kNotFound) {
    set_unsafe(InvariantCode::accounting_mismatch);
    return {.status = ApplyStatus::capacity_exhausted,
            .invariant = InvariantCode::accounting_mismatch,
            .state_changed = true};
  }
  FillSlot fill;
  fill.logical_fill_id = event.logical_fill_id;
  fill.order_id = event.order_id;
  fill.account_id = event.account_id;
  fill.strategy_id = event.strategy_id;
  fill.venue_id = event.venue_id;
  fill.instrument_id = event.instrument_id;
  fill.side = event.side;
  fill.price_ticks = event.price_ticks;
  fill.quantity_units = event.quantity_units;
  fill.fee_currency_nanos = event.fee_currency_nanos;
  fill.application_ordinal = ++event_ordinal_;
  fill.action_generation =
      corporate_action_generations_[find_instrument(event.instrument_id)];
  fill.source_mask = source_mask(event.fill_source);
  fill.active = true;
  fill.occupied = true;
  auto position_copy = positions_[position_index];
  if (!apply_fill_accounting(position_copy, fill)) {
    set_unsafe(InvariantCode::arithmetic_overflow);
    return {.status = ApplyStatus::arithmetic_overflow,
            .invariant = InvariantCode::arithmetic_overflow,
            .state_changed = true};
  }
  positions_[position_index] = position_copy;
  fills_[fill_index] = fill;
  if (order_index != kNotFound) {
    auto& order = orders_[order_index];
    order.cumulative_fill_quantity_units += event.quantity_units;
    order.remaining_quantity_units -= event.quantity_units;
    order.state = order.remaining_quantity_units == 0U
                      ? OrderLifecycleState::filled
                      : OrderLifecycleState::partially_filled;
    order.last_update_time_ns = event.process_monotonic_time_ns;
  }
  return {.status = ApplyStatus::applied,
          .invariant = InvariantCode::none,
          .state_changed = true};
}

bool PortfolioRiskService::rebuild_position(const std::size_t position_index) noexcept {
  if (position_index >= positions_.size() || !positions_[position_index].occupied) {
    return false;
  }
  auto rebuilt = positions_[position_index];
  rebuilt.quantity_units = rebuilt.base_quantity_units;
  rebuilt.open_cost_currency_nanos = rebuilt.base_open_cost_currency_nanos;
  rebuilt.realized_pnl_currency_nanos = rebuilt.base_realized_pnl_currency_nanos;
  std::array<std::size_t, kMaximumPortfolioFills> matching{};
  std::size_t count{};
  for (std::size_t index = 0U; index < fills_.size(); ++index) {
    const auto& fill = fills_[index];
    if (fill.occupied && fill.active && fill.account_id == rebuilt.account_id &&
        fill.strategy_id == rebuilt.strategy_id &&
        fill.instrument_id == rebuilt.instrument_id &&
        fill.action_generation == rebuilt.action_generation) {
      matching[count++] = index;
    }
  }
  std::sort(matching.begin(), matching.begin() + static_cast<std::ptrdiff_t>(count),
            [this](const std::size_t left, const std::size_t right) {
              return fills_[left].application_ordinal <
                     fills_[right].application_ordinal;
            });
  for (std::size_t index = 0U; index < count; ++index) {
    if (!apply_fill_accounting(rebuilt, fills_[matching[index]])) {
      return false;
    }
  }
  positions_[position_index] = rebuilt;
  return true;
}

PortfolioRiskService::InternalApplyResult
PortfolioRiskService::apply_fill_correction(const PortfolioEvent& event) noexcept {
  const auto fill_index = find_fill(event.target_fill_id);
  if (fill_index == kNotFound || event.logical_fill_id != event.target_fill_id) {
    return {.status = ApplyStatus::requires_reconciliation,
            .invariant = InvariantCode::accounting_mismatch};
  }
  auto& fill = fills_[fill_index];
  const auto instrument_index = find_instrument(fill.instrument_id);
  if (instrument_index == kNotFound ||
      fill.action_generation != corporate_action_generations_[instrument_index] ||
      fill.account_id != event.account_id || fill.strategy_id != event.strategy_id ||
      fill.instrument_id != event.instrument_id || fill.venue_id != event.venue_id) {
    return {.status = ApplyStatus::requires_reconciliation,
            .invariant = InvariantCode::unsupported_corporate_action};
  }
  const auto position_index =
      find_position(fill.account_id, fill.strategy_id, fill.instrument_id);
  if (position_index == kNotFound) {
    set_unsafe(InvariantCode::accounting_mismatch);
    return {.status = ApplyStatus::conflict,
            .invariant = InvariantCode::accounting_mismatch,
            .state_changed = true};
  }
  const auto previous = fill;
  fill.order_id = event.order_id;
  fill.side = event.side;
  fill.price_ticks = event.price_ticks;
  fill.quantity_units = event.quantity_units;
  fill.fee_currency_nanos = event.fee_currency_nanos;
  if (!rebuild_position(position_index)) {
    fill = previous;
    static_cast<void>(rebuild_position(position_index));
    set_unsafe(InvariantCode::arithmetic_overflow);
    return {.status = ApplyStatus::arithmetic_overflow,
            .invariant = InvariantCode::arithmetic_overflow,
            .state_changed = true};
  }
  const auto order_index = find_order(fill.order_id);
  if (order_index != kNotFound) {
    std::uint64_t cumulative{};
    for (const auto& candidate : fills_) {
      if (candidate.occupied && candidate.active &&
          candidate.order_id == fill.order_id &&
          !checked_add_unsigned(cumulative, candidate.quantity_units, cumulative)) {
        fill = previous;
        static_cast<void>(rebuild_position(position_index));
        set_unsafe(InvariantCode::arithmetic_overflow);
        return {.status = ApplyStatus::arithmetic_overflow,
                .invariant = InvariantCode::arithmetic_overflow,
                .state_changed = true};
      }
    }
    auto& order = orders_[order_index];
    if (cumulative > order.quantity_units) {
      fill = previous;
      static_cast<void>(rebuild_position(position_index));
      set_unsafe(InvariantCode::conflicting_fill);
      return {.status = ApplyStatus::conflict,
              .invariant = InvariantCode::conflicting_fill,
              .state_changed = true};
    }
    order.cumulative_fill_quantity_units = cumulative;
    order.remaining_quantity_units = order.quantity_units - cumulative;
    order.state = order.remaining_quantity_units == 0U
                      ? OrderLifecycleState::filled
                      : OrderLifecycleState::partially_filled;
  }
  correction_count_.fetch_add(1U, std::memory_order_relaxed);
  return {.status = ApplyStatus::applied,
          .invariant = InvariantCode::none,
          .state_changed = true};
}

PortfolioRiskService::InternalApplyResult
PortfolioRiskService::apply_fill_bust(const PortfolioEvent& event) noexcept {
  const auto fill_index = find_fill(event.target_fill_id);
  if (fill_index == kNotFound) {
    return {.status = ApplyStatus::requires_reconciliation,
            .invariant = InvariantCode::accounting_mismatch};
  }
  auto& fill = fills_[fill_index];
  if (!fill.active) {
    return {.status = ApplyStatus::duplicate, .invariant = InvariantCode::none};
  }
  const auto instrument_index = find_instrument(fill.instrument_id);
  if (instrument_index == kNotFound ||
      fill.action_generation != corporate_action_generations_[instrument_index]) {
    return {.status = ApplyStatus::requires_reconciliation,
            .invariant = InvariantCode::unsupported_corporate_action};
  }
  const auto position_index =
      find_position(fill.account_id, fill.strategy_id, fill.instrument_id);
  if (position_index == kNotFound) {
    set_unsafe(InvariantCode::accounting_mismatch);
    return {.status = ApplyStatus::conflict,
            .invariant = InvariantCode::accounting_mismatch,
            .state_changed = true};
  }
  fill.active = false;
  if (!rebuild_position(position_index)) {
    fill.active = true;
    static_cast<void>(rebuild_position(position_index));
    set_unsafe(InvariantCode::arithmetic_overflow);
    return {.status = ApplyStatus::arithmetic_overflow,
            .invariant = InvariantCode::arithmetic_overflow,
            .state_changed = true};
  }
  const auto order_index = find_order(fill.order_id);
  if (order_index != kNotFound) {
    auto& order = orders_[order_index];
    if (order.cumulative_fill_quantity_units < fill.quantity_units ||
        !checked_add_unsigned(order.remaining_quantity_units, fill.quantity_units,
                              order.remaining_quantity_units)) {
      fill.active = true;
      static_cast<void>(rebuild_position(position_index));
      set_unsafe(InvariantCode::accounting_mismatch);
      return {.status = ApplyStatus::conflict,
              .invariant = InvariantCode::accounting_mismatch,
              .state_changed = true};
    }
    order.cumulative_fill_quantity_units -= fill.quantity_units;
    order.state = order.cumulative_fill_quantity_units == 0U
                      ? OrderLifecycleState::open
                      : OrderLifecycleState::partially_filled;
  }
  bust_count_.fetch_add(1U, std::memory_order_relaxed);
  return {.status = ApplyStatus::applied,
          .invariant = InvariantCode::none,
          .state_changed = true};
}

PortfolioRiskService::InternalApplyResult
PortfolioRiskService::apply_mark(const PortfolioEvent& event) noexcept {
  const auto index = find_instrument(event.instrument_id);
  if (index == kNotFound || event.process_monotonic_time_ns < mark_times_[index]) {
    return {.status = ApplyStatus::invalid, .invariant = InvariantCode::stale_event};
  }
  marks_[index] = event.price_ticks;
  mark_times_[index] = event.process_monotonic_time_ns;
  return {.status = ApplyStatus::applied,
          .invariant = InvariantCode::none,
          .state_changed = true};
}

void PortfolioRiskService::clear_session_state(const bool carry_positions) noexcept {
  orders_ = {};
  fills_ = {};
  idempotency_ = {};
  marks_ = {};
  mark_times_ = {};
  corporate_action_generations_ = {};
  event_ordinal_ = 0U;
  strategy_peaks_ = {};
  snapshot_.peak_pnl_currency_nanos = 0;
  for (auto& position : positions_) {
    if (!position.occupied) {
      continue;
    }
    if (!carry_positions || position.quantity_units == 0) {
      position = {};
      continue;
    }
    position.realized_pnl_currency_nanos = 0;
    position.base_quantity_units = position.quantity_units;
    position.base_open_cost_currency_nanos = position.open_cost_currency_nanos;
    position.base_realized_pnl_currency_nanos = 0;
    position.action_generation = 0U;
  }
}

PortfolioRiskService::InternalApplyResult
PortfolioRiskService::apply_rollover(const PortfolioEvent& event) noexcept {
  for (const auto& order : orders_) {
    if (order.occupied && active_order_state(order.state)) {
      return {.status = ApplyStatus::requires_reconciliation,
              .invariant = InvariantCode::accounting_mismatch};
    }
  }
  if (event.rollover_policy == RolloverPolicy::flat_required) {
    for (const auto& position : positions_) {
      if (position.occupied && position.quantity_units != 0) {
        return {.status = ApplyStatus::requires_reconciliation,
                .invariant = InvariantCode::accounting_mismatch};
      }
    }
  }
  const bool carry = event.rollover_policy == RolloverPolicy::carry_positions;
  clear_session_state(carry);
  configuration_.session_id = event.next_session_id;
  configuration_.stable_hash = stable_configuration_hash(configuration_);
  invariant_ = InvariantCode::none;
  health_ = PortfolioHealth::starting;
  return {.status = ApplyStatus::applied,
          .invariant = InvariantCode::none,
          .state_changed = true};
}

PortfolioRiskService::InternalApplyResult
PortfolioRiskService::apply_corporate_action(const PortfolioEvent& event) noexcept {
  const auto instrument_index = find_instrument(event.instrument_id);
  if (instrument_index == kNotFound) {
    return {.status = ApplyStatus::invalid,
            .invariant = InvariantCode::unsupported_corporate_action};
  }
  if (!corporate_action_is_exact(event, instrument_index)) {
    return {.status = ApplyStatus::requires_reconciliation,
            .invariant = InvariantCode::unsupported_corporate_action};
  }
  commit_corporate_action(event, instrument_index);
  return {.status = ApplyStatus::applied,
          .invariant = InvariantCode::none,
          .state_changed = true};
}

bool PortfolioRiskService::corporate_action_is_exact(
    const PortfolioEvent& event, const std::size_t instrument_index) const noexcept {
  const auto numerator = event.corporate_action_numerator;
  const auto denominator = event.corporate_action_denominator;
  for (const auto& position : positions_) {
    if (!position.occupied || position.instrument_id != event.instrument_id) {
      continue;
    }
    std::uint64_t absolute_quantity{};
    std::uint64_t product{};
    if (!checked_abs(position.quantity_units, absolute_quantity) ||
        !checked_multiply(absolute_quantity, numerator, product) ||
        product % denominator != 0U ||
        product / denominator >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
  }
  for (const auto& order : orders_) {
    if (!order.occupied || order.instrument_id != event.instrument_id) {
      continue;
    }
    std::uint64_t quantity_product{};
    std::uint64_t cumulative_product{};
    std::uint64_t remaining_product{};
    std::uint64_t price_product{};
    if (!checked_multiply(order.quantity_units, numerator, quantity_product) ||
        !checked_multiply(order.cumulative_fill_quantity_units, numerator,
                          cumulative_product) ||
        !checked_multiply(order.remaining_quantity_units, numerator,
                          remaining_product) ||
        !checked_multiply(static_cast<std::uint64_t>(order.price_ticks), denominator,
                          price_product) ||
        quantity_product % denominator != 0U ||
        cumulative_product % denominator != 0U ||
        remaining_product % denominator != 0U || price_product % numerator != 0U ||
        price_product / numerator >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
  }
  if (marks_[instrument_index] > 0) {
    std::uint64_t mark_product{};
    if (!checked_multiply(static_cast<std::uint64_t>(marks_[instrument_index]),
                          denominator, mark_product) ||
        mark_product % numerator != 0U ||
        mark_product / numerator >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
  }
  return true;
}

void PortfolioRiskService::commit_corporate_action(
    const PortfolioEvent& event, const std::size_t instrument_index) noexcept {
  const auto numerator = event.corporate_action_numerator;
  const auto denominator = event.corporate_action_denominator;
  ++corporate_action_generations_[instrument_index];
  for (auto& position : positions_) {
    if (!position.occupied || position.instrument_id != event.instrument_id) {
      continue;
    }
    std::uint64_t absolute_quantity{};
    static_cast<void>(checked_abs(position.quantity_units, absolute_quantity));
    const auto adjusted = absolute_quantity * numerator / denominator;
    position.quantity_units = position.quantity_units < 0
                                  ? -static_cast<std::int64_t>(adjusted)
                                  : static_cast<std::int64_t>(adjusted);
    position.base_quantity_units = position.quantity_units;
    position.base_open_cost_currency_nanos = position.open_cost_currency_nanos;
    position.base_realized_pnl_currency_nanos = position.realized_pnl_currency_nanos;
    position.action_generation = corporate_action_generations_[instrument_index];
  }
  for (auto& order : orders_) {
    if (!order.occupied || order.instrument_id != event.instrument_id) {
      continue;
    }
    order.quantity_units = order.quantity_units * numerator / denominator;
    order.cumulative_fill_quantity_units =
        order.cumulative_fill_quantity_units * numerator / denominator;
    order.remaining_quantity_units =
        order.remaining_quantity_units * numerator / denominator;
    order.price_ticks = static_cast<std::int64_t>(
        static_cast<std::uint64_t>(order.price_ticks) * denominator / numerator);
  }
  if (marks_[instrument_index] > 0) {
    marks_[instrument_index] = static_cast<std::int64_t>(
        static_cast<std::uint64_t>(marks_[instrument_index]) * denominator / numerator);
  }
}

bool PortfolioRiskService::recompute_snapshot(
    PortfolioSnapshot& output, const SnapshotBuildContext context) const noexcept {
  bool missing_mark = false;
  initialize_snapshot(output, context, missing_mark);
  if (!accumulate_orders(output)) {
    return false;
  }
  accumulate_fills(output);
  return accumulate_positions(output) && finalize_snapshot(output, missing_mark);
}

void PortfolioRiskService::initialize_snapshot(PortfolioSnapshot& output,
                                               const SnapshotBuildContext context,
                                               bool& missing_mark) const noexcept {
  output = {};
  output.schema_major = kPortfolioSchemaMajor;
  output.schema_minor = kPortfolioSchemaMinor;
  output.risk_snapshot_id =
      common::RiskSnapshotId{configuration_.stable_hash, snapshot_sequence_};
  output.session_id = configuration_.session_id;
  output.configuration_version = configuration_.configuration_version;
  output.sequence = snapshot_sequence_;
  output.source_journal_sequence = context.journal_sequence;
  output.as_of_process_monotonic_time_ns = context.event_time_ns;
  output.instrument_count = configuration_.instrument_count;
  output.strategy_count = configuration_.strategy_count;
  output.account_count = configuration_.account_count;
  output.sector_count = configuration_.sector_count;
  output.invariant_failure_count = invariant_failure_count_;
  missing_mark = false;
  for (std::size_t index = 0U; index < configuration_.instrument_count; ++index) {
    output.instruments[index].instrument_id =
        configuration_.instruments[index].instrument_id;
    output.instruments[index].mark_price_ticks = marks_[index];
    missing_mark = missing_mark || marks_[index] <= 0;
  }
  for (std::size_t index = 0U; index < configuration_.strategy_count; ++index) {
    output.strategies[index].strategy_id = configuration_.strategies[index];
  }
  for (std::size_t index = 0U; index < configuration_.account_count; ++index) {
    output.accounts[index].account_id = configuration_.accounts[index];
  }
  for (std::size_t index = 0U; index < configuration_.sector_count; ++index) {
    output.sectors[index].sector_index = static_cast<std::uint32_t>(index);
  }
}

bool PortfolioRiskService::accumulate_orders(PortfolioSnapshot& output) const noexcept {
  for (const auto& order : orders_) {
    if (!order.occupied || !active_order_state(order.state)) {
      continue;
    }
    const auto instrument_index = find_instrument(order.instrument_id);
    if (instrument_index == kNotFound) {
      return false;
    }
    ++output.active_order_count;
    auto& instrument = output.instruments[instrument_index];
    auto& pending = order.side == Side::buy ? instrument.pending_buy_quantity_units
                                            : instrument.pending_sell_quantity_units;
    if (!add_unsigned(pending, order.remaining_quantity_units)) {
      return false;
    }
  }
  return true;
}

void PortfolioRiskService::accumulate_fills(PortfolioSnapshot& output) const noexcept {
  for (const auto& fill : fills_) {
    if (!fill.occupied || !fill.active) {
      continue;
    }
    ++output.active_fill_count;
    if ((fill.source_mask & kPrimarySourceMask) != 0U &&
        (fill.source_mask & kDropCopySourceMask) == 0U) {
      ++output.unmatched_primary_fill_count;
    }
    if ((fill.source_mask & kDropCopySourceMask) != 0U &&
        (fill.source_mask & kPrimarySourceMask) == 0U) {
      ++output.unmatched_drop_copy_fill_count;
    }
    if (find_order(fill.order_id) == kNotFound) {
      ++output.orphan_fill_count;
    }
  }
}

bool PortfolioRiskService::accumulate_position(
    PortfolioSnapshot& output, const PositionSlot& position) const noexcept {
  const auto instrument_index = find_instrument(position.instrument_id);
  const auto strategy_index = find_strategy(position.strategy_id);
  const auto account_index = find_account(position.account_id);
  if (instrument_index == kNotFound || strategy_index == kNotFound ||
      account_index == kNotFound || output.position_count >= output.positions.size()) {
    return false;
  }
  const auto& reference = configuration_.instruments[instrument_index];
  auto& instrument = output.instruments[instrument_index];
  auto& strategy = output.strategies[strategy_index];
  auto& account = output.accounts[account_index];
  auto& sector = output.sectors[reference.sector_index];
  auto& position_output = output.positions[output.position_count++];
  position_output.account_id = position.account_id;
  position_output.strategy_id = position.strategy_id;
  position_output.instrument_id = position.instrument_id;
  position_output.net_quantity_units = position.quantity_units;
  position_output.open_cost_currency_nanos = position.open_cost_currency_nanos;
  position_output.realized_pnl_currency_nanos = position.realized_pnl_currency_nanos;
  std::uint64_t absolute_position_cost{};
  std::uint64_t absolute_position_quantity{};
  std::uint64_t average_denominator{};
  if (!checked_abs(position.open_cost_currency_nanos, absolute_position_cost) ||
      !checked_abs(position.quantity_units, absolute_position_quantity) ||
      (absolute_position_quantity != 0U &&
       !checked_multiply(absolute_position_quantity,
                         reference.tick_value_currency_nanos, average_denominator))) {
    return false;
  }
  if (average_denominator != 0U) {
    position_output.average_price_ticks =
        static_cast<std::int64_t>(absolute_position_cost / average_denominator);
  }
  if (!add_signed(instrument.net_quantity_units, position.quantity_units) ||
      !add_signed(instrument.realized_pnl_currency_nanos,
                  position.realized_pnl_currency_nanos) ||
      !add_signed(output.realized_pnl_currency_nanos,
                  position.realized_pnl_currency_nanos)) {
    return false;
  }
  if (position.quantity_units == 0) {
    return true;
  }
  if (marks_[instrument_index] <= 0) {
    return true;
  }
  std::uint64_t absolute_quantity{};
  std::int64_t absolute_notional_signed{};
  if (!checked_abs(position.quantity_units, absolute_quantity) ||
      !checked_notional(marks_[instrument_index], reference.tick_value_currency_nanos,
                        absolute_quantity, absolute_notional_signed)) {
    return false;
  }
  const auto signed_notional = position.quantity_units < 0 ? -absolute_notional_signed
                                                           : absolute_notional_signed;
  std::int64_t unrealized{};
  if (!checked_subtract(signed_notional, position.open_cost_currency_nanos,
                        unrealized) ||
      !add_signed(instrument.unrealized_pnl_currency_nanos, unrealized) ||
      !add_signed(output.unrealized_pnl_currency_nanos, unrealized)) {
    return false;
  }
  position_output.unrealized_pnl_currency_nanos = unrealized;
  const auto absolute_notional = static_cast<std::uint64_t>(absolute_notional_signed);
  if (!add_unsigned(instrument.gross_exposure_currency_nanos, absolute_notional) ||
      !add_signed(instrument.net_exposure_currency_nanos, signed_notional) ||
      !add_unsigned(strategy.gross_exposure_currency_nanos, absolute_notional) ||
      !add_signed(strategy.net_exposure_currency_nanos, signed_notional) ||
      !add_unsigned(account.gross_exposure_currency_nanos, absolute_notional) ||
      !add_signed(account.net_exposure_currency_nanos, signed_notional) ||
      !add_unsigned(sector.gross_exposure_currency_nanos, absolute_notional) ||
      !add_signed(sector.net_exposure_currency_nanos, signed_notional) ||
      !add_unsigned(output.gross_exposure_currency_nanos, absolute_notional) ||
      !add_signed(output.net_exposure_currency_nanos, signed_notional)) {
    return false;
  }
  std::int64_t beta{};
  std::uint64_t liquidity{};
  std::uint64_t event_exposure{};
  if (!scaled_signed(signed_notional, reference.beta_ppm, beta) ||
      !scaled_unsigned(absolute_notional, reference.liquidity_weight_ppm, liquidity) ||
      !scaled_unsigned(absolute_notional, reference.event_weight_ppm, event_exposure) ||
      !add_signed(output.beta_exposure_currency_nanos, beta) ||
      !add_unsigned(output.liquidity_adjusted_exposure_currency_nanos, liquidity) ||
      !add_unsigned(output.event_exposure_currency_nanos, event_exposure)) {
    return false;
  }
  std::int64_t position_pnl{};
  return checked_add(position.realized_pnl_currency_nanos, unrealized, position_pnl) &&
         add_signed(strategy.pnl_currency_nanos, position_pnl) &&
         add_signed(account.pnl_currency_nanos, position_pnl);
}

bool PortfolioRiskService::accumulate_positions(
    PortfolioSnapshot& output) const noexcept {
  for (const auto& position : positions_) {
    if (!position.occupied) {
      continue;
    }
    if (!accumulate_position(output, position)) {
      return false;
    }
  }
  return true;
}

bool PortfolioRiskService::finalize_snapshot(PortfolioSnapshot& output,
                                             const bool missing_mark) const noexcept {
  if (!checked_add(output.realized_pnl_currency_nanos,
                   output.unrealized_pnl_currency_nanos,
                   output.total_pnl_currency_nanos)) {
    return false;
  }
  output.peak_pnl_currency_nanos =
      std::max(snapshot_.peak_pnl_currency_nanos, output.total_pnl_currency_nanos);
  if (output.peak_pnl_currency_nanos > output.total_pnl_currency_nanos) {
    std::int64_t drawdown{};
    if (!checked_subtract(output.peak_pnl_currency_nanos,
                          output.total_pnl_currency_nanos, drawdown)) {
      return false;
    }
    output.drawdown_currency_nanos = static_cast<std::uint64_t>(drawdown);
  }
  for (std::size_t index = 0U; index < output.strategy_count; ++index) {
    auto& strategy = output.strategies[index];
    strategy.peak_pnl_currency_nanos =
        std::max(strategy_peaks_[index], strategy.pnl_currency_nanos);
    if (strategy.peak_pnl_currency_nanos > strategy.pnl_currency_nanos) {
      std::int64_t drawdown{};
      if (!checked_subtract(strategy.peak_pnl_currency_nanos,
                            strategy.pnl_currency_nanos, drawdown)) {
        return false;
      }
      strategy.drawdown_currency_nanos = static_cast<std::uint64_t>(drawdown);
    }
  }
  if (stopped_.load(std::memory_order_relaxed)) {
    output.health = PortfolioHealth::stopped;
    output.invariant = invariant_;
    output.ready = false;
  } else if (health_ == PortfolioHealth::unsafe ||
             invariant_ == InvariantCode::conflicting_fill ||
             invariant_ == InvariantCode::arithmetic_overflow ||
             invariant_ == InvariantCode::accounting_mismatch ||
             invariant_ == InvariantCode::journal_exhausted) {
    output.health = PortfolioHealth::unsafe;
    output.invariant = invariant_;
    output.ready = false;
  } else if (missing_mark || output.orphan_fill_count != 0U ||
             (configuration_.require_drop_copy_confirmation &&
              (output.unmatched_primary_fill_count != 0U ||
               output.unmatched_drop_copy_fill_count != 0U))) {
    output.health = PortfolioHealth::reconciling;
    output.invariant = invariant_;
    if (missing_mark) {
      output.invariant = InvariantCode::stale_event;
    } else if (output.orphan_fill_count != 0U) {
      output.invariant = InvariantCode::orphan_fill;
    }
    output.ready = false;
  } else {
    output.health = PortfolioHealth::healthy;
    output.invariant = InvariantCode::none;
    output.ready = true;
  }
  output.stable_hash = stable_snapshot_hash(output);
  return true;
}

bool PortfolioRiskService::refresh_snapshot(
    const std::uint64_t event_time_ns, const std::uint64_t journal_sequence) noexcept {
  PortfolioSnapshot next;
  const SnapshotBuildContext context{.event_time_ns = event_time_ns,
                                     .journal_sequence = journal_sequence};
  if (!recompute_snapshot(next, context)) {
    return false;
  }
  snapshot_ = next;
  const auto status = snapshot_store_.publish(snapshot_, event_time_ns);
  return status == event_bus::SnapshotPublishStatus::published;
}

bool PortfolioRiskService::verify_invariants_locked() const noexcept {
  for (const auto& position : positions_) {
    if (!position.occupied) {
      continue;
    }
    if ((position.quantity_units == 0 && position.open_cost_currency_nanos != 0) ||
        (position.quantity_units > 0 && position.open_cost_currency_nanos < 0) ||
        (position.quantity_units < 0 && position.open_cost_currency_nanos > 0) ||
        find_account(position.account_id) == kNotFound ||
        find_strategy(position.strategy_id) == kNotFound ||
        find_instrument(position.instrument_id) == kNotFound) {
      return false;
    }
  }
  for (const auto& fill : fills_) {
    if (fill.occupied &&
        (!fill.logical_fill_id.valid() || fill.quantity_units == 0U ||
         fill.price_ticks <= 0 || find_instrument(fill.instrument_id) == kNotFound)) {
      return false;
    }
  }
  return snapshot_.stable_hash == stable_snapshot_hash(snapshot_);
}

bool PortfolioRiskService::verify_invariants() noexcept {
  if (!try_enter()) {
    return false;
  }
  const auto valid = verify_invariants_locked();
  if (!valid) {
    set_unsafe(InvariantCode::accounting_mismatch);
  }
  leave();
  return valid;
}

void PortfolioRiskService::set_unsafe(const InvariantCode code) noexcept {
  health_ = PortfolioHealth::unsafe;
  invariant_ = code;
  ++invariant_failure_count_;
}

PortfolioSnapshot
PortfolioRiskService::snapshot_for_quiescent_inspection() const noexcept {
  return snapshot_;
}

event_bus::SnapshotReadStatus PortfolioRiskService::acquire_snapshot(
    const std::uint64_t acquired_at_ns,
    PortfolioSnapshotStore::ReadHandle& output) noexcept {
  return snapshot_store_.acquire(acquired_at_ns, output);
}

StressResult
PortfolioRiskService::evaluate_stress(StressConfiguration configuration) noexcept {
  StressResult result;
  if (configuration.stable_hash == 0U ||
      stable_stress_configuration_hash(configuration) != configuration.stable_hash ||
      configuration.sector_index >= configuration_.sector_count ||
      configuration.volatility_loss_ppm > kPortfolioPpm ||
      configuration.liquidity_loss_ppm > kPortfolioPpm) {
    return result;
  }
  PortfolioSnapshotStore::ReadHandle handle;
  if (snapshot_store_.acquire(snapshot_.as_of_process_monotonic_time_ns, handle) !=
          event_bus::SnapshotReadStatus::snapshot ||
      !handle->ready) {
    return result;
  }
  const auto& value = *handle;
  result.snapshot_sequence = value.sequence;
  result.snapshot_hash = value.stable_hash;
  std::array<std::int64_t, 6U> impacts{};
  if (!scaled_signed(value.net_exposure_currency_nanos, configuration.market_gap_ppm,
                     impacts[0U])) {
    return {};
  }
  std::uint64_t volatility_loss{};
  if (!scaled_unsigned(value.gross_exposure_currency_nanos,
                       configuration.volatility_loss_ppm, volatility_loss) ||
      volatility_loss >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return {};
  }
  impacts[1U] = -static_cast<std::int64_t>(volatility_loss);
  if (!scaled_signed(
          value.sectors[configuration.sector_index].net_exposure_currency_nanos,
          configuration.sector_shock_ppm, impacts[2U]) ||
      !scaled_signed(value.beta_exposure_currency_nanos,
                     configuration.correlated_selloff_ppm, impacts[3U])) {
    return {};
  }
  const auto excess_liquidity = value.liquidity_adjusted_exposure_currency_nanos >
                                        value.gross_exposure_currency_nanos
                                    ? value.liquidity_adjusted_exposure_currency_nanos -
                                          value.gross_exposure_currency_nanos
                                    : 0U;
  std::uint64_t liquidity_loss{};
  if (!scaled_unsigned(excess_liquidity, configuration.liquidity_loss_ppm,
                       liquidity_loss) ||
      liquidity_loss >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return {};
  }
  impacts[4U] = -static_cast<std::int64_t>(liquidity_loss);
  for (std::size_t index = 0U; index < configuration_.instrument_count; ++index) {
    if (value.instruments[index].net_quantity_units != 0 &&
        !add_signed(
            impacts[5U],
            configuration_.instruments[index].options_gamma_stress_currency_nanos)) {
      return {};
    }
  }
  for (std::size_t index = 0U; index < result.outcomes.size(); ++index) {
    result.outcomes[index].scenario =
        static_cast<StressScenario>(static_cast<std::uint8_t>(index + 1U));
    result.outcomes[index].pnl_impact_currency_nanos = impacts[index];
    if (!checked_add(value.total_pnl_currency_nanos, impacts[index],
                     result.outcomes[index].stressed_total_pnl_currency_nanos)) {
      return {};
    }
  }
  result.valid = true;
  result.stable_hash = stable_stress_result_hash(result);
  return result;
}

RecoveryStatus
PortfolioRiskService::recover_from(const PortfolioJournal& source) noexcept {
  if (&source == &journal_ || journal_.size() != 0U || !initialized()) {
    return RecoveryStatus::busy;
  }
  for (std::uint64_t sequence = 1U; sequence <= source.size(); ++sequence) {
    PortfolioJournalRecord expected;
    if (!source.read(sequence, expected)) {
      return RecoveryStatus::source_corrupt;
    }
    const auto actual = apply(expected.event);
    if (actual.status == ApplyStatus::journal_unavailable) {
      return RecoveryStatus::target_journal_full;
    }
    if (actual.status != expected.result.status ||
        actual.invariant != expected.result.invariant ||
        actual.snapshot_sequence != expected.result.snapshot_sequence ||
        actual.snapshot_hash != expected.result.snapshot_hash) {
      return RecoveryStatus::replay_diverged;
    }
  }
  return RecoveryStatus::recovered;
}

PortfolioMetrics PortfolioRiskService::metrics() const noexcept {
  return {.events = event_count_.load(std::memory_order_relaxed),
          .applied = applied_count_.load(std::memory_order_relaxed),
          .duplicates = duplicate_count_.load(std::memory_order_relaxed),
          .reconciled = reconciled_count_.load(std::memory_order_relaxed),
          .rejected = rejected_count_.load(std::memory_order_relaxed),
          .corrections = correction_count_.load(std::memory_order_relaxed),
          .busts = bust_count_.load(std::memory_order_relaxed),
          .invariant_failures = invariant_failure_count_,
          .snapshot_publications =
              snapshot_publication_count_.load(std::memory_order_relaxed),
          .snapshot_publish_failures =
              snapshot_publish_failure_count_.load(std::memory_order_relaxed),
          .journal_full_results = journal_full_count_.load(std::memory_order_relaxed)};
}

void PortfolioRiskService::shutdown(
    const std::uint64_t process_monotonic_time_ns) noexcept {
  if (!initialized() || process_monotonic_time_ns == 0U || !try_enter()) {
    return;
  }
  stopped_.store(true, std::memory_order_release);
  health_ = PortfolioHealth::stopped;
  ++snapshot_sequence_;
  if (refresh_snapshot(process_monotonic_time_ns, journal_.size())) {
    snapshot_publication_count_.fetch_add(1U, std::memory_order_relaxed);
  } else {
    snapshot_publish_failure_count_.fetch_add(1U, std::memory_order_relaxed);
  }
  leave();
}

} // namespace aegis::risk::portfolio
