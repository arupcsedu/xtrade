#ifndef AEGIS_OMS_TYPES_HPP
#define AEGIS_OMS_TYPES_HPP

#include "aegis/common/identifiers.hpp"
#include "aegis/risk/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace aegis::oms {

inline constexpr std::size_t kMaximumOmsOrders = 1'024U;
inline constexpr std::size_t kMaximumOmsExecutions = 4'096U;
inline constexpr std::size_t kOmsIdempotencyCapacity = 8'192U;
inline constexpr std::size_t kClientOrderIdBytes = 33U;

enum class OrderState : std::uint8_t {
  created = 1,
  ready = 2,
  pending_ack = 3,
  working = 4,
  partially_filled = 5,
  pending_cancel = 6,
  pending_replace = 7,
  filled = 8,
  canceled = 9,
  rejected = 10,
  expired = 11,
  unknown_recovery = 12,
};

enum class InputKind : std::uint8_t {
  accept_intent = 1,
  mark_ready = 2,
  dispatch = 3,
  cancel_intent = 4,
  replace_intent = 5,
  acknowledgement = 6,
  rejection = 7,
  fill = 8,
  cancel_acknowledgement = 9,
  cancel_rejection = 10,
  replace_acknowledgement = 11,
  replace_rejection = 12,
  expire = 13,
  recovery_begin = 14,
  recovery_observation = 15,
  authority_update = 16,
};

enum class EventSource : std::uint8_t {
  internal = 1,
  gateway = 2,
  drop_copy = 3,
  recovery = 4,
};

enum class RecoveryObservation : std::uint8_t {
  none = 0,
  working = 1,
  partially_filled = 2,
  filled = 3,
  canceled = 4,
  rejected = 5,
  expired = 6,
  absent_ambiguous = 7,
};

enum class GatewayCommandKind : std::uint8_t {
  none = 0,
  new_order = 1,
  cancel = 2,
  replace = 3,
};

enum class ApplyStatus : std::uint8_t {
  applied = 1,
  duplicate = 2,
  reconciled = 3,
  out_of_order_applied = 4,
  forbidden_transition = 5,
  invalid = 6,
  not_found = 7,
  capacity_exhausted = 8,
  journal_unavailable = 9,
  fenced = 10,
  inhibited = 11,
  busy = 12,
  stopped = 13,
};

enum class OmsReason : std::uint8_t {
  none = 0,
  accepted = 1,
  exact_risk_approval_required = 2,
  approval_expired = 3,
  scope_mismatch = 4,
  duplicate_receipt = 5,
  receipt_conflict = 6,
  duplicate_intent = 7,
  order_not_found = 8,
  forbidden_transition = 9,
  stale_fencing_token = 10,
  exchange_epoch_mismatch = 11,
  external_id_conflict = 12,
  execution_conflict = 13,
  invalid_quantity = 14,
  invalid_price = 15,
  arithmetic_overflow = 16,
  order_capacity = 17,
  execution_capacity = 18,
  idempotency_capacity = 19,
  journal_capacity = 20,
  recovery_required = 21,
  ambiguous_absence = 22,
  snapshot_unavailable = 23,
  configuration_invalid = 24,
  shutdown = 25,
};

enum class OmsHealth : std::uint8_t {
  starting = 1,
  healthy = 2,
  recovering = 3,
  unsafe = 4,
  stopped = 5,
};

enum class OmsInvariant : std::uint8_t {
  none = 0,
  configuration_invalid = 1,
  receipt_conflict = 2,
  external_mapping_conflict = 3,
  execution_conflict = 4,
  quantity_mismatch = 5,
  arithmetic_overflow = 6,
  split_brain = 7,
  journal_exhausted = 8,
  snapshot_publication_failed = 9,
  replay_diverged = 10,
};

enum class PersistenceStatus : std::uint8_t {
  completed = 1,
  io_error = 2,
  invalid_header = 3,
  incompatible_abi = 4,
  configuration_mismatch = 5,
  corrupt_record = 6,
  capacity_exhausted = 7,
  trailing_data = 8,
  target_not_empty = 9,
};

