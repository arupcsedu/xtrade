#ifndef AEGIS_RISK_TYPES_HPP
#define AEGIS_RISK_TYPES_HPP

#include "aegis/common/identifiers.hpp"
#include "aegis/common/sha256.hpp"
#include "aegis/market_state/controller.hpp"
#include "aegis/time/clock_quality.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace aegis::risk {

inline constexpr std::size_t kMaximumRiskSymbols = 64U;
inline constexpr std::size_t kMaximumRiskStrategies = 32U;
inline constexpr std::size_t kMaximumRiskVenues = 16U;
inline constexpr std::size_t kMaximumRiskSectors = 16U;
inline constexpr std::size_t kMaximumRiskFactors = 8U;
inline constexpr std::size_t kDuplicateIntentCapacity = 4'096U;
inline constexpr std::size_t kMaximumRateEvents = 2'048U;
inline constexpr std::uint32_t kPartsPerMillion = 1'000'000U;
inline constexpr std::uint64_t kMaximumMonetaryNanos =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

enum class TradingMode : std::uint8_t {
  simulation = 1,
  paper = 2,
  live = 3,
};

enum class IntentAction : std::uint8_t {
  buy = 1,
  sell = 2,
  cancel = 3,
};

enum class DecisionCode : std::uint8_t {
  rejected = 1,
  approved = 2,
};

// Values 1-30 are the normative evaluation order from Prompt 20.
enum class RiskCheck : std::uint8_t {
  none = 0,
  trading_mode_authorization = 1,
  operator_session_authorization = 2,
  strategy_authorization = 3,
  symbol_authorization = 4,
  restricted_list = 5,
  market_state = 6,
  halt = 7,
  clock_health = 8,
  feed_book_health = 9,
  maximum_order_quantity = 10,
  maximum_order_notional = 11,
  price_collar = 12,
  tick_size = 13,
  duplicate_intent = 14,
  order_rate = 15,
  cancel_rate = 16,
  symbol_position = 17,
  gross_exposure = 18,
  net_exposure = 19,
  sector_concentration = 20,
  factor_exposure = 21,
  daily_loss = 22,
  strategy_loss = 23,
  drawdown = 24,
  credit_capital = 25,
  short_sale_locate = 26,
  self_trade_prevention = 27,
  venue_authorization = 28,
  kill_switch = 29,
  configuration_freshness = 30,
};

enum class RiskReason : std::uint8_t {
  within_limits = 1,
  invalid_intent = 2,
  expired_intent = 3,
  trading_mode_unauthorized = 4,
  operator_session_unauthorized = 5,
  strategy_unauthorized = 6,
  symbol_unauthorized = 7,
  restricted_instrument = 8,
  market_state_unsafe = 9,
  trading_halted = 10,
  clock_unhealthy = 11,
  feed_book_unhealthy = 12,
  order_quantity_exceeded = 13,
  order_notional_exceeded = 14,
  price_collar_exceeded = 15,
  invalid_tick_size = 16,
  duplicate_intent = 17,
  order_rate_exceeded = 18,
  cancel_rate_exceeded = 19,
  symbol_position_exceeded = 20,
  gross_exposure_exceeded = 21,
  net_exposure_exceeded = 22,
  sector_concentration_exceeded = 23,
  factor_exposure_exceeded = 24,
  daily_loss_exceeded = 25,
  strategy_loss_exceeded = 26,
  drawdown_exceeded = 27,
  credit_capital_exceeded = 28,
  short_sale_locate_denied = 29,
  self_trade_prevention_denied = 30,
  venue_unauthorized = 31,
  kill_switch_engaged = 32,
  configuration_stale = 33,
  stale_position = 34,
  risk_state_unavailable = 35,
  configuration_rollback = 36,
  split_brain_epoch = 37,
  arithmetic_overflow = 38,
  journal_unavailable = 39,
  engine_busy = 40,
};

enum class PolicyHookResult : std::uint8_t {
  not_required = 1,
  allowed = 2,
  denied = 3,
  unavailable = 4,
};

enum class KillSwitchScope : std::uint8_t {
  symbol = 1,
  strategy = 2,
  venue = 3,
  account = 4,
  firm = 5,
};

enum class EvaluationStatus : std::uint8_t {
  journaled = 1,
  journal_unavailable = 2,
  engine_busy = 3,
  not_initialized = 4,
};

enum class InstallStatus : std::uint8_t {
  installed = 1,
  unchanged = 2,
  invalid = 3,
  rollback_rejected = 4,
  revision_conflict = 5,
  busy = 6,
};

enum class StateUpdateStatus : std::uint8_t {
  applied = 1,
  invalid = 2,
  arithmetic_overflow = 3,
  unavailable = 4,
  busy = 5,
};

