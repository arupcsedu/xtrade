#include "aegis/market_state/controller.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace aegis::market_state {
namespace {

constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;

template <std::size_t Size>
[[nodiscard]] constexpr std::uint64_t
hash_words(const std::array<std::uint64_t, Size>& words) noexcept {
  auto hash = kFnvOffset;
  for (const auto word : words) {
    for (std::size_t byte = 0U; byte < sizeof(word); ++byte) {
      const auto shift = static_cast<unsigned>(byte * 8U);
      hash ^= (word >> shift) & 0xFFU;
      hash *= kFnvPrime;
    }
  }
  return hash;
}

[[nodiscard]] constexpr std::uint64_t
schema_word(const MarketStateSnapshot& snapshot) noexcept {
  return static_cast<std::uint64_t>(snapshot.schema_major) |
         (static_cast<std::uint64_t>(snapshot.schema_minor) << 16U);
}

[[nodiscard]] constexpr std::uint64_t
state_word(const MarketStateSnapshot& snapshot) noexcept {
  return static_cast<std::uint64_t>(snapshot.state) |
         (static_cast<std::uint64_t>(snapshot.primary_reason) << 8U);
}

[[nodiscard]] constexpr bool
valid_official_status(const OfficialTradingStatus value) noexcept {
  return value >= OfficialTradingStatus::unknown &&
         value <= OfficialTradingStatus::closed;
}

[[nodiscard]] constexpr bool valid_feed_health(const FeedHealth value) noexcept {
  return value >= FeedHealth::unknown && value <= FeedHealth::invalid;
}

[[nodiscard]] constexpr bool valid_book(const BookValidity value) noexcept {
  return value >= BookValidity::recovering && value <= BookValidity::invalid;
}

[[nodiscard]] constexpr bool valid_clock(const ClockQuality value) noexcept {
  return value >= ClockQuality::unknown && value <= ClockQuality::unsafe;
}

[[nodiscard]] constexpr bool valid_news(const NewsEventState value) noexcept {
  return value >= NewsEventState::none && value <= NewsEventState::resolved;
}

[[nodiscard]] constexpr bool
valid_calendar_state(const CalendarEventState value) noexcept {
  return value >= CalendarEventState::none && value <= CalendarEventState::complete;
}

[[nodiscard]] constexpr bool
valid_calendar(const CalendarEventInput& calendar) noexcept {
  if (!valid_calendar_state(calendar.state)) {
    return false;
  }
  return calendar.state == CalendarEventState::none
             ? calendar.release_wall_clock_utc_ns == 0U
             : calendar.release_wall_clock_utc_ns != 0U;
}

[[nodiscard]] constexpr bool valid_input_shape(const MarketStateInput& input) noexcept {
  return input.input_sequence != 0U && input.process_monotonic_time_ns != 0U &&
         input.wall_clock_utc_ns != 0U &&
         valid_official_status(input.official_trading_status) &&
         valid_feed_health(input.feed_health) && valid_book(input.book_validity) &&
         valid_clock(input.clock_quality) && valid_news(input.news_event_state) &&
         valid_calendar(input.earnings_calendar) &&
         valid_calendar(input.macro_calendar) &&
         input.realized_volatility_ppm <= kPartsPerMillion &&
         input.model_ood_ppm <= kPartsPerMillion &&
         input.model_disagreement_ppm <= kPartsPerMillion;
}

[[nodiscard]] constexpr bool
scheduled_window_active(const CalendarEventInput& calendar,
                        const std::uint64_t wall_clock_utc_ns,
                        const std::uint64_t lead_ns) noexcept {
  return calendar.state == CalendarEventState::scheduled &&
         wall_clock_utc_ns <= calendar.release_wall_clock_utc_ns &&
         calendar.release_wall_clock_utc_ns - wall_clock_utc_ns <= lead_ns;
}

[[nodiscard]] constexpr bool
scheduled_overdue(const CalendarEventInput& calendar,
                  const std::uint64_t wall_clock_utc_ns) noexcept {
  return calendar.state == CalendarEventState::scheduled &&
         wall_clock_utc_ns > calendar.release_wall_clock_utc_ns;
}

[[nodiscard]] constexpr bool
release_active(const CalendarEventInput& calendar) noexcept {
  return calendar.state == CalendarEventState::release_active;
}

[[nodiscard]] constexpr bool is_safety_state(const MarketState state) noexcept {
  return state == MarketState::halted || state == MarketState::data_degraded ||
         state == MarketState::shutdown;
}

[[nodiscard]] constexpr bool is_event_state(const MarketState state) noexcept {
  return state == MarketState::scheduled_event || state == MarketState::breaking_news ||
         state == MarketState::event_price_discovery ||
         state == MarketState::volatility_spike;
}

[[nodiscard]] constexpr std::uint64_t elapsed_since(const std::uint64_t now,
                                                    const std::uint64_t then) noexcept {
  return now >= then ? now - then : 0U;
}

void add_reason(std::uint64_t& mask, const MarketStateReason reason) noexcept {
  mask |= reason_bit(reason);
}

} // namespace

