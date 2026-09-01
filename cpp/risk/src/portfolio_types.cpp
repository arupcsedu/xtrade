#include "aegis/risk/portfolio_types.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace aegis::risk::portfolio {
namespace {

constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;

void mix(std::uint64_t& hash, const std::uint64_t value) noexcept {
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    hash ^= (value >> shift) & 0xFFU;
    hash *= kFnvPrime;
  }
}

template <typename Tag>
void mix_id(std::uint64_t& hash, const common::Identifier128<Tag> value) noexcept {
  mix(hash, value.high());
  mix(hash, value.low());
}

[[nodiscard]] std::uint64_t nonzero(const std::uint64_t value) noexcept {
  return value == 0U ? 1U : value;
}

template <typename Value>
[[nodiscard]] std::uint64_t signed_bits(const Value value) noexcept {
  using Unsigned = std::make_unsigned_t<Value>;
  return static_cast<std::uint64_t>(std::bit_cast<Unsigned>(value));
}

[[nodiscard]] bool valid_side(const Side value) noexcept {
  return value == Side::buy || value == Side::sell;
}

[[nodiscard]] bool valid_order_state(const OrderLifecycleState value) noexcept {
  return value >= OrderLifecycleState::accepted &&
         value <= OrderLifecycleState::rejected;
}

[[nodiscard]] bool valid_fill_source(const FillSource value) noexcept {
  return value == FillSource::primary || value == FillSource::drop_copy;
}