enum class RecoveryStatus : std::uint8_t {
  recovered_ready = 1,
  recovered_inhibited = 2,
  source_corrupt = 3,
  configuration_mismatch = 4,
  replay_diverged = 5,
  target_not_empty = 6,
  target_journal_full = 7,
  busy = 8,
};

// Provider-neutral external identities. No exchange encoding or semantics are
// implied by this repository-owned 128-bit normalized value.
struct ExternalOrderId {
  std::uint64_t high{};
  std::uint64_t low{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return high != 0U || low != 0U;
  }

  auto operator<=>(const ExternalOrderId&) const = default;
};

struct LeaderAuthority {
  std::uint64_t exchange_session_epoch{};
  std::uint64_t fencing_token{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return exchange_session_epoch != 0U && fencing_token != 0U;
  }
};

// Fixed-layout hot-path contracts intentionally expose aggregate members.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct OmsConfiguration {
  common::SessionId session_id{};
  common::AccountId account_id{};
  common::ConfigurationVersion configuration_version{};
  risk::TradingMode trading_mode{risk::TradingMode::simulation};
  std::uint64_t exchange_session_epoch{};
  std::uint64_t initial_fencing_token{};
  std::uint64_t maximum_order_age_ns{};
  bool require_drop_copy{true};
  std::uint64_t stable_hash{};
};

struct OmsInput {
  common::GlobalEventId receipt_id{};
  InputKind kind{InputKind::accept_intent};
  EventSource source{EventSource::internal};
  common::OrderId order_id{};
  ExternalOrderId external_order_id{};
  common::GlobalEventId execution_id{};
  risk::RiskIntent intent{};
  risk::RiskDecision risk_decision{};
  LeaderAuthority authority{};
  RecoveryObservation recovery_observation{RecoveryObservation::none};
  std::int64_t price_ticks{};
  std::uint64_t quantity_units{};
  std::uint64_t venue_sequence{};
  std::int64_t exchange_event_time_ns{};
  std::int64_t nic_receive_time_ns{};
  std::uint64_t process_monotonic_time_ns{};
  std::uint64_t stable_hash{};
};

struct GatewayCommand {
  common::GlobalEventId command_id{};
  GatewayCommandKind kind{GatewayCommandKind::none};
  common::OrderId order_id{};
  ExternalOrderId external_order_id{};
  std::array<char, kClientOrderIdBytes> client_order_id{};
  common::SessionId session_id{};
  common::AccountId account_id{};
  common::StrategyId strategy_id{};
  common::VenueId venue_id{};
  common::InstrumentId instrument_id{};
  common::ConfigurationVersion configuration_version{};
  risk::IntentAction side{risk::IntentAction::buy};
  std::int64_t price_ticks{};
  std::uint64_t quantity_units{};
  std::uint64_t cumulative_fill_quantity_units{};
  LeaderAuthority authority{};
  std::uint64_t intent_hash{};
  std::uint64_t risk_decision_hash{};
  std::uint64_t generated_process_monotonic_time_ns{};
  std::uint64_t stable_hash{};
};

struct OmsOrderSnapshot {
  common::OrderId order_id{};
  common::IntentId intent_id{};
  common::SessionId session_id{};
  common::AccountId account_id{};
  common::StrategyId strategy_id{};
  common::VenueId venue_id{};
  common::InstrumentId instrument_id{};
  common::ConfigurationVersion configuration_version{};
  ExternalOrderId external_order_id{};
  std::array<char, kClientOrderIdBytes> client_order_id{};
  LeaderAuthority authority{};
  OrderState state{OrderState::created};
  OrderState state_before_recovery{OrderState::created};
  risk::IntentAction side{risk::IntentAction::buy};
  std::int64_t price_ticks{};
  std::uint64_t quantity_units{};
  std::uint64_t cumulative_fill_quantity_units{};
  std::uint64_t remaining_quantity_units{};
  std::int64_t pending_replace_price_ticks{};
  std::uint64_t pending_replace_quantity_units{};
  std::uint64_t state_version{};
  std::uint64_t last_venue_sequence{};
  std::uint64_t last_update_process_monotonic_time_ns{};
  std::uint64_t intent_hash{};
  std::uint64_t risk_decision_hash{};
  std::uint8_t fill_source_mask{};
  bool occupied{false};
};