bool valid_market_state(const MarketState state) noexcept {
  return state >= MarketState::startup && state <= MarketState::shutdown;
}

std::uint64_t reason_bit(const MarketStateReason reason) noexcept {
  const auto ordinal = static_cast<std::uint8_t>(reason);
  return reason <= MarketStateReason::forbidden_transition_prevented
             ? std::uint64_t{1U} << ordinal
             : 0U;
}

bool transition_permitted(const MarketState from, const MarketState to) noexcept {
  if (!valid_market_state(from) || !valid_market_state(to)) {
    return false;
  }
  if (from == to) {
    return true;
  }
  if (to == MarketState::halted || to == MarketState::data_degraded ||
      to == MarketState::shutdown) {
    return from != MarketState::shutdown;
  }
  switch (from) {
  case MarketState::startup:
  case MarketState::halted:
  case MarketState::data_degraded:
    return to == MarketState::reopening || to == MarketState::recovery;
  case MarketState::reopening:
    return to == MarketState::recovery;
  case MarketState::recovery:
    return to == MarketState::normal || to == MarketState::scheduled_event ||
           to == MarketState::breaking_news ||
           to == MarketState::event_price_discovery ||
           to == MarketState::volatility_spike || to == MarketState::reopening;
  case MarketState::normal:
  case MarketState::scheduled_event:
  case MarketState::breaking_news:
  case MarketState::event_price_discovery:
  case MarketState::volatility_spike:
    return to == MarketState::normal || to == MarketState::scheduled_event ||
           to == MarketState::breaking_news ||
           to == MarketState::event_price_discovery ||
           to == MarketState::volatility_spike || to == MarketState::reopening ||
           to == MarketState::recovery;
  case MarketState::shutdown:
    return false;
  }
  return false;
}

std::uint64_t stable_market_state_hash(const MarketStateSnapshot& snapshot) noexcept {
  return hash_words(std::array{
      schema_word(snapshot), state_word(snapshot), snapshot.reason_mask,
      snapshot.input_sequence, snapshot.transition_sequence,
      snapshot.observed_process_monotonic_time_ns, snapshot.observed_wall_clock_utc_ns,
      snapshot.state_entered_process_monotonic_time_ns});
}

std::uint64_t stable_transition_hash(const MarketStateTransition& transition) noexcept {
  const auto schema = static_cast<std::uint64_t>(transition.schema_major) |
                      (static_cast<std::uint64_t>(transition.schema_minor) << 16U);
  const auto states = static_cast<std::uint64_t>(transition.from_state) |
                      (static_cast<std::uint64_t>(transition.to_state) << 8U) |
                      (static_cast<std::uint64_t>(transition.primary_reason) << 16U);
  return hash_words(std::array{
      schema, states, transition.reason_mask, transition.input_sequence,
      transition.transition_sequence, transition.observed_process_monotonic_time_ns,
      transition.observed_wall_clock_utc_ns, transition.previous_snapshot_hash,
      transition.next_snapshot_hash, transition.configuration_hash});
}

std::uint64_t stable_config_hash(const MarketStateConfig& config) noexcept {
  return hash_words(std::array{
      config.minimum_dwell_ns, config.recovery_stabilization_ns,
      config.reopening_stabilization_ns, config.scheduled_event_lead_ns,
      static_cast<std::uint64_t>(config.volatility_spike_threshold_ppm),
      config.spread_spike_threshold_ticks, config.minimum_aggregate_depth_units,
      static_cast<std::uint64_t>(config.model_ood_threshold_ppm),
      static_cast<std::uint64_t>(config.model_disagreement_threshold_ppm)});
}