[[nodiscard]] bool duplicate_account(const PortfolioConfiguration& value,
                                     const std::size_t index) noexcept {
  for (std::size_t previous = 0U; previous < index; ++previous) {
    if (value.accounts[previous] == value.accounts[index]) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool duplicate_strategy(const PortfolioConfiguration& value,
                                      const std::size_t index) noexcept {
  for (std::size_t previous = 0U; previous < index; ++previous) {
    if (value.strategies[previous] == value.strategies[index]) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool duplicate_instrument(const PortfolioConfiguration& value,
                                        const std::size_t index) noexcept {
  for (std::size_t previous = 0U; previous < index; ++previous) {
    if (value.instruments[previous].instrument_id ==
        value.instruments[index].instrument_id) {
      return true;
    }
  }
  return false;
}

void mix_instrument_exposure(std::uint64_t& hash,
                             const InstrumentExposure& value) noexcept {
  mix_id(hash, value.instrument_id);
  mix(hash, signed_bits(value.net_quantity_units));
  mix(hash, value.pending_buy_quantity_units);
  mix(hash, value.pending_sell_quantity_units);
  mix(hash, signed_bits(value.mark_price_ticks));
  mix(hash, signed_bits(value.realized_pnl_currency_nanos));
  mix(hash, signed_bits(value.unrealized_pnl_currency_nanos));
  mix(hash, value.gross_exposure_currency_nanos);
  mix(hash, signed_bits(value.net_exposure_currency_nanos));
}

void mix_strategy_exposure(std::uint64_t& hash,
                           const StrategyExposure& value) noexcept {
  mix_id(hash, value.strategy_id);
  mix(hash, value.gross_exposure_currency_nanos);
  mix(hash, signed_bits(value.net_exposure_currency_nanos));
  mix(hash, signed_bits(value.pnl_currency_nanos));
  mix(hash, signed_bits(value.peak_pnl_currency_nanos));
  mix(hash, value.drawdown_currency_nanos);
}

} // namespace

bool valid_configuration(const PortfolioConfiguration& value) noexcept {
  if (!value.configuration_version.valid() || !value.session_id.valid() ||
      value.account_count == 0U || value.strategy_count == 0U ||
      value.instrument_count == 0U || value.sector_count == 0U ||
      value.account_count > kMaximumPortfolioAccounts ||
      value.strategy_count > kMaximumPortfolioStrategies ||
      value.instrument_count > kMaximumPortfolioInstruments ||
      value.sector_count > kMaximumPortfolioSectors) {
    return false;
  }
  for (std::size_t index = 0U; index < value.account_count; ++index) {
    if (!value.accounts[index].valid() || duplicate_account(value, index)) {
      return false;
    }
  }
  for (std::size_t index = 0U; index < value.strategy_count; ++index) {
    if (!value.strategies[index].valid() || duplicate_strategy(value, index)) {
      return false;
    }
  }
  for (std::size_t index = 0U; index < value.instrument_count; ++index) {
    const auto& instrument = value.instruments[index];
    if (!instrument.instrument_id.valid() ||
        instrument.tick_value_currency_nanos == 0U ||
        instrument.tick_value_currency_nanos >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        instrument.sector_index >= value.sector_count ||
        instrument.liquidity_weight_ppm < kPortfolioPpm ||
        instrument.event_weight_ppm > 10U * kPortfolioPpm ||
        instrument.liquidity_weight_ppm > 10U * kPortfolioPpm ||
        instrument.beta_ppm < -10 * static_cast<std::int32_t>(kPortfolioPpm) ||
        instrument.beta_ppm > 10 * static_cast<std::int32_t>(kPortfolioPpm) ||
        duplicate_instrument(value, index)) {
      return false;
    }
  }
  return value.stable_hash != 0U &&
         stable_configuration_hash(value) == value.stable_hash;
}

std::uint64_t stable_configuration_hash(PortfolioConfiguration value) noexcept {
  value.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix_id(hash, value.configuration_version);
  mix_id(hash, value.session_id);
  mix(hash, value.account_count);
  mix(hash, value.strategy_count);
  mix(hash, value.instrument_count);
  mix(hash, value.sector_count);
  mix(hash, value.require_drop_copy_confirmation ? 1U : 0U);
  for (std::size_t index = 0U;
       index < value.account_count && index < value.accounts.size(); ++index) {
    mix_id(hash, value.accounts[index]);
  }
  for (std::size_t index = 0U;
       index < value.strategy_count && index < value.strategies.size(); ++index) {
    mix_id(hash, value.strategies[index]);
  }
  for (std::size_t index = 0U;
       index < value.instrument_count && index < value.instruments.size(); ++index) {
    const auto& instrument = value.instruments[index];
    mix_id(hash, instrument.instrument_id);
    mix(hash, instrument.tick_value_currency_nanos);
    mix(hash, instrument.sector_index);
    mix(hash, signed_bits(instrument.beta_ppm));
    mix(hash, instrument.liquidity_weight_ppm);
    mix(hash, instrument.event_weight_ppm);
    mix(hash, signed_bits(instrument.options_gamma_stress_currency_nanos));
  }
  return nonzero(hash);
}

bool valid_event(const PortfolioEvent& value) noexcept {
  if (!value.event_id.valid() || !value.session_id.valid() ||
      !value.configuration_version.valid() || value.process_monotonic_time_ns == 0U ||
      value.stable_hash == 0U || stable_event_hash(value) != value.stable_hash) {
    return false;
  }
  switch (value.kind) {
  case PortfolioEventKind::order:
    return value.order_id.valid() && value.account_id.valid() &&
           value.strategy_id.valid() && value.venue_id.valid() &&
           value.instrument_id.valid() && valid_side(value.side) &&
           valid_order_state(value.order_state) && value.price_ticks > 0 &&
           value.quantity_units > 0U &&
           value.cumulative_fill_quantity_units <= value.quantity_units &&
           value.remaining_quantity_units <= value.quantity_units &&
           value.cumulative_fill_quantity_units + value.remaining_quantity_units <=
               value.quantity_units;
  case PortfolioEventKind::fill:
    return value.logical_fill_id.valid() && value.order_id.valid() &&
           value.account_id.valid() && value.strategy_id.valid() &&
           value.venue_id.valid() && value.instrument_id.valid() &&
           valid_side(value.side) && valid_fill_source(value.fill_source) &&
           value.price_ticks > 0 && value.quantity_units > 0U &&
           value.quantity_units <=
               static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  case PortfolioEventKind::fill_correction:
    return value.target_fill_id.valid() && value.logical_fill_id.valid() &&
           value.order_id.valid() && value.account_id.valid() &&
           value.strategy_id.valid() && value.venue_id.valid() &&
           value.instrument_id.valid() && valid_side(value.side) &&
           value.price_ticks > 0 && value.quantity_units > 0U &&
           value.quantity_units <=
               static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  case PortfolioEventKind::fill_bust:
    return value.target_fill_id.valid();
  case PortfolioEventKind::mark:
    return value.instrument_id.valid() && value.price_ticks > 0;
  case PortfolioEventKind::session_rollover:
    return value.next_session_id.valid() && value.next_session_id != value.session_id &&
           (value.rollover_policy == RolloverPolicy::flat_required ||
            value.rollover_policy == RolloverPolicy::carry_positions);
  case PortfolioEventKind::corporate_action:
    return value.instrument_id.valid() && value.corporate_action_numerator > 0U &&
           value.corporate_action_denominator > 0U &&
           (value.corporate_action_kind == CorporateActionKind::split ||
            value.corporate_action_kind == CorporateActionKind::reverse_split);
  }
  return false;
}

std::uint64_t stable_event_hash(PortfolioEvent value) noexcept {
  value.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix_id(hash, value.event_id);
  mix_id(hash, value.logical_fill_id);
  mix_id(hash, value.target_fill_id);
  mix_id(hash, value.session_id);
  mix_id(hash, value.next_session_id);
  mix_id(hash, value.configuration_version);
  mix_id(hash, value.order_id);
  mix_id(hash, value.account_id);
  mix_id(hash, value.strategy_id);
  mix_id(hash, value.venue_id);
  mix_id(hash, value.instrument_id);
  mix(hash, static_cast<std::uint8_t>(value.kind));
  mix(hash, static_cast<std::uint8_t>(value.side));
  mix(hash, static_cast<std::uint8_t>(value.order_state));
  mix(hash, static_cast<std::uint8_t>(value.fill_source));
  mix(hash, static_cast<std::uint8_t>(value.rollover_policy));
  mix(hash, static_cast<std::uint8_t>(value.corporate_action_kind));
  mix(hash, signed_bits(value.price_ticks));
  mix(hash, value.quantity_units);
  mix(hash, value.cumulative_fill_quantity_units);
  mix(hash, value.remaining_quantity_units);
  mix(hash, signed_bits(value.fee_currency_nanos));
  mix(hash, value.corporate_action_numerator);
  mix(hash, value.corporate_action_denominator);
  mix(hash, value.process_monotonic_time_ns);
  return nonzero(hash);
}

std::uint64_t stable_snapshot_hash(PortfolioSnapshot value) noexcept {
  value.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix(hash, value.schema_major);
  mix(hash, value.schema_minor);
  mix_id(hash, value.risk_snapshot_id);
  mix_id(hash, value.session_id);
  mix_id(hash, value.configuration_version);
  mix(hash, value.sequence);
  mix(hash, value.source_journal_sequence);
  mix(hash, value.as_of_process_monotonic_time_ns);
  mix(hash, static_cast<std::uint8_t>(value.health));
  mix(hash, static_cast<std::uint8_t>(value.invariant));
  mix(hash, value.ready ? 1U : 0U);
  mix(hash, value.instrument_count);
  mix(hash, value.strategy_count);
  mix(hash, value.account_count);
  mix(hash, value.sector_count);
  mix(hash, value.position_count);
  for (std::size_t index = 0U;
       index < value.instrument_count && index < value.instruments.size(); ++index) {
    mix_instrument_exposure(hash, value.instruments[index]);
  }
  for (std::size_t index = 0U;
       index < value.strategy_count && index < value.strategies.size(); ++index) {
    mix_strategy_exposure(hash, value.strategies[index]);
  }
  for (std::size_t index = 0U;
       index < value.position_count && index < value.positions.size(); ++index) {
    const auto& position = value.positions[index];
    mix_id(hash, position.account_id);
    mix_id(hash, position.strategy_id);
    mix_id(hash, position.instrument_id);
    mix(hash, signed_bits(position.net_quantity_units));
    mix(hash, signed_bits(position.average_price_ticks));
    mix(hash, signed_bits(position.open_cost_currency_nanos));
    mix(hash, signed_bits(position.realized_pnl_currency_nanos));
    mix(hash, signed_bits(position.unrealized_pnl_currency_nanos));
  }
  for (std::size_t index = 0U;
       index < value.account_count && index < value.accounts.size(); ++index) {
    const auto& account = value.accounts[index];
    mix_id(hash, account.account_id);
    mix(hash, account.gross_exposure_currency_nanos);
    mix(hash, signed_bits(account.net_exposure_currency_nanos));
    mix(hash, signed_bits(account.pnl_currency_nanos));
  }
  for (std::size_t index = 0U;
       index < value.sector_count && index < value.sectors.size(); ++index) {
    const auto& sector = value.sectors[index];
    mix(hash, sector.sector_index);
    mix(hash, sector.gross_exposure_currency_nanos);
    mix(hash, signed_bits(sector.net_exposure_currency_nanos));
  }
  mix(hash, value.gross_exposure_currency_nanos);
  mix(hash, signed_bits(value.net_exposure_currency_nanos));
  mix(hash, signed_bits(value.beta_exposure_currency_nanos));
  mix(hash, value.liquidity_adjusted_exposure_currency_nanos);
  mix(hash, value.event_exposure_currency_nanos);
  mix(hash, signed_bits(value.realized_pnl_currency_nanos));
  mix(hash, signed_bits(value.unrealized_pnl_currency_nanos));
  mix(hash, signed_bits(value.total_pnl_currency_nanos));
  mix(hash, signed_bits(value.peak_pnl_currency_nanos));
  mix(hash, value.drawdown_currency_nanos);
  mix(hash, value.active_order_count);
  mix(hash, value.active_fill_count);
  mix(hash, value.unmatched_primary_fill_count);
  mix(hash, value.unmatched_drop_copy_fill_count);
  mix(hash, value.orphan_fill_count);
  mix(hash, value.invariant_failure_count);
  return nonzero(hash);
}

std::uint64_t stable_stress_configuration_hash(StressConfiguration value) noexcept {
  value.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix(hash, signed_bits(value.market_gap_ppm));
  mix(hash, value.volatility_loss_ppm);
  mix(hash, value.sector_index);
  mix(hash, signed_bits(value.sector_shock_ppm));
  mix(hash, signed_bits(value.correlated_selloff_ppm));
  mix(hash, value.liquidity_loss_ppm);
  return nonzero(hash);
}

std::uint64_t stable_stress_result_hash(StressResult value) noexcept {
  value.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  for (const auto& outcome : value.outcomes) {
    mix(hash, static_cast<std::uint8_t>(outcome.scenario));
    mix(hash, signed_bits(outcome.pnl_impact_currency_nanos));
    mix(hash, signed_bits(outcome.stressed_total_pnl_currency_nanos));
  }
  mix(hash, value.snapshot_sequence);
  mix(hash, value.snapshot_hash);
  mix(hash, value.valid ? 1U : 0U);
  return nonzero(hash);
}

std::uint64_t portfolio_snapshot_checksum(const void* value) noexcept {
  if (value == nullptr) {
    return 0U;
  }
  return stable_snapshot_hash(*static_cast<const PortfolioSnapshot*>(value));
}

} // namespace aegis::risk::portfolio