// Fixed-layout value contracts intentionally expose aggregate members.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct SymbolLimit {
  common::InstrumentId instrument_id;
  std::uint64_t tick_value_currency_nanos{};
  std::uint64_t price_increment_ticks{};
  std::uint64_t maximum_order_quantity_units{};
  std::uint64_t maximum_order_notional_currency_nanos{};
  std::uint64_t maximum_price_deviation_ticks{};
  std::uint64_t maximum_absolute_position_units{};
  std::uint16_t sector_index{};
  std::array<std::int32_t, kMaximumRiskFactors> factor_beta_ppm{};
  bool authorized{false};
  bool restricted{true};
};

struct StrategyLimit {
  common::StrategyId strategy_id;
  std::uint64_t maximum_loss_currency_nanos{};
  std::uint64_t maximum_drawdown_currency_nanos{};
  bool authorized{false};
};

struct VenueLimit {
  common::VenueId venue_id;
  bool authorized{false};
};

struct RiskLimitSnapshot {
  common::RiskSnapshotId risk_snapshot_id;
  common::ConfigurationVersion configuration_version;
  common::ConfigurationVersion policy_version;
  common::SessionId session_id;
  common::AccountId account_id;
  std::uint64_t revision{};
  std::uint64_t authority_epoch{};
  std::uint64_t published_process_monotonic_time_ns{};
  std::uint64_t valid_until_process_monotonic_time_ns{};
  std::uint64_t approval_ttl_ns{};
  std::uint64_t maximum_position_age_ns{};
  std::uint64_t maximum_market_state_age_ns{};
  std::uint64_t maximum_clock_state_age_ns{};
  std::uint64_t maximum_feed_book_age_ns{};
  std::uint64_t maximum_configuration_age_ns{};
  std::uint64_t order_rate_window_ns{};
  std::uint64_t cancel_rate_window_ns{};
  std::uint32_t maximum_orders_per_window{};
  std::uint32_t maximum_cancels_per_window{};
  std::uint16_t allowed_market_state_mask{};
  std::uint64_t maximum_gross_exposure_currency_nanos{};
  std::uint64_t maximum_absolute_net_exposure_currency_nanos{};
  std::uint64_t maximum_daily_loss_currency_nanos{};
  std::uint64_t maximum_drawdown_currency_nanos{};
  std::uint64_t credit_capital_limit_currency_nanos{};
  std::array<std::uint64_t, kMaximumRiskSectors>
      maximum_sector_exposure_currency_nanos{};
  std::array<std::uint64_t, kMaximumRiskFactors>
      maximum_factor_exposure_currency_nanos{};
  std::array<SymbolLimit, kMaximumRiskSymbols> symbols{};
  std::array<StrategyLimit, kMaximumRiskStrategies> strategies{};
  std::array<VenueLimit, kMaximumRiskVenues> venues{};
  std::uint32_t symbol_count{};
  std::uint32_t strategy_count{};
  std::uint32_t venue_count{};
  std::uint32_t sector_count{};
  std::uint32_t factor_count{};
  bool allow_simulation{true};
  bool allow_paper{false};
  bool require_locate_for_short_sale{true};
  bool require_self_trade_prevention{true};
  std::uint64_t stable_hash{};
};

struct RiskIntent {
  common::IntentId intent_id;
  common::SessionId session_id;
  common::AccountId account_id;
  common::StrategyId strategy_id;
  common::VenueId venue_id;
  common::InstrumentId instrument_id;
  common::ForecastId source_forecast_id;
  common::FeatureSnapshotId feature_snapshot_id;
  common::OrderId target_order_id;
  common::ConfigurationVersion configuration_version;
  IntentAction action{IntentAction::buy};
  std::int64_t limit_price_ticks{};
  std::uint64_t quantity_units{};
  std::uint64_t created_process_monotonic_time_ns{};
  std::uint64_t expire_process_monotonic_time_ns{};
  common::Sha256Digest canonical_sha256{};
  std::uint64_t stable_hash{};
};

struct RiskContext {
  std::uint64_t now_process_monotonic_time_ns{};
  TradingMode trading_mode{TradingMode::simulation};
  bool operator_authorized{false};
  bool session_authorized{false};
  std::uint64_t operator_authorization_valid_until_ns{};
  std::uint64_t authority_epoch{};
  std::uint64_t process_id{};
  market_state::MarketStateSnapshot market_state_snapshot;
  market_state::OfficialTradingStatus official_trading_status{
      market_state::OfficialTradingStatus::unknown};
  market_state::FeedHealth feed_health{market_state::FeedHealth::unknown};
  market_state::BookValidity book_validity{market_state::BookValidity::recovering};
  time::ClockQualitySnapshot clock_quality_snapshot;
  std::uint64_t feed_book_observed_process_monotonic_time_ns{};
  std::uint64_t feed_book_state_hash{};
  std::int64_t reference_price_ticks{};
  PolicyHookResult short_sale_locate{PolicyHookResult::unavailable};
  PolicyHookResult self_trade_prevention{PolicyHookResult::unavailable};
  std::uint64_t stable_hash{};
};

