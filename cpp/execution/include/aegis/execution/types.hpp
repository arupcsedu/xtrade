#ifndef AEGIS_EXECUTION_TYPES_HPP
#define AEGIS_EXECUTION_TYPES_HPP

#include "aegis/common/identifiers.hpp"
#include "aegis/market_data/synthetic/event.hpp"
#include "aegis/market_state/controller.hpp"
#include "aegis/oms/types.hpp"
#include "aegis/risk/types.hpp"
#include "aegis/time/clock_quality.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#ifndef AEGIS_LIVE_TRADING_COMPILED
#define AEGIS_LIVE_TRADING_COMPILED 0
#endif

namespace aegis::execution {

inline constexpr bool kLiveTradingCompiled = AEGIS_LIVE_TRADING_COMPILED == 1;
inline constexpr std::size_t kMaximumGatewayOrders = 1'024U;
inline constexpr std::size_t kMaximumGatewayEvents = 4'096U;
inline constexpr std::size_t kMaximumGatewayCommands = 4'096U;
inline constexpr std::size_t kMaximumGatewayJournalRecords = 16'384U;
inline constexpr std::size_t kMaximumGatewayInstruments = 64U;
inline constexpr std::size_t kMaximumProtocolFrameBytes = 2'048U;
inline constexpr std::uint32_t kPartsPerMillion = 1'000'000U;

enum class GatewayMode : std::uint8_t {
  simulation = 1,
  paper = 2,
  halted = 3,
#if AEGIS_LIVE_TRADING_COMPILED
  live_armed = 4,
  live_active = 5,
#endif
};

enum class GatewayHealth : std::uint8_t {
  starting = 1,
  healthy = 2,
  recovering = 3,
  degraded = 4,
  unsafe = 5,
  stopped = 6,
};

enum class SessionState : std::uint8_t {
  stopped = 1,
  starting = 2,
  logging_on = 3,
  active = 4,
  recovering = 5,
  logging_off = 6,
  halted = 7,
};

enum class SessionReason : std::uint8_t {
  none = 0,
  operator_start = 1,
  logon_accepted = 2,
  operator_logoff = 3,
  logoff_complete = 4,
  heartbeat_timeout = 5,
  inbound_sequence_gap = 6,
  recovery_requested = 7,
  recovery_complete = 8,
  recovery_ambiguous = 9,
  split_brain = 10,
  journal_unavailable = 11,
  shutdown = 12,
};

enum class SequenceStatus : std::uint8_t {
  first = 1,
  in_order = 2,
  duplicate = 3,
  gap = 4,
  out_of_order = 5,
  exhausted = 6,
  invalid = 7,
};

enum class GatewaySubmitStatus : std::uint8_t {
  accepted = 1,
  duplicate = 2,
  rejected = 3,
  capacity_exhausted = 4,
  busy = 5,
  stopped = 6,
};

enum class GatewayPollStatus : std::uint8_t {
  event = 1,
  empty = 2,
  recovering = 3,
  sequence_error = 4,
  stopped = 5,
};

enum class GatewayReason : std::uint8_t {
  none = 0,
  accepted = 1,
  invalid_configuration = 2,
  invalid_request = 3,
  invalid_command = 4,
  command_kind_mismatch = 5,
  duplicate_command = 6,
  command_identity_conflict = 7,
  session_not_active = 8,
  mode_mismatch = 9,
  scope_mismatch = 10,
  stale_fencing_token = 11,
  risk_decision_invalid = 12,
  risk_decision_rejected = 13,
  risk_decision_expired = 14,
  risk_hash_mismatch = 15,
  market_state_unsafe = 16,
  clock_unhealthy = 17,
  feed_unhealthy = 18,
  book_invalid = 19,
  trading_halted = 20,
  kill_switch_engaged = 21,
  signed_configuration_required = 22,
  operator_authorization_required = 23,
  activation_record_required = 24,
  rate_limited = 25,
  order_capacity = 26,
  event_capacity = 27,
  journal_capacity = 28,
  order_not_found = 29,
  paper_policy_reject = 30,
  sequence_gap = 31,
  heartbeat_timeout = 32,
  recovery_required = 33,
  recovery_ambiguous = 34,
  decode_failure = 35,
  unsupported_protocol = 36,
  arithmetic_overflow = 37,
  shutdown = 38,
};

enum class GatewayResponseKind : std::uint8_t {
  acknowledgement = 1,
  rejection = 2,
  fill = 3,
  cancel_acknowledgement = 4,
  cancel_rejection = 5,
  replace_acknowledgement = 6,
  replace_rejection = 7,
  heartbeat = 8,
  session_status = 9,
};

enum class ProtocolBoundaryKind : std::uint8_t {
  synthetic = 1,
  native_protocol = 2,
  fix_compatible = 3,
};

enum class AdapterStatus : std::uint8_t {
  encoded = 1,
  decoded = 2,
  malformed = 3,
  unsupported = 4,
  output_too_small = 5,
  authorization_invalid = 6,
};

enum class GatewayAuditKind : std::uint8_t {
  session_transition = 1,
  command_decision = 2,
  response_scheduled = 3,
  response_emitted = 4,
  market_observation = 5,
  recovery = 6,
  shutdown = 7,
};

enum class PaperOrderState : std::uint8_t {
  pending_ack = 1,
  working = 2,
  pending_cancel = 3,
  pending_replace = 4,
  filled = 5,
  canceled = 6,
  rejected = 7,
  unknown_recovery = 8,
};

enum class RecoveryStatus : std::uint8_t {
  completed = 1,
  ambiguous = 2,
  invalid_snapshot = 3,
  configuration_mismatch = 4,
  busy = 5,
};

// Fixed-layout values intentionally expose aggregate fields for bounded hot-path use.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct SyntheticInstrumentMapping {
  common::VenueId venue_id;
  common::InstrumentId instrument_id;
  std::uint32_t synthetic_venue_number{};
  std::uint32_t synthetic_instrument_number{};
};

