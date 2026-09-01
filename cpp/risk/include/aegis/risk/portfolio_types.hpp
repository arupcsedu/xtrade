#ifndef AEGIS_RISK_PORTFOLIO_TYPES_HPP
#define AEGIS_RISK_PORTFOLIO_TYPES_HPP

#include "aegis/common/identifiers.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace aegis::risk::portfolio {

inline constexpr std::uint32_t kPortfolioSchemaMajor = 1U;
inline constexpr std::uint32_t kPortfolioSchemaMinor = 0U;
inline constexpr std::uint32_t kPortfolioPpm = 1'000'000U;
inline constexpr std::size_t kMaximumPortfolioAccounts = 8U;
inline constexpr std::size_t kMaximumPortfolioStrategies = 32U;
inline constexpr std::size_t kMaximumPortfolioInstruments = 64U;
inline constexpr std::size_t kMaximumPortfolioSectors = 16U;
inline constexpr std::size_t kMaximumPortfolioPositions = 512U;
inline constexpr std::size_t kMaximumPortfolioOrders = 1'024U;
inline constexpr std::size_t kMaximumPortfolioFills = 4'096U;
inline constexpr std::size_t kPortfolioIdempotencyCapacity = 8'192U;

enum class Side : std::uint8_t { buy = 1, sell = 2 };

enum class OrderLifecycleState : std::uint8_t {
  accepted = 1,
  open = 2,
  partially_filled = 3,
  cancel_pending = 4,
  cancelled = 5,
  filled = 6,
  rejected = 7,
};

enum class FillSource : std::uint8_t { primary = 1, drop_copy = 2 };

enum class PortfolioEventKind : std::uint8_t {
  order = 1,
  fill = 2,
  fill_correction = 3,
  fill_bust = 4,
  mark = 5,
  session_rollover = 6,
  corporate_action = 7,
};

enum class RolloverPolicy : std::uint8_t {
  flat_required = 1,
  carry_positions = 2,
};

enum class CorporateActionKind : std::uint8_t {
  split = 1,
  reverse_split = 2,
};

enum class PortfolioHealth : std::uint8_t {
  starting = 1,
  healthy = 2,
  reconciling = 3,
  unsafe = 4,
  stopped = 5,
};

enum class ApplyStatus : std::uint8_t {
  applied = 1,
  duplicate = 2,
  reconciled = 3,
  invalid = 4,
  conflict = 5,
  capacity_exhausted = 6,
  journal_unavailable = 7,
  arithmetic_overflow = 8,
  requires_reconciliation = 9,
  busy = 10,
  stopped = 11,
};

enum class InvariantCode : std::uint8_t {
  none = 0,
  invalid_configuration = 1,
  conflicting_fill = 2,
  orphan_fill = 3,
  accounting_mismatch = 4,
  arithmetic_overflow = 5,
  stale_event = 6,
  unsupported_corporate_action = 7,
  journal_exhausted = 8,
};

enum class StressScenario : std::uint8_t {
  market_gap = 1,
  volatility_spike = 2,
  sector_shock = 3,
  correlated_selloff = 4,
  liquidity_collapse = 5,
  options_gamma_shock = 6,
};

// Fixed-layout value contracts intentionally expose aggregate members.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct InstrumentRiskReference {
  common::InstrumentId instrument_id;
  std::uint64_t tick_value_currency_nanos{};
  std::uint32_t sector_index{};
  std::int32_t beta_ppm{};
  std::uint32_t liquidity_weight_ppm{kPortfolioPpm};
  std::uint32_t event_weight_ppm{kPortfolioPpm};
  std::int64_t options_gamma_stress_currency_nanos{};
};

struct PortfolioConfiguration {
  common::ConfigurationVersion configuration_version;
  common::SessionId session_id;
  std::array<common::AccountId, kMaximumPortfolioAccounts> accounts{};
  std::array<common::StrategyId, kMaximumPortfolioStrategies> strategies{};
  std::array<InstrumentRiskReference, kMaximumPortfolioInstruments> instruments{};
  std::uint32_t account_count{};
  std::uint32_t strategy_count{};
  std::uint32_t instrument_count{};
  std::uint32_t sector_count{};
  bool require_drop_copy_confirmation{false};
  std::uint64_t stable_hash{};
};

