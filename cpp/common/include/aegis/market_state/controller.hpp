#ifndef AEGIS_MARKET_STATE_CONTROLLER_HPP
#define AEGIS_MARKET_STATE_CONTROLLER_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace aegis::market_state {

inline constexpr std::uint32_t kPartsPerMillion = 1'000'000U;
inline constexpr std::uint16_t kMarketStateSchemaMajor = 1U;
inline constexpr std::uint16_t kMarketStateSchemaMinor = 0U;
inline constexpr std::size_t kMarketStateCount = 11U;

enum class MarketState : std::uint8_t {
  startup = 0,
  normal,
  scheduled_event,
  breaking_news,
  event_price_discovery,
  volatility_spike,
  data_degraded,
  halted,
  reopening,
  recovery,
  shutdown,
};

enum class OfficialTradingStatus : std::uint8_t {
  unknown = 0,
  pre_open,
  open,
  halted,
  auction,
  closed,
};

enum class FeedHealth : std::uint8_t {
  unknown = 0,
  recovering,
  healthy,
  degraded,
  stale,
  invalid,
};

enum class BookValidity : std::uint8_t {
  recovering = 0,
  valid,
  invalid,
};

enum class ClockQuality : std::uint8_t {
  unknown = 0,
  syncing,
  healthy,
  degraded,
  unsafe,
};

enum class NewsEventState : std::uint8_t {
  none = 0,
  breaking,
  price_discovery,
  resolved,
};

enum class CalendarEventState : std::uint8_t {
  none = 0,
  scheduled,
  release_active,
  complete,
};

enum class MarketStateReason : std::uint8_t {
  startup_initialized = 0,
  operator_shutdown,
  official_halt,
  official_market_closed,
  official_status_unknown,
  clock_unknown,
  clock_unsafe,
  clock_degraded,
  feed_unknown,
  feed_degraded,
  feed_stale,
  feed_invalid,
  book_invalid,
  kill_switch_engaged,
  reopening_pre_open,
  reopening_auction,
  clock_syncing,
  feed_recovering,
  book_recovering,
  operator_recovery,
  safety_recovery_required,
  event_cooldown,
  news_breaking,
  event_price_discovery,
  earnings_scheduled,
  macro_scheduled,
  earnings_and_macro_scheduled,
  volatility_threshold,
  spread_threshold,
  depth_threshold,
  model_ood_threshold,
  model_disagreement_threshold,
  normal_conditions_stable,
  malformed_input,
  input_ordering_invalid,
  scheduled_release_overdue,
  reopening_stabilizing,
  recovery_stabilizing,
  minimum_dwell_active,
  forbidden_transition_prevented,
};

enum class JournalAppendStatus : std::uint8_t {
  accepted = 0,
  full,
  stopped,
};

enum class PublishStatus : std::uint8_t {
  published = 0,
  busy,
  invalid_snapshot,
};

enum class ReadStatus : std::uint8_t {
  snapshot = 0,
  empty,
  invalidated,
  busy,
  corrupt,
};

enum class EvaluationStatus : std::uint8_t {
  transitioned = 0,
  unchanged,
  held_for_dwell,
  journal_rejected,
  not_initialized,
  shutdown_terminal,
};

// Fixed-layout values form the allocation-free edge-core contract.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct CalendarEventInput {
  CalendarEventState state{CalendarEventState::none};
  std::uint64_t release_wall_clock_utc_ns{};
};

struct OperatorControls {
  bool kill_switch_engaged{false};
  bool recovery_requested{false};
  bool shutdown_requested{false};
};

struct MarketStateInput {
  std::uint64_t input_sequence{};
  std::uint64_t process_monotonic_time_ns{};
  std::uint64_t wall_clock_utc_ns{};
  OfficialTradingStatus official_trading_status{OfficialTradingStatus::unknown};
  FeedHealth feed_health{FeedHealth::unknown};
  BookValidity book_validity{BookValidity::recovering};
  ClockQuality clock_quality{ClockQuality::unknown};
  NewsEventState news_event_state{NewsEventState::none};
  CalendarEventInput earnings_calendar;
  CalendarEventInput macro_calendar;
  std::uint32_t realized_volatility_ppm{};
  std::uint64_t spread_ticks{};
  std::uint64_t aggregate_depth_units{};
  std::uint32_t model_ood_ppm{};
  std::uint32_t model_disagreement_ppm{};
  OperatorControls operator_controls;
};