bool MarketStateSnapshot::valid() const noexcept {
  return schema_major == kMarketStateSchemaMajor &&
         schema_minor == kMarketStateSchemaMinor && valid_market_state(state) &&
         reason_bit(primary_reason) != 0U &&
         (reason_mask & reason_bit(primary_reason)) != 0U &&
         observed_process_monotonic_time_ns != 0U && observed_wall_clock_utc_ns != 0U &&
         state_entered_process_monotonic_time_ns != 0U && stable_hash != 0U &&
         stable_hash == stable_market_state_hash(*this);
}

bool MarketStateTransition::valid() const noexcept {
  return schema_major == kMarketStateSchemaMajor &&
         schema_minor == kMarketStateSchemaMinor &&
         transition_permitted(from_state, to_state) &&
         reason_bit(primary_reason) != 0U &&
         (reason_mask & reason_bit(primary_reason)) != 0U &&
         observed_process_monotonic_time_ns != 0U && observed_wall_clock_utc_ns != 0U &&
         next_snapshot_hash != 0U && configuration_hash != 0U && record_hash != 0U &&
         record_hash == stable_transition_hash(*this);
}

PublishStatus
AtomicMarketStatePublisher::publish(const MarketStateSnapshot& snapshot) noexcept {
  if (!snapshot.valid()) {
    return PublishStatus::invalid_snapshot;
  }
  auto sequence = publication_sequence_.load(std::memory_order_relaxed);
  if ((sequence & 1U) != 0U || !publication_sequence_.compare_exchange_strong(
                                   sequence, sequence + 1U, std::memory_order_acq_rel,
                                   std::memory_order_relaxed)) {
    busy_count_.fetch_add(1U, std::memory_order_relaxed);
    return PublishStatus::busy;
  }
  schema_word_.store(schema_word(snapshot), std::memory_order_relaxed);
  state_word_.store(state_word(snapshot), std::memory_order_relaxed);
  reason_mask_.store(snapshot.reason_mask, std::memory_order_relaxed);
  input_sequence_.store(snapshot.input_sequence, std::memory_order_relaxed);
  transition_sequence_.store(snapshot.transition_sequence, std::memory_order_relaxed);
  observed_process_monotonic_time_ns_.store(snapshot.observed_process_monotonic_time_ns,
                                            std::memory_order_relaxed);
  observed_wall_clock_utc_ns_.store(snapshot.observed_wall_clock_utc_ns,
                                    std::memory_order_relaxed);
  state_entered_process_monotonic_time_ns_.store(
      snapshot.state_entered_process_monotonic_time_ns, std::memory_order_relaxed);
  stable_hash_.store(snapshot.stable_hash, std::memory_order_relaxed);
  populated_.store(true, std::memory_order_relaxed);
  publication_sequence_.store(sequence + 2U, std::memory_order_release);
  invalidated_.store(false, std::memory_order_release);
  publish_count_.fetch_add(1U, std::memory_order_relaxed);
  return PublishStatus::published;
}

void AtomicMarketStatePublisher::invalidate() noexcept {
  invalidated_.store(true, std::memory_order_release);
  invalidation_count_.fetch_add(1U, std::memory_order_relaxed);
}