struct PortfolioEvent {
  common::GlobalEventId event_id;
  common::GlobalEventId logical_fill_id;
  common::GlobalEventId target_fill_id;
  common::SessionId session_id;
  common::SessionId next_session_id;
  common::ConfigurationVersion configuration_version;
  common::OrderId order_id;
  common::AccountId account_id;
  common::StrategyId strategy_id;
  common::VenueId venue_id;
  common::InstrumentId instrument_id;
  PortfolioEventKind kind{PortfolioEventKind::order};
  Side side{Side::buy};
  OrderLifecycleState order_state{OrderLifecycleState::accepted};
  FillSource fill_source{FillSource::primary};
  RolloverPolicy rollover_policy{RolloverPolicy::flat_required};
  CorporateActionKind corporate_action_kind{CorporateActionKind::split};
  std::int64_t price_ticks{};
  std::uint64_t quantity_units{};
  std::uint64_t cumulative_fill_quantity_units{};
  std::uint64_t remaining_quantity_units{};
  std::int64_t fee_currency_nanos{};
  std::uint64_t corporate_action_numerator{};
  std::uint64_t corporate_action_denominator{};
  std::uint64_t process_monotonic_time_ns{};
  std::uint64_t stable_hash{};
};

struct ApplyResult {
  ApplyStatus status{ApplyStatus::invalid};
  InvariantCode invariant{InvariantCode::none};
  std::uint64_t journal_sequence{};
  std::uint64_t snapshot_sequence{};
  std::uint64_t snapshot_hash{};
};

struct InstrumentExposure {
  common::InstrumentId instrument_id;
  std::int64_t net_quantity_units{};
  std::uint64_t pending_buy_quantity_units{};
  std::uint64_t pending_sell_quantity_units{};
  std::int64_t mark_price_ticks{};
  std::int64_t realized_pnl_currency_nanos{};
  std::int64_t unrealized_pnl_currency_nanos{};
  std::uint64_t gross_exposure_currency_nanos{};
  std::int64_t net_exposure_currency_nanos{};
};

struct PositionExposure {
  common::AccountId account_id;
  common::StrategyId strategy_id;
  common::InstrumentId instrument_id;
  std::int64_t net_quantity_units{};
  std::int64_t average_price_ticks{};
  std::int64_t open_cost_currency_nanos{};
  std::int64_t realized_pnl_currency_nanos{};
  std::int64_t unrealized_pnl_currency_nanos{};
};

struct StrategyExposure {
  common::StrategyId strategy_id;
  std::uint64_t gross_exposure_currency_nanos{};
  std::int64_t net_exposure_currency_nanos{};
  std::int64_t pnl_currency_nanos{};
  std::int64_t peak_pnl_currency_nanos{};
  std::uint64_t drawdown_currency_nanos{};
};

struct AccountExposure {
  common::AccountId account_id;
  std::uint64_t gross_exposure_currency_nanos{};
  std::int64_t net_exposure_currency_nanos{};
  std::int64_t pnl_currency_nanos{};
};

struct SectorExposure {
  std::uint32_t sector_index{};
  std::uint64_t gross_exposure_currency_nanos{};
  std::int64_t net_exposure_currency_nanos{};
};