struct PaperModelConfiguration {
  std::uint64_t deterministic_seed{20'260'831U};
  std::uint64_t acknowledgement_latency_ns{50'000U};
  std::uint64_t cancel_latency_ns{75'000U};
  std::uint64_t replace_latency_ns{75'000U};
  std::uint64_t initial_queue_ahead_units{10U};
  std::uint64_t maximum_fill_chunk_units{100U};
  std::uint64_t fee_per_unit_currency_nanos{1'000U};
  std::uint64_t slippage_ticks{1U};
  std::uint64_t impact_ticks_per_million_units{1U};
  std::uint32_t reject_every_nth_new_order{};
  bool allow_auction_orders{true};
};

struct GatewayConfiguration {
  common::SessionId session_id;
  common::AccountId account_id;
  common::VenueId venue_id;
  common::ConfigurationVersion configuration_version;
  GatewayMode startup_mode{GatewayMode::simulation};
  std::uint64_t exchange_session_epoch{};
  std::uint64_t initial_fencing_token{};
  std::uint64_t heartbeat_interval_ns{100'000'000U};
  std::uint64_t heartbeat_timeout_ns{500'000'000U};
  std::uint64_t maximum_clock_age_ns{100'000'000U};
  std::uint64_t command_rate_window_ns{1'000'000'000U};
  std::uint32_t maximum_new_orders_per_window{1'000U};
  std::uint32_t maximum_cancels_per_window{2'000U};
  std::uint32_t maximum_replaces_per_window{1'000U};
  std::array<SyntheticInstrumentMapping, kMaximumGatewayInstruments> mappings{};
  std::uint32_t mapping_count{};
  PaperModelConfiguration paper_model;
  std::uint64_t stable_hash{};
};

struct FinalSafetyState {
  std::uint64_t observed_process_monotonic_time_ns{};
  market_state::MarketStateSnapshot market_state_snapshot;
  market_state::OfficialTradingStatus official_trading_status{
      market_state::OfficialTradingStatus::unknown};
  market_state::FeedHealth feed_health{market_state::FeedHealth::unknown};
  market_state::BookValidity book_validity{market_state::BookValidity::recovering};
  time::ClockQualitySnapshot clock_quality_snapshot;
  oms::LeaderAuthority authority;
  std::uint64_t effective_configuration_hash{};
  std::uint64_t activation_record_hash{};
  std::uint64_t operator_authorization_valid_until_ns{};
  bool signed_configuration_valid{false};
  bool operator_authorized{false};
  bool activation_record_durable{false};
  bool kill_switch_engaged{true};
  bool journal_ready{false};
  std::uint64_t stable_hash{};
};

struct GatewayRequest {
  oms::GatewayCommand command;
  risk::RiskDecision risk_decision;
  FinalSafetyState safety;
  std::uint64_t stable_hash{};
};

struct GatewaySubmitResult {
  GatewaySubmitStatus status{GatewaySubmitStatus::rejected};
  GatewayReason reason{GatewayReason::invalid_request};
  std::uint64_t outbound_sequence{};
  std::uint64_t journal_sequence{};
  std::uint64_t journal_record_hash{};
  std::uint64_t stable_hash{};
};

struct GatewayEvent {
  common::GlobalEventId event_id;
  common::GlobalEventId source_command_id;
  common::GlobalEventId execution_id;
  common::OrderId order_id;
  oms::ExternalOrderId external_order_id;
  common::SessionId session_id;
  common::AccountId account_id;
  common::VenueId venue_id;
  common::InstrumentId instrument_id;
  common::ConfigurationVersion configuration_version;
  oms::LeaderAuthority authority;
  GatewayMode mode{GatewayMode::simulation};
  GatewayResponseKind kind{GatewayResponseKind::rejection};
  GatewayReason reason{GatewayReason::none};
  std::uint64_t venue_sequence{};
  std::int64_t exchange_event_time_ns{};
  std::int64_t nic_receive_time_ns{};
  std::uint64_t process_monotonic_time_ns{};
  std::int64_t price_ticks{};
  std::uint64_t quantity_units{};
  std::uint64_t cumulative_fill_quantity_units{};
  std::uint64_t remaining_quantity_units{};
  std::uint64_t fee_currency_nanos{};
  std::uint64_t queue_ahead_before_units{};
  std::uint64_t queue_ahead_after_units{};
  std::uint64_t modeled_slippage_ticks{};
  std::uint64_t modeled_impact_ticks{};
  std::uint64_t source_market_event_hash{};
  std::uint64_t stable_hash{};
};

struct SessionSnapshot {
  SessionState state{SessionState::stopped};
  SessionReason reason{SessionReason::none};
  std::uint64_t exchange_session_epoch{};
  std::uint64_t fencing_token{};
  std::uint64_t next_outbound_sequence{1U};
  std::uint64_t expected_inbound_sequence{1U};
  std::uint64_t last_inbound_process_monotonic_time_ns{};
  std::uint64_t last_outbound_process_monotonic_time_ns{};
  std::uint64_t transition_count{};
  std::uint64_t stable_hash{};
};

struct PaperOrderSnapshot {
  common::GlobalEventId source_command_id;
  common::OrderId order_id;
  oms::ExternalOrderId external_order_id;
  common::InstrumentId instrument_id;
  risk::IntentAction side{risk::IntentAction::buy};
  PaperOrderState state{PaperOrderState::pending_ack};
  std::int64_t price_ticks{};
  std::uint64_t quantity_units{};
  std::uint64_t cumulative_fill_quantity_units{};
  std::uint64_t queue_ahead_units{};
  std::int64_t pending_replace_price_ticks{};
  std::uint64_t pending_replace_quantity_units{};
  std::uint64_t acknowledgement_due_ns{};
  std::uint64_t cancel_due_ns{};
  std::uint64_t replace_due_ns{};
  bool occupied{false};
};

struct GatewayRecoverySnapshot {
  common::SessionId session_id;
  common::AccountId account_id;
  common::VenueId venue_id;
  common::ConfigurationVersion configuration_version;
  std::uint64_t configuration_hash{};
  std::uint64_t exchange_session_epoch{};
  std::uint64_t next_inbound_sequence{};
  std::array<PaperOrderSnapshot, kMaximumGatewayOrders> orders{};
  std::uint32_t order_count{};
  std::uint64_t stable_hash{};
};

struct GatewayMetrics {
  GatewayMode mode{GatewayMode::simulation};
  GatewayHealth health{GatewayHealth::starting};
  SessionState session_state{SessionState::stopped};
  std::uint64_t commands_received{};
  std::uint64_t commands_accepted{};
  std::uint64_t commands_rejected{};
  std::uint64_t duplicate_commands{};
  std::uint64_t new_orders{};
  std::uint64_t cancels{};
  std::uint64_t replaces{};
  std::uint64_t acknowledgements{};
  std::uint64_t rejects{};
  std::uint64_t fills{};
  std::uint64_t partial_fills{};
  std::uint64_t heartbeats{};
  std::uint64_t rate_limit_rejections{};
  std::uint64_t sequence_gaps{};
  std::uint64_t recovery_count{};
  std::uint64_t journal_full_count{};
  std::uint64_t event_queue_high_watermark{};
};

struct GatewayAuditRecord {
  std::uint64_t sequence{};
  GatewayAuditKind kind{GatewayAuditKind::command_decision};
  GatewayMode mode{GatewayMode::simulation};
  SessionState session_state{SessionState::stopped};
  GatewayReason reason{GatewayReason::none};
  common::GlobalEventId correlation_id;
  common::OrderId order_id;
  std::uint64_t command_hash{};
  std::uint64_t risk_decision_hash{};
  std::uint64_t safety_state_hash{};
  std::uint64_t gateway_event_hash{};
  std::uint64_t process_monotonic_time_ns{};
  std::uint64_t previous_record_hash{};
  std::uint64_t stable_hash{};
};

struct OpaqueProtocolFrame {
  ProtocolBoundaryKind boundary{ProtocolBoundaryKind::native_protocol};
  std::uint16_t byte_count{};
  std::array<std::uint8_t, kMaximumProtocolFrameBytes> bytes{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] bool valid_gateway_mode(GatewayMode mode) noexcept;
[[nodiscard]] bool
valid_gateway_configuration(const GatewayConfiguration& value) noexcept;
[[nodiscard]] bool valid_final_safety_state(const FinalSafetyState& value) noexcept;
[[nodiscard]] bool valid_gateway_request(const GatewayRequest& value) noexcept;
[[nodiscard]] bool valid_gateway_event(const GatewayEvent& value) noexcept;
[[nodiscard]] bool
valid_recovery_snapshot(const GatewayRecoverySnapshot& value) noexcept;
[[nodiscard]] std::uint64_t
stable_gateway_configuration_hash(const GatewayConfiguration& value) noexcept;
[[nodiscard]] std::uint64_t
stable_final_safety_state_hash(const FinalSafetyState& value) noexcept;
[[nodiscard]] std::uint64_t
stable_gateway_request_hash(const GatewayRequest& value) noexcept;
[[nodiscard]] std::uint64_t
stable_gateway_submit_result_hash(const GatewaySubmitResult& value) noexcept;
[[nodiscard]] std::uint64_t
stable_gateway_event_hash(const GatewayEvent& value) noexcept;
[[nodiscard]] std::uint64_t
stable_session_snapshot_hash(const SessionSnapshot& value) noexcept;
[[nodiscard]] std::uint64_t
stable_recovery_snapshot_hash(const GatewayRecoverySnapshot& value) noexcept;
[[nodiscard]] std::uint64_t
stable_gateway_audit_record_hash(const GatewayAuditRecord& value) noexcept;
[[nodiscard]] std::string_view gateway_mode_name(GatewayMode mode) noexcept;
[[nodiscard]] std::string_view gateway_reason_name(GatewayReason reason) noexcept;

static_assert(std::is_trivially_copyable_v<GatewayRequest>);
static_assert(std::is_trivially_copyable_v<GatewayEvent>);
static_assert(std::is_trivially_copyable_v<GatewayAuditRecord>);

} // namespace aegis::execution

#endif // AEGIS_EXECUTION_TYPES_HPP