ReadStatus
AtomicMarketStatePublisher::read(MarketStateSnapshot& output) const noexcept {
  if (!populated_.load(std::memory_order_acquire)) {
    return ReadStatus::empty;
  }
  if (invalidated_.load(std::memory_order_acquire)) {
    return ReadStatus::invalidated;
  }
  for (std::size_t retry = 0U; retry < kMaximumReadRetries; ++retry) {
    const auto before = publication_sequence_.load(std::memory_order_acquire);
    if ((before & 1U) != 0U) {
      continue;
    }
    const auto schema = schema_word_.load(std::memory_order_relaxed);
    const auto state = state_word_.load(std::memory_order_relaxed);
    const MarketStateSnapshot candidate{
        .schema_major = static_cast<std::uint16_t>(schema & 0xFFFFU),
        .schema_minor = static_cast<std::uint16_t>((schema >> 16U) & 0xFFFFU),
        .state = static_cast<MarketState>(state & 0xFFU),
        .primary_reason = static_cast<MarketStateReason>((state >> 8U) & 0xFFU),
        .reason_mask = reason_mask_.load(std::memory_order_relaxed),
        .input_sequence = input_sequence_.load(std::memory_order_relaxed),
        .transition_sequence = transition_sequence_.load(std::memory_order_relaxed),
        .observed_process_monotonic_time_ns =
            observed_process_monotonic_time_ns_.load(std::memory_order_relaxed),
        .observed_wall_clock_utc_ns =
            observed_wall_clock_utc_ns_.load(std::memory_order_relaxed),
        .state_entered_process_monotonic_time_ns =
            state_entered_process_monotonic_time_ns_.load(std::memory_order_relaxed),
        .stable_hash = stable_hash_.load(std::memory_order_relaxed)};
    const auto after = publication_sequence_.load(std::memory_order_acquire);
    if (before != after || (after & 1U) != 0U) {
      continue;
    }
    if (!candidate.valid()) {
      corrupt_count_.fetch_add(1U, std::memory_order_relaxed);
      return ReadStatus::corrupt;
    }
    output = candidate;
    read_count_.fetch_add(1U, std::memory_order_relaxed);
    return ReadStatus::snapshot;
  }
  busy_count_.fetch_add(1U, std::memory_order_relaxed);
  return ReadStatus::busy;
}

PublisherMetrics AtomicMarketStatePublisher::metrics() const noexcept {
  return {.publish_count = publish_count_.load(std::memory_order_relaxed),
          .read_count = read_count_.load(std::memory_order_relaxed),
          .busy_count = busy_count_.load(std::memory_order_relaxed),
          .corrupt_count = corrupt_count_.load(std::memory_order_relaxed),
          .invalidation_count = invalidation_count_.load(std::memory_order_relaxed)};
}

MarketStateController::MarketStateController(
    const MarketStateConfig config, const TransitionJournalSink journal,
    const std::uint64_t initial_process_monotonic_time_ns,
    const std::uint64_t initial_wall_clock_utc_ns) noexcept
    : config_(config), journal_(journal),
      configuration_hash_(stable_config_hash(config)),
      last_input_process_monotonic_time_ns_(initial_process_monotonic_time_ns) {
  if (!config_.valid() || !journal_.valid() ||
      initial_process_monotonic_time_ns == 0U || initial_wall_clock_utc_ns == 0U) {
    return;
  }
  snapshot_ = {.state = MarketState::startup,
               .primary_reason = MarketStateReason::startup_initialized,
               .reason_mask = reason_bit(MarketStateReason::startup_initialized),
               .input_sequence = 0U,
               .transition_sequence = 0U,
               .observed_process_monotonic_time_ns = initial_process_monotonic_time_ns,
               .observed_wall_clock_utc_ns = initial_wall_clock_utc_ns,
               .state_entered_process_monotonic_time_ns =
                   initial_process_monotonic_time_ns};
  snapshot_.stable_hash = stable_market_state_hash(snapshot_);
  auto transition = MarketStateTransition{
      .from_state = MarketState::startup,
      .to_state = MarketState::startup,
      .primary_reason = MarketStateReason::startup_initialized,
      .reason_mask = snapshot_.reason_mask,
      .input_sequence = 0U,
      .transition_sequence = 0U,
      .observed_process_monotonic_time_ns = initial_process_monotonic_time_ns,
      .observed_wall_clock_utc_ns = initial_wall_clock_utc_ns,
      .previous_snapshot_hash = 0U,
      .next_snapshot_hash = snapshot_.stable_hash,
      .configuration_hash = configuration_hash_};
  transition.record_hash = stable_transition_hash(transition);
  if (journal_.append(transition) != JournalAppendStatus::accepted ||
      publisher_.publish(snapshot_) != PublishStatus::published) {
    snapshot_ = {};
    return;
  }
  initialized_ = true;
}

bool MarketStateController::initialized() const noexcept { return initialized_; }

std::uint64_t MarketStateController::configuration_hash() const noexcept {
  return configuration_hash_;
}

const MarketStateSnapshot& MarketStateController::snapshot() const noexcept {
  return snapshot_;
}

const AtomicMarketStatePublisher& MarketStateController::publisher() const noexcept {
  return publisher_;
}

ControllerMetrics MarketStateController::metrics() const noexcept { return metrics_; }