struct PortfolioSnapshot {
  std::uint32_t schema_major{kPortfolioSchemaMajor};
  std::uint32_t schema_minor{kPortfolioSchemaMinor};
  common::RiskSnapshotId risk_snapshot_id;
  common::SessionId session_id;
  common::ConfigurationVersion configuration_version;
  std::uint64_t sequence{};
  std::uint64_t source_journal_sequence{};
  std::uint64_t as_of_process_monotonic_time_ns{};
  PortfolioHealth health{PortfolioHealth::starting};
  InvariantCode invariant{InvariantCode::none};
  bool ready{false};
  std::uint32_t instrument_count{};
  std::uint32_t strategy_count{};
  std::uint32_t account_count{};
  std::uint32_t sector_count{};
  std::uint32_t position_count{};
  std::array<InstrumentExposure, kMaximumPortfolioInstruments> instruments{};
  std::array<PositionExposure, kMaximumPortfolioPositions> positions{};
  std::array<StrategyExposure, kMaximumPortfolioStrategies> strategies{};
  std::array<AccountExposure, kMaximumPortfolioAccounts> accounts{};
  std::array<SectorExposure, kMaximumPortfolioSectors> sectors{};
  std::uint64_t gross_exposure_currency_nanos{};
  std::int64_t net_exposure_currency_nanos{};
  std::int64_t beta_exposure_currency_nanos{};
  std::uint64_t liquidity_adjusted_exposure_currency_nanos{};
  std::uint64_t event_exposure_currency_nanos{};
  std::int64_t realized_pnl_currency_nanos{};
  std::int64_t unrealized_pnl_currency_nanos{};
  std::int64_t total_pnl_currency_nanos{};
  std::int64_t peak_pnl_currency_nanos{};
  std::uint64_t drawdown_currency_nanos{};
  std::uint64_t active_order_count{};
  std::uint64_t active_fill_count{};
  std::uint64_t unmatched_primary_fill_count{};
  std::uint64_t unmatched_drop_copy_fill_count{};
  std::uint64_t orphan_fill_count{};
  std::uint64_t invariant_failure_count{};
  std::uint64_t stable_hash{};
};

struct StressConfiguration {
  std::int32_t market_gap_ppm{-100'000};
  std::uint32_t volatility_loss_ppm{150'000U};
  std::uint32_t sector_index{};
  std::int32_t sector_shock_ppm{-200'000};
  std::int32_t correlated_selloff_ppm{-250'000};
  std::uint32_t liquidity_loss_ppm{500'000U};
  std::uint64_t stable_hash{};
};

struct StressOutcome {
  StressScenario scenario{StressScenario::market_gap};
  std::int64_t pnl_impact_currency_nanos{};
  std::int64_t stressed_total_pnl_currency_nanos{};
};

struct StressResult {
  std::array<StressOutcome, 6U> outcomes{};
  std::uint64_t snapshot_sequence{};
  std::uint64_t snapshot_hash{};
  bool valid{false};
  std::uint64_t stable_hash{};
};

struct PortfolioMetrics {
  std::uint64_t events{};
  std::uint64_t applied{};
  std::uint64_t duplicates{};
  std::uint64_t reconciled{};
  std::uint64_t rejected{};
  std::uint64_t corrections{};
  std::uint64_t busts{};
  std::uint64_t invariant_failures{};
  std::uint64_t snapshot_publications{};
  std::uint64_t snapshot_publish_failures{};
  std::uint64_t journal_full_results{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] bool valid_configuration(const PortfolioConfiguration& value) noexcept;
[[nodiscard]] std::uint64_t
stable_configuration_hash(PortfolioConfiguration value) noexcept;
[[nodiscard]] bool valid_event(const PortfolioEvent& value) noexcept;
[[nodiscard]] std::uint64_t stable_event_hash(PortfolioEvent value) noexcept;
[[nodiscard]] std::uint64_t stable_snapshot_hash(PortfolioSnapshot value) noexcept;
[[nodiscard]] std::uint64_t
stable_stress_configuration_hash(StressConfiguration value) noexcept;
[[nodiscard]] std::uint64_t stable_stress_result_hash(StressResult value) noexcept;
[[nodiscard]] std::uint64_t portfolio_snapshot_checksum(const void* value) noexcept;

static_assert(std::is_trivially_copyable_v<PortfolioEvent>);
static_assert(std::is_trivially_copyable_v<PortfolioSnapshot>);
static_assert((kPortfolioIdempotencyCapacity & (kPortfolioIdempotencyCapacity - 1U)) ==
              0U);

} // namespace aegis::risk::portfolio

#endif // AEGIS_RISK_PORTFOLIO_TYPES_HPP