struct MarketStateConfig {
  std::uint64_t minimum_dwell_ns{};
  std::uint64_t recovery_stabilization_ns{};
  std::uint64_t reopening_stabilization_ns{};
  std::uint64_t scheduled_event_lead_ns{};
  std::uint32_t volatility_spike_threshold_ppm{};
  std::uint64_t spread_spike_threshold_ticks{};
  std::uint64_t minimum_aggregate_depth_units{};
  std::uint32_t model_ood_threshold_ppm{};
  std::uint32_t model_disagreement_threshold_ppm{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return minimum_dwell_ns != 0U && recovery_stabilization_ns != 0U &&
           reopening_stabilization_ns != 0U && scheduled_event_lead_ns != 0U &&
           volatility_spike_threshold_ppm != 0U &&
           volatility_spike_threshold_ppm <= kPartsPerMillion &&
           spread_spike_threshold_ticks != 0U && minimum_aggregate_depth_units != 0U &&
           model_ood_threshold_ppm != 0U &&
           model_ood_threshold_ppm <= kPartsPerMillion &&
           model_disagreement_threshold_ppm != 0U &&
           model_disagreement_threshold_ppm <= kPartsPerMillion;
  }
};

struct MarketStateSnapshot {
  std::uint16_t schema_major{kMarketStateSchemaMajor};
  std::uint16_t schema_minor{kMarketStateSchemaMinor};
  MarketState state{MarketState::startup};
  MarketStateReason primary_reason{MarketStateReason::startup_initialized};
  std::uint64_t reason_mask{};
  std::uint64_t input_sequence{};
  std::uint64_t transition_sequence{};
  std::uint64_t observed_process_monotonic_time_ns{};
  std::uint64_t observed_wall_clock_utc_ns{};
  std::uint64_t state_entered_process_monotonic_time_ns{};
  std::uint64_t stable_hash{};

  [[nodiscard]] bool valid() const noexcept;
};

struct MarketStateTransition {
  std::uint16_t schema_major{kMarketStateSchemaMajor};
  std::uint16_t schema_minor{kMarketStateSchemaMinor};
  MarketState from_state{MarketState::startup};
  MarketState to_state{MarketState::startup};
  MarketStateReason primary_reason{MarketStateReason::startup_initialized};
  std::uint64_t reason_mask{};
  std::uint64_t input_sequence{};
  std::uint64_t transition_sequence{};
  std::uint64_t observed_process_monotonic_time_ns{};
  std::uint64_t observed_wall_clock_utc_ns{};
  std::uint64_t previous_snapshot_hash{};
  std::uint64_t next_snapshot_hash{};
  std::uint64_t configuration_hash{};
  std::uint64_t record_hash{};

  [[nodiscard]] bool valid() const noexcept;
};

struct PublisherMetrics {
  std::uint64_t publish_count{};
  std::uint64_t read_count{};
  std::uint64_t busy_count{};
  std::uint64_t corrupt_count{};
  std::uint64_t invalidation_count{};
};

struct ControllerMetrics {
  std::uint64_t evaluation_count{};
  std::uint64_t transition_count{};
  std::uint64_t unchanged_count{};
  std::uint64_t dwell_hold_count{};
  std::uint64_t journal_rejection_count{};
  std::uint64_t invalid_input_count{};
};

struct EvaluationResult {
  EvaluationStatus status{EvaluationStatus::not_initialized};
  MarketStateSnapshot snapshot;
  bool transitioned{false};
  bool new_orders_permitted{false};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

using TransitionJournalFunction =
    JournalAppendStatus (*)(void*, const MarketStateTransition&) noexcept;

class TransitionJournalSink final {
public:
  constexpr TransitionJournalSink() noexcept = default;
  constexpr TransitionJournalSink(void* context,
                                  TransitionJournalFunction function) noexcept
      : context_(context), function_(function) {}

  [[nodiscard]] constexpr bool valid() const noexcept { return function_ != nullptr; }

  [[nodiscard]] JournalAppendStatus
  append(const MarketStateTransition& transition) const noexcept {
    return valid() ? function_(context_, transition) : JournalAppendStatus::stopped;
  }

private:
  void* context_{};
  TransitionJournalFunction function_{};
};

class AtomicMarketStatePublisher final {
public:
  AtomicMarketStatePublisher() noexcept = default;
  AtomicMarketStatePublisher(const AtomicMarketStatePublisher&) = delete;
  AtomicMarketStatePublisher& operator=(const AtomicMarketStatePublisher&) = delete;