MarketStateController::Candidate
MarketStateController::candidate(const MarketState state,
                                 const MarketStateReason reason,
                                 const bool malformed) noexcept {
  return {.state = state,
          .primary_reason = reason,
          .reason_mask = reason_bit(reason),
          .malformed = malformed};
}

MarketStateController::Candidate
MarketStateController::resolve_absolute_priority(const MarketStateInput& input,
                                                 bool& matched) noexcept {
  matched = true;
  // Official halt/close is inspectable independently and outranks every
  // inferred market condition, even when another field is malformed.
  if (valid_official_status(input.official_trading_status) &&
      input.official_trading_status == OfficialTradingStatus::halted) {
    return candidate(MarketState::halted, MarketStateReason::official_halt);
  }
  if (valid_official_status(input.official_trading_status) &&
      input.official_trading_status == OfficialTradingStatus::closed) {
    return candidate(MarketState::halted, MarketStateReason::official_market_closed);
  }
  if (input.operator_controls.shutdown_requested) {
    return candidate(MarketState::shutdown, MarketStateReason::operator_shutdown);
  }
  if (!valid_input_shape(input)) {
    return candidate(MarketState::data_degraded, MarketStateReason::malformed_input,
                     true);
  }
  matched = false;
  return {};
}

MarketStateController::Candidate
MarketStateController::resolve_integrity_priority(const MarketStateInput& input,
                                                  bool& matched) noexcept {
  matched = true;
  // 2. Unsafe clock.
  if (input.clock_quality == ClockQuality::unsafe) {
    return candidate(MarketState::data_degraded, MarketStateReason::clock_unsafe);
  }
  if (input.clock_quality == ClockQuality::unknown) {
    return candidate(MarketState::data_degraded, MarketStateReason::clock_unknown);
  }

  // 3. Invalid or stale current data.
  if (input.official_trading_status == OfficialTradingStatus::unknown) {
    return candidate(MarketState::data_degraded,
                     MarketStateReason::official_status_unknown);
  }
  if (input.feed_health == FeedHealth::invalid) {
    return candidate(MarketState::data_degraded, MarketStateReason::feed_invalid);
  }
  if (input.feed_health == FeedHealth::stale) {
    return candidate(MarketState::data_degraded, MarketStateReason::feed_stale);
  }
  if (input.feed_health == FeedHealth::unknown) {
    return candidate(MarketState::data_degraded, MarketStateReason::feed_unknown);
  }
  if (input.book_validity == BookValidity::invalid) {
    return candidate(MarketState::data_degraded, MarketStateReason::book_invalid);
  }
  if (scheduled_overdue(input.earnings_calendar, input.wall_clock_utc_ns) ||
      scheduled_overdue(input.macro_calendar, input.wall_clock_utc_ns)) {
    return candidate(MarketState::data_degraded,
                     MarketStateReason::scheduled_release_overdue);
  }
  if (input.feed_health == FeedHealth::degraded) {
    return candidate(MarketState::data_degraded, MarketStateReason::feed_degraded);
  }
  if (input.clock_quality == ClockQuality::degraded) {
    return candidate(MarketState::data_degraded, MarketStateReason::clock_degraded);
  }

  // 4. Kill switch.
  if (input.operator_controls.kill_switch_engaged) {
    return candidate(MarketState::data_degraded,
                     MarketStateReason::kill_switch_engaged);
  }
  matched = false;
  return {};
}

MarketStateController::Candidate
MarketStateController::resolve_recovery_priority(const MarketStateInput& input,
                                                 bool& matched) noexcept {
  matched = true;
  // 5. Recovery and reopening.
  if (input.official_trading_status == OfficialTradingStatus::pre_open) {
    return candidate(MarketState::reopening, MarketStateReason::reopening_pre_open);
  }
  if (input.official_trading_status == OfficialTradingStatus::auction) {
    return candidate(MarketState::reopening, MarketStateReason::reopening_auction);
  }
  if (input.clock_quality == ClockQuality::syncing) {
    return candidate(MarketState::recovery, MarketStateReason::clock_syncing);
  }
  if (input.feed_health == FeedHealth::recovering) {
    return candidate(MarketState::recovery, MarketStateReason::feed_recovering);
  }
  if (input.book_validity == BookValidity::recovering) {
    return candidate(MarketState::recovery, MarketStateReason::book_recovering);
  }
  if (input.operator_controls.recovery_requested) {
    return candidate(MarketState::recovery, MarketStateReason::operator_recovery);
  }
  if (input.news_event_state == NewsEventState::resolved) {
    return candidate(MarketState::recovery, MarketStateReason::event_cooldown);
  }
  matched = false;
  return {};
}