struct OmsSnapshot {
  common::SessionId session_id{};
  common::AccountId account_id{};
  common::ConfigurationVersion configuration_version{};
  std::uint64_t configuration_hash{};
  std::uint64_t exchange_session_epoch{};
  std::uint64_t active_fencing_token{};
  std::uint64_t snapshot_sequence{};
  std::uint64_t source_journal_sequence{};
  std::uint64_t source_journal_hash{};
  std::uint64_t as_of_process_monotonic_time_ns{};
  std::array<OmsOrderSnapshot, kMaximumOmsOrders> orders{};
  std::uint32_t order_count{};
  std::uint32_t live_order_count{};
  std::uint32_t unknown_recovery_count{};
  std::uint32_t unreconciled_fill_count{};
  OmsHealth health{OmsHealth::starting};
  OmsInvariant invariant{OmsInvariant::none};
  bool ready{false};
  std::uint64_t stable_hash{};
};

struct ApplyResult {
  ApplyStatus status{ApplyStatus::invalid};
  OmsReason reason{OmsReason::none};
  common::OrderId order_id{};
  OrderState previous_state{OrderState::created};
  OrderState current_state{OrderState::created};
  std::uint64_t state_version{};
  std::uint64_t journal_sequence{};
  // Populated after commit. It is intentionally excluded from the logical
  // result hash to avoid a circular record-hash dependency.
  std::uint64_t journal_record_hash{};
  std::uint64_t snapshot_sequence{};
  std::uint64_t snapshot_hash{};
  GatewayCommand gateway_command{};
  std::uint64_t stable_hash{};
};

struct OmsMetrics {
  std::uint64_t inputs{};
  std::uint64_t applied{};
  std::uint64_t duplicates{};
  std::uint64_t forbidden_transitions{};
  std::uint64_t out_of_order_events{};
  std::uint64_t reconciliations{};
  std::uint64_t emitted_commands{};
  std::uint64_t fenced_commands{};
  std::uint64_t journal_full_results{};
  std::uint64_t snapshot_publications{};
  std::uint64_t snapshot_publish_failures{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] bool valid_order_state(OrderState value) noexcept;
[[nodiscard]] bool terminal_state(OrderState value) noexcept;
[[nodiscard]] bool live_state(OrderState value) noexcept;
[[nodiscard]] bool valid_configuration(const OmsConfiguration& value) noexcept;
[[nodiscard]] bool valid_input(const OmsInput& value) noexcept;
[[nodiscard]] bool valid_gateway_command(const GatewayCommand& value) noexcept;
[[nodiscard]] bool valid_order_snapshot(const OmsOrderSnapshot& value) noexcept;
[[nodiscard]] bool valid_snapshot(const OmsSnapshot& value) noexcept;
[[nodiscard]] bool transition_allowed(OrderState from, InputKind input,
                                      OrderState to) noexcept;
[[nodiscard]] bool exact_risk_approval(const risk::RiskIntent& intent,
                                       const risk::RiskDecision& decision,
                                       std::uint64_t now_ns) noexcept;
[[nodiscard]] common::OrderId
deterministic_order_id(const OmsConfiguration& configuration,
                       const risk::RiskIntent& intent) noexcept;
[[nodiscard]] std::array<char, kClientOrderIdBytes>
deterministic_client_order_id(common::OrderId order_id) noexcept;
[[nodiscard]] std::uint64_t stable_configuration_hash(OmsConfiguration value) noexcept;
[[nodiscard]] std::uint64_t stable_input_hash(OmsInput value) noexcept;
[[nodiscard]] std::uint64_t stable_gateway_command_hash(GatewayCommand value) noexcept;
[[nodiscard]] std::uint64_t stable_order_snapshot_hash(OmsOrderSnapshot value) noexcept;
[[nodiscard]] std::uint64_t stable_snapshot_hash(const OmsSnapshot& value) noexcept;
[[nodiscard]] std::uint64_t stable_apply_result_hash(ApplyResult value) noexcept;

static_assert(std::is_trivially_copyable_v<OmsInput>);
static_assert(std::is_trivially_copyable_v<GatewayCommand>);
static_assert(std::is_trivially_copyable_v<OmsSnapshot>);

} // namespace aegis::oms

#endif // AEGIS_OMS_TYPES_HPP