struct RiskDecision {
  common::GlobalEventId global_event_id;
  common::IntentId intent_id;
  common::SessionId session_id;
  common::AccountId account_id;
  common::StrategyId strategy_id;
  common::VenueId venue_id;
  common::InstrumentId instrument_id;
  common::RiskSnapshotId risk_snapshot_id;
  common::ConfigurationVersion configuration_version;
  DecisionCode decision{DecisionCode::rejected};
  RiskReason reason{RiskReason::risk_state_unavailable};
  RiskCheck failed_check{RiskCheck::none};
  IntentAction intent_action{IntentAction::buy};
  TradingMode trading_mode{TradingMode::simulation};
  std::uint64_t approved_quantity_units{};
  std::int64_t approved_price_ticks{};
  std::uint64_t evaluated_process_monotonic_time_ns{};
  std::uint64_t valid_until_process_monotonic_time_ns{};
  std::uint64_t intent_hash{};
  common::Sha256Digest intent_sha256{};
  std::uint64_t risk_snapshot_hash{};
  std::uint64_t risk_context_hash{};
  std::uint64_t position_generation{};
  std::uint64_t authority_epoch{};
  std::uint64_t policy_revision{};
  std::uint64_t evaluation_ordinal{};
  std::uint64_t journal_sequence{};
  std::uint64_t reason_mask{};
  std::uint32_t completed_check_mask{};
  std::uint64_t stable_hash{};
};

struct RiskEvaluationRequest {
  common::GlobalEventId global_event_id;
  RiskIntent intent;
  RiskContext context;
};

struct EvaluationResult {
  EvaluationStatus status{EvaluationStatus::not_initialized};
  RiskDecision decision;
};

struct PositionSeed {
  std::uint32_t symbol_index{};
  std::int64_t net_quantity_units{};
  std::int64_t mark_price_ticks{};
  std::uint64_t as_of_process_monotonic_time_ns{};
};

struct FillUpdate {
  std::uint32_t symbol_index{};
  std::uint32_t strategy_index{};
  IntentAction side{IntentAction::buy};
  std::uint64_t fill_quantity_units{};
  std::uint64_t release_reserved_quantity_units{};
  std::int64_t realized_pnl_delta_currency_nanos{};
  std::uint64_t as_of_process_monotonic_time_ns{};
};

struct ProfitLossUpdate {
  std::uint32_t strategy_index{};
  std::int64_t firm_daily_pnl_currency_nanos{};
  std::int64_t firm_peak_pnl_currency_nanos{};
  std::int64_t strategy_pnl_currency_nanos{};
  std::int64_t strategy_peak_pnl_currency_nanos{};
  std::uint64_t as_of_process_monotonic_time_ns{};
};

struct KillSwitchUpdate {
  KillSwitchScope scope{KillSwitchScope::firm};
  std::uint32_t target_index{};
  std::uint64_t authority_epoch{};
  std::uint64_t command_sequence{};
  bool engaged{true};
  bool operator_authorized{false};
};

struct RiskMetrics {
  std::uint64_t evaluations{};
  std::uint64_t approvals{};
  std::uint64_t rejections{};
  std::uint64_t busy_results{};
  std::uint64_t journal_full_results{};
  std::uint64_t position_generation{};
  std::uint64_t kill_generation{};
  std::uint64_t active_limit_revision{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] bool valid_trading_mode(TradingMode mode) noexcept;
[[nodiscard]] bool valid_intent_action(IntentAction action) noexcept;
[[nodiscard]] bool valid_policy_hook_result(PolicyHookResult result) noexcept;
[[nodiscard]] bool valid_limit_snapshot(const RiskLimitSnapshot& snapshot) noexcept;
[[nodiscard]] bool valid_risk_intent(const RiskIntent& intent) noexcept;
[[nodiscard]] bool valid_risk_context(const RiskContext& context) noexcept;
[[nodiscard]] bool valid_risk_decision(const RiskDecision& decision) noexcept;
[[nodiscard]] std::uint64_t
stable_limit_snapshot_hash(RiskLimitSnapshot snapshot) noexcept;
[[nodiscard]] std::uint64_t stable_risk_intent_hash(RiskIntent intent) noexcept;
[[nodiscard]] std::uint64_t stable_risk_context_hash(RiskContext context) noexcept;
[[nodiscard]] std::uint64_t stable_risk_decision_hash(RiskDecision decision) noexcept;
[[nodiscard]] std::uint64_t reason_bit(RiskReason reason) noexcept;
[[nodiscard]] std::uint32_t check_bit(RiskCheck check) noexcept;

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert((kDuplicateIntentCapacity & (kDuplicateIntentCapacity - 1U)) == 0U);
static_assert((kMaximumRateEvents & (kMaximumRateEvents - 1U)) == 0U);
static_assert(std::is_trivially_copyable_v<RiskIntent>);
static_assert(std::is_trivially_copyable_v<RiskDecision>);

} // namespace aegis::risk

#endif // AEGIS_RISK_TYPES_HPP