MarketStateController::Candidate
MarketStateController::resolve_event_priority(const MarketStateInput& input,
                                              bool& matched) const noexcept {
  matched = true;
  // 6. Event states. Breaking news outranks release price discovery, which
  // outranks a pre-release scheduled window.
  if (input.news_event_state == NewsEventState::breaking) {
    return candidate(MarketState::breaking_news, MarketStateReason::news_breaking);
  }
  if (input.news_event_state == NewsEventState::price_discovery ||
      release_active(input.earnings_calendar) || release_active(input.macro_calendar)) {
    return candidate(MarketState::event_price_discovery,
                     MarketStateReason::event_price_discovery);
  }
  const bool earnings_scheduled =
      scheduled_window_active(input.earnings_calendar, input.wall_clock_utc_ns,
                              config_.scheduled_event_lead_ns);
  const bool macro_scheduled = scheduled_window_active(
      input.macro_calendar, input.wall_clock_utc_ns, config_.scheduled_event_lead_ns);
  if (earnings_scheduled && macro_scheduled) {
    return candidate(MarketState::scheduled_event,
                     MarketStateReason::earnings_and_macro_scheduled);
  }
  if (earnings_scheduled) {
    return candidate(MarketState::scheduled_event,
                     MarketStateReason::earnings_scheduled);
  }
  if (macro_scheduled) {
    return candidate(MarketState::scheduled_event, MarketStateReason::macro_scheduled);
  }
  matched = false;
  return {};
}

MarketStateController::Candidate MarketStateController::resolve_predictive_priority(
    const MarketStateInput& input) const noexcept {
  Candidate result{};
  const auto mark = [&result](const bool active,
                              const MarketStateReason reason) noexcept {
    if (!active) {
      return;
    }
    if (result.reason_mask == 0U) {
      result.state = MarketState::volatility_spike;
      result.primary_reason = reason;
    }
    add_reason(result.reason_mask, reason);
  };
  // 7. Normal predictive state and bounded stress/OOD thresholds.
  mark(input.realized_volatility_ppm >= config_.volatility_spike_threshold_ppm,
       MarketStateReason::volatility_threshold);
  mark(input.spread_ticks >= config_.spread_spike_threshold_ticks,
       MarketStateReason::spread_threshold);
  mark(input.aggregate_depth_units < config_.minimum_aggregate_depth_units,
       MarketStateReason::depth_threshold);
  mark(input.model_ood_ppm >= config_.model_ood_threshold_ppm,
       MarketStateReason::model_ood_threshold);
  mark(input.model_disagreement_ppm >= config_.model_disagreement_threshold_ppm,
       MarketStateReason::model_disagreement_threshold);
  if (result.reason_mask == 0U) {
    return candidate(MarketState::normal, MarketStateReason::normal_conditions_stable);
  }
  return result;
}

MarketStateController::Candidate
MarketStateController::resolve(const MarketStateInput& input) const noexcept {
  bool matched = false;
  auto result = resolve_absolute_priority(input, matched);
  if (matched) {
    return result;
  }
  result = resolve_integrity_priority(input, matched);
  if (matched) {
    return result;
  }
  result = resolve_recovery_priority(input, matched);
  if (matched) {
    return result;
  }
  result = resolve_event_priority(input, matched);
  if (matched) {
    return result;
  }
  return resolve_predictive_priority(input);
}