  [[nodiscard]] PublishStatus publish(const MarketStateSnapshot& snapshot) noexcept;
  void invalidate() noexcept;
  [[nodiscard]] ReadStatus read(MarketStateSnapshot& output) const noexcept;
  [[nodiscard]] PublisherMetrics metrics() const noexcept;

private:
  static constexpr std::size_t kMaximumReadRetries = 8U;
  std::atomic<std::uint64_t> publication_sequence_{0U};
  std::atomic<std::uint64_t> schema_word_{0U};
  std::atomic<std::uint64_t> state_word_{0U};
  std::atomic<std::uint64_t> reason_mask_{0U};
  std::atomic<std::uint64_t> input_sequence_{0U};
  std::atomic<std::uint64_t> transition_sequence_{0U};
  std::atomic<std::uint64_t> observed_process_monotonic_time_ns_{0U};
  std::atomic<std::uint64_t> observed_wall_clock_utc_ns_{0U};
  std::atomic<std::uint64_t> state_entered_process_monotonic_time_ns_{0U};
  std::atomic<std::uint64_t> stable_hash_{0U};
  std::atomic<std::uint64_t> publish_count_{0U};
  mutable std::atomic<std::uint64_t> read_count_{0U};
  mutable std::atomic<std::uint64_t> busy_count_{0U};
  mutable std::atomic<std::uint64_t> corrupt_count_{0U};
  std::atomic<std::uint64_t> invalidation_count_{0U};
  std::atomic<bool> populated_{false};
  std::atomic<bool> invalidated_{false};
};

class MarketStateController final {
public:
  MarketStateController(MarketStateConfig config, TransitionJournalSink journal,
                        std::uint64_t initial_process_monotonic_time_ns,
                        std::uint64_t initial_wall_clock_utc_ns) noexcept;
  MarketStateController(const MarketStateController&) = delete;
  MarketStateController& operator=(const MarketStateController&) = delete;

  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] std::uint64_t configuration_hash() const noexcept;
  [[nodiscard]] const MarketStateSnapshot& snapshot() const noexcept;
  [[nodiscard]] const AtomicMarketStatePublisher& publisher() const noexcept;
  [[nodiscard]] ControllerMetrics metrics() const noexcept;
  [[nodiscard]] EvaluationResult evaluate(const MarketStateInput& input) noexcept;

private:
  struct Candidate {
    MarketState state{MarketState::data_degraded};
    MarketStateReason primary_reason{MarketStateReason::malformed_input};
    std::uint64_t reason_mask{};
    bool malformed{false};
  };

  [[nodiscard]] static Candidate candidate(MarketState state, MarketStateReason reason,
                                           bool malformed = false) noexcept;
  [[nodiscard]] static Candidate
  resolve_absolute_priority(const MarketStateInput& input, bool& matched) noexcept;
  [[nodiscard]] static Candidate
  resolve_integrity_priority(const MarketStateInput& input, bool& matched) noexcept;
  [[nodiscard]] static Candidate
  resolve_recovery_priority(const MarketStateInput& input, bool& matched) noexcept;
  [[nodiscard]] Candidate resolve_event_priority(const MarketStateInput& input,
                                                 bool& matched) const noexcept;
  [[nodiscard]] Candidate
  resolve_predictive_priority(const MarketStateInput& input) const noexcept;
  [[nodiscard]] Candidate resolve(const MarketStateInput& input) const noexcept;
  [[nodiscard]] Candidate apply_event_exit_guard(const MarketStateInput& input,
                                                 Candidate candidate,
                                                 bool& held) const noexcept;
  [[nodiscard]] Candidate apply_transition_guards(const MarketStateInput& input,
                                                  Candidate candidate,
                                                  bool& held) noexcept;
  [[nodiscard]] EvaluationResult commit(const MarketStateInput& input,
                                        const Candidate& candidate,
                                        bool advance_input_watermark) noexcept;

  MarketStateConfig config_;
  TransitionJournalSink journal_;
  AtomicMarketStatePublisher publisher_;
  MarketStateSnapshot snapshot_;
  ControllerMetrics metrics_;
  std::uint64_t configuration_hash_{};
  std::uint64_t last_input_sequence_{};
  std::uint64_t last_input_process_monotonic_time_ns_{};
  std::uint64_t exit_stabilization_started_ns_{};
  bool publication_invalidated_{false};
  bool initialized_{false};
};

[[nodiscard]] bool valid_market_state(MarketState state) noexcept;
[[nodiscard]] bool transition_permitted(MarketState from, MarketState to) noexcept;
[[nodiscard]] std::uint64_t reason_bit(MarketStateReason reason) noexcept;
[[nodiscard]] std::uint64_t
stable_market_state_hash(const MarketStateSnapshot& snapshot) noexcept;
[[nodiscard]] std::uint64_t
stable_transition_hash(const MarketStateTransition& transition) noexcept;
[[nodiscard]] std::uint64_t
stable_config_hash(const MarketStateConfig& config) noexcept;

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::is_trivially_copyable_v<MarketStateInput>);
static_assert(std::is_trivially_copyable_v<MarketStateSnapshot>);
static_assert(std::is_trivially_copyable_v<MarketStateTransition>);

} // namespace aegis::market_state

#endif // AEGIS_MARKET_STATE_CONTROLLER_HPP