MarketStateController::Candidate MarketStateController::apply_transition_guards(
    const MarketStateInput& input, Candidate candidate, bool& held) noexcept {
  held = false;
  const auto current = snapshot_.state;
  if ((current == MarketState::reopening || current == MarketState::recovery) &&
      candidate.state == current) {
    exit_stabilization_started_ns_ = 0U;
    return candidate;
  }
  if (candidate.state == current || is_safety_state(candidate.state)) {
    exit_stabilization_started_ns_ = 0U;
    return candidate;
  }

  // A cleared halt, invalid book/feed, kill, or startup must pass recovery.
  if ((current == MarketState::startup || current == MarketState::halted ||
       current == MarketState::data_degraded) &&
      candidate.state != MarketState::reopening) {
    candidate.state = MarketState::recovery;
    candidate.primary_reason = MarketStateReason::safety_recovery_required;
    candidate.reason_mask = reason_bit(candidate.primary_reason);
    exit_stabilization_started_ns_ = 0U;
    return candidate;
  }

  if (current == MarketState::reopening && candidate.state != MarketState::reopening) {
    if (exit_stabilization_started_ns_ == 0U) {
      exit_stabilization_started_ns_ = input.process_monotonic_time_ns;
    }
    if (elapsed_since(input.process_monotonic_time_ns, exit_stabilization_started_ns_) <
        config_.reopening_stabilization_ns) {
      held = true;
      return {.state = current,
              .primary_reason = MarketStateReason::reopening_stabilizing,
              .reason_mask = reason_bit(MarketStateReason::reopening_stabilizing)};
    }
    candidate.state = MarketState::recovery;
    candidate.primary_reason = MarketStateReason::safety_recovery_required;
    candidate.reason_mask = reason_bit(candidate.primary_reason);
    exit_stabilization_started_ns_ = 0U;
    return candidate;
  }

  if (current == MarketState::recovery && candidate.state != MarketState::recovery &&
      candidate.state != MarketState::reopening) {
    if (exit_stabilization_started_ns_ == 0U) {
      exit_stabilization_started_ns_ = input.process_monotonic_time_ns;
    }
    if (elapsed_since(input.process_monotonic_time_ns, exit_stabilization_started_ns_) <
        config_.recovery_stabilization_ns) {
      held = true;
      return {.state = current,
              .primary_reason = MarketStateReason::recovery_stabilizing,
              .reason_mask = reason_bit(MarketStateReason::recovery_stabilizing)};
    }
    exit_stabilization_started_ns_ = 0U;
    return candidate;
  }

  candidate = apply_event_exit_guard(input, candidate, held);
  if (held) {
    return candidate;
  }

  if (!transition_permitted(current, candidate.state)) {
    candidate.state = MarketState::recovery;
    candidate.primary_reason = MarketStateReason::forbidden_transition_prevented;
    candidate.reason_mask = reason_bit(candidate.primary_reason);
  }
  return candidate;
}

MarketStateController::Candidate MarketStateController::apply_event_exit_guard(
    const MarketStateInput& input, Candidate candidate, bool& held) const noexcept {
  const auto current = snapshot_.state;
  const bool normal_event_exit = candidate.state == MarketState::normal;
  const bool requested_event_exit =
      candidate.primary_reason == MarketStateReason::event_cooldown ||
      candidate.primary_reason == MarketStateReason::operator_recovery;
  if (!is_event_state(current) || (!normal_event_exit && !requested_event_exit)) {
    return candidate;
  }
  const auto state_age =
      elapsed_since(input.process_monotonic_time_ns,
                    snapshot_.state_entered_process_monotonic_time_ns);
  if (state_age < config_.minimum_dwell_ns) {
    held = true;
    return {.state = current,
            .primary_reason = MarketStateReason::minimum_dwell_active,
            .reason_mask = reason_bit(MarketStateReason::minimum_dwell_active)};
  }
  candidate.state = MarketState::recovery;
  if (normal_event_exit) {
    candidate.primary_reason = MarketStateReason::event_cooldown;
  }
  candidate.reason_mask = reason_bit(candidate.primary_reason);
  return candidate;
}

EvaluationResult
MarketStateController::commit(const MarketStateInput& input, const Candidate& candidate,
                              const bool advance_input_watermark) noexcept {
  const bool state_changed = candidate.state != snapshot_.state;
  auto next = MarketStateSnapshot{
      .state = candidate.state,
      .primary_reason = candidate.primary_reason,
      .reason_mask = candidate.reason_mask,
      .input_sequence = input.input_sequence,
      .transition_sequence = snapshot_.transition_sequence + 1U,
      .observed_process_monotonic_time_ns = input.process_monotonic_time_ns,
      .observed_wall_clock_utc_ns = input.wall_clock_utc_ns,
      .state_entered_process_monotonic_time_ns =
          state_changed ? input.process_monotonic_time_ns
                        : snapshot_.state_entered_process_monotonic_time_ns};
  next.stable_hash = stable_market_state_hash(next);
  auto transition = MarketStateTransition{
      .from_state = snapshot_.state,
      .to_state = next.state,
      .primary_reason = next.primary_reason,
      .reason_mask = next.reason_mask,
      .input_sequence = next.input_sequence,
      .transition_sequence = next.transition_sequence,
      .observed_process_monotonic_time_ns = next.observed_process_monotonic_time_ns,
      .observed_wall_clock_utc_ns = next.observed_wall_clock_utc_ns,
      .previous_snapshot_hash = snapshot_.stable_hash,
      .next_snapshot_hash = next.stable_hash,
      .configuration_hash = configuration_hash_};
  transition.record_hash = stable_transition_hash(transition);
  if (journal_.append(transition) != JournalAppendStatus::accepted) {
    ++metrics_.journal_rejection_count;
    publisher_.invalidate();
    publication_invalidated_ = true;
    return {.status = EvaluationStatus::journal_rejected,
            .snapshot = snapshot_,
            .transitioned = false,
            .new_orders_permitted = false};
  }
  const auto publish_status = publisher_.publish(next);
  if (publish_status != PublishStatus::published) {
    // The controller is the sole publisher. A failure indicates violated
    // ownership or memory corruption and is therefore fail-closed.
    publisher_.invalidate();
    initialized_ = false;
    return {.status = EvaluationStatus::not_initialized,
            .snapshot = snapshot_,
            .transitioned = false,
            .new_orders_permitted = false};
  }
  snapshot_ = next;
  publication_invalidated_ = false;
  exit_stabilization_started_ns_ = 0U;
  if (advance_input_watermark) {
    last_input_sequence_ = input.input_sequence;
    last_input_process_monotonic_time_ns_ = input.process_monotonic_time_ns;
  }
  ++metrics_.transition_count;
  return {.status = EvaluationStatus::transitioned,
          .snapshot = snapshot_,
          .transitioned = true,
          .new_orders_permitted = snapshot_.state == MarketState::normal};
}

EvaluationResult
MarketStateController::evaluate(const MarketStateInput& input) noexcept {
  if (!initialized_) {
    return {.status = EvaluationStatus::not_initialized,
            .snapshot = snapshot_,
            .transitioned = false,
            .new_orders_permitted = false};
  }
  ++metrics_.evaluation_count;
  if (snapshot_.state == MarketState::shutdown) {
    return {.status = EvaluationStatus::shutdown_terminal,
            .snapshot = snapshot_,
            .transitioned = false,
            .new_orders_permitted = false};
  }

  const bool ordered =
      input.input_sequence > last_input_sequence_ &&
      input.process_monotonic_time_ns >= last_input_process_monotonic_time_ns_;
  auto candidate = resolve(input);
  const bool absolute_safety_override = candidate.state == MarketState::halted ||
                                        candidate.state == MarketState::shutdown;
  if (!ordered) {
    ++metrics_.invalid_input_count;
  }
  if (!ordered && !absolute_safety_override) {
    candidate = {.state = MarketState::data_degraded,
                 .primary_reason = MarketStateReason::input_ordering_invalid,
                 .reason_mask = reason_bit(MarketStateReason::input_ordering_invalid),
                 .malformed = true};
  }
  if (candidate.malformed && ordered) {
    ++metrics_.invalid_input_count;
  }

  bool held = false;
  candidate = apply_transition_guards(input, candidate, held);
  if (held) {
    last_input_sequence_ = input.input_sequence;
    last_input_process_monotonic_time_ns_ = input.process_monotonic_time_ns;
    ++metrics_.dwell_hold_count;
    return {.status = EvaluationStatus::held_for_dwell,
            .snapshot = snapshot_,
            .transitioned = false,
            .new_orders_permitted = snapshot_.state == MarketState::normal};
  }
  if (candidate.state == snapshot_.state &&
      candidate.primary_reason == snapshot_.primary_reason &&
      candidate.reason_mask == snapshot_.reason_mask && !publication_invalidated_) {
    if (ordered) {
      last_input_sequence_ = input.input_sequence;
      last_input_process_monotonic_time_ns_ = input.process_monotonic_time_ns;
    }
    ++metrics_.unchanged_count;
    return {.status = EvaluationStatus::unchanged,
            .snapshot = snapshot_,
            .transitioned = false,
            .new_orders_permitted = snapshot_.state == MarketState::normal};
  }
  return commit(input, candidate, ordered);
}

} // namespace aegis::market_state
