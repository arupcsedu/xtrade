#include "aegis/market_state/controller.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace state = aegis::market_state;

namespace {

inline constexpr std::size_t kJournalCapacity = 512U;
inline constexpr std::uint64_t kInitialMonotonicNs = 100U;
inline constexpr std::uint64_t kInitialWallNs = 1'000'000U;

class RecordingJournal final {
public:
  [[nodiscard]] static state::JournalAppendStatus
  append(void* context, const state::MarketStateTransition& transition) noexcept {
    auto& journal = *static_cast<RecordingJournal*>(context);
    if (journal.status_ != state::JournalAppendStatus::accepted) {
      return journal.status_;
    }
    if (journal.size_ == journal.records_.size()) {
      return state::JournalAppendStatus::full;
    }
    journal.records_[journal.size_] = transition;
    ++journal.size_;
    return state::JournalAppendStatus::accepted;
  }

  [[nodiscard]] state::TransitionJournalSink sink() noexcept {
    return {this, &RecordingJournal::append};
  }

  void reject(const state::JournalAppendStatus status) noexcept { status_ = status; }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  [[nodiscard]] const state::MarketStateTransition&
  operator[](const std::size_t index) const noexcept {
    return records_[index];
  }

private:
  std::array<state::MarketStateTransition, kJournalCapacity> records_{};
  std::size_t size_{};
  state::JournalAppendStatus status_{state::JournalAppendStatus::accepted};
};

[[nodiscard]] constexpr state::MarketStateConfig config() noexcept {
  return {.minimum_dwell_ns = 10U,
          .recovery_stabilization_ns = 20U,
          .reopening_stabilization_ns = 30U,
          .scheduled_event_lead_ns = 100U,
          .volatility_spike_threshold_ppm = 200'000U,
          .spread_spike_threshold_ticks = 10U,
          .minimum_aggregate_depth_units = 1'000U,
          .model_ood_threshold_ppm = 800'000U,
          .model_disagreement_threshold_ppm = 700'000U};
}

[[nodiscard]] constexpr state::MarketStateInput
healthy_input(const std::uint64_t sequence, const std::uint64_t monotonic_ns,
              const std::uint64_t wall_ns) noexcept {
  return {.input_sequence = sequence,
          .process_monotonic_time_ns = monotonic_ns,
          .wall_clock_utc_ns = wall_ns,
          .official_trading_status = state::OfficialTradingStatus::open,
          .feed_health = state::FeedHealth::healthy,
          .book_validity = state::BookValidity::valid,
          .clock_quality = state::ClockQuality::healthy,
          .news_event_state = state::NewsEventState::none,
          .earnings_calendar = {},
          .macro_calendar = {},
          .realized_volatility_ppm = 10'000U,
          .spread_ticks = 2U,
          .aggregate_depth_units = 10'000U,
          .model_ood_ppm = 10'000U,
          .model_disagreement_ppm = 10'000U,
          .operator_controls = {}};
}

void reach_normal(state::MarketStateController& controller, std::uint64_t& sequence,
                  std::uint64_t& monotonic_ns, std::uint64_t& wall_ns) {
  ++sequence;
  monotonic_ns += 1U;
  wall_ns += 1U;
  ASSERT_EQ(controller.evaluate(healthy_input(sequence, monotonic_ns, wall_ns))
                .snapshot.state,
            state::MarketState::recovery);
  ++sequence;
  monotonic_ns += 1U;
  wall_ns += 1U;
  EXPECT_EQ(controller.evaluate(healthy_input(sequence, monotonic_ns, wall_ns)).status,
            state::EvaluationStatus::held_for_dwell);
  ++sequence;
  monotonic_ns += config().recovery_stabilization_ns;
  wall_ns += config().recovery_stabilization_ns;
  const auto normal =
      controller.evaluate(healthy_input(sequence, monotonic_ns, wall_ns));
  ASSERT_EQ(normal.status, state::EvaluationStatus::transitioned);
  ASSERT_EQ(normal.snapshot.state, state::MarketState::normal);
  ASSERT_TRUE(normal.new_orders_permitted);
}

void expect_no_direct_normal_recovery_bypass(const RecordingJournal& journal) {
  for (std::size_t index = 1U; index < journal.size(); ++index) {
    EXPECT_TRUE(journal[index].valid());
    EXPECT_FALSE(journal[index].from_state == state::MarketState::halted &&
                 journal[index].to_state == state::MarketState::normal);
    EXPECT_FALSE(journal[index].from_state == state::MarketState::data_degraded &&
                 journal[index].to_state == state::MarketState::normal);
  }
}

TEST(MarketStateControllerTest, RejectsInvalidConfigurationJournalAndInitialTime) {
  RecordingJournal journal;
  auto invalid = config();
  invalid.minimum_dwell_ns = 0U;
  const state::MarketStateController bad_config{invalid, journal.sink(),
                                                kInitialMonotonicNs, kInitialWallNs};
  EXPECT_FALSE(bad_config.initialized());

  const state::MarketStateController no_journal{
      config(), {}, kInitialMonotonicNs, kInitialWallNs};
  EXPECT_FALSE(no_journal.initialized());

  const state::MarketStateController bad_time{config(), journal.sink(), 0U,
                                              kInitialWallNs};
  EXPECT_FALSE(bad_time.initialized());

  RecordingJournal stopped;
  stopped.reject(state::JournalAppendStatus::stopped);
  state::MarketStateController rejected{config(), stopped.sink(), kInitialMonotonicNs,
                                        kInitialWallNs};
  EXPECT_FALSE(rejected.initialized());
  EXPECT_EQ(rejected.evaluate(healthy_input(1U, 101U, 1'000'001U)).status,
            state::EvaluationStatus::not_initialized);
}

TEST(MarketStateControllerTest, InitializationIsJournaledAndAtomicallyPublished) {
  RecordingJournal journal;
  const state::MarketStateController controller{config(), journal.sink(),
                                                kInitialMonotonicNs, kInitialWallNs};
  ASSERT_TRUE(controller.initialized());
  ASSERT_EQ(journal.size(), 1U);
  EXPECT_TRUE(journal[0U].valid());
  EXPECT_EQ(journal[0U].from_state, state::MarketState::startup);
  EXPECT_EQ(journal[0U].to_state, state::MarketState::startup);
  EXPECT_NE(controller.configuration_hash(), 0U);

  state::MarketStateSnapshot published{};
  EXPECT_EQ(controller.publisher().read(published), state::ReadStatus::snapshot);
  EXPECT_EQ(published.stable_hash, controller.snapshot().stable_hash);
  EXPECT_TRUE(published.valid());
  const auto publisher_metrics = controller.publisher().metrics();
  EXPECT_EQ(publisher_metrics.publish_count, 1U);
  EXPECT_EQ(publisher_metrics.read_count, 1U);
}

TEST(MarketStateControllerTest, OfficialHaltOverridesEveryInferredCondition) {
  RecordingJournal journal;
  state::MarketStateController controller{config(), journal.sink(), kInitialMonotonicNs,
                                          kInitialWallNs};
  auto input = healthy_input(1U, 101U, 1'000'001U);
  input.official_trading_status = state::OfficialTradingStatus::halted;
  input.clock_quality = state::ClockQuality::unsafe;
  input.feed_health = state::FeedHealth::invalid;
  input.book_validity = state::BookValidity::invalid;
  input.news_event_state = state::NewsEventState::breaking;
  input.realized_volatility_ppm = state::kPartsPerMillion + 1U;
  input.operator_controls.kill_switch_engaged = true;
  const auto result = controller.evaluate(input);
  ASSERT_EQ(result.snapshot.state, state::MarketState::halted);
  EXPECT_EQ(result.snapshot.primary_reason, state::MarketStateReason::official_halt);
  EXPECT_FALSE(result.new_orders_permitted);
}

TEST(MarketStateControllerTest, RankedPriorityUpdatesAuthoritativeReason) {
  RecordingJournal journal;
  state::MarketStateController controller{config(), journal.sink(), kInitialMonotonicNs,
                                          kInitialWallNs};
  std::uint64_t sequence{};
  auto monotonic = kInitialMonotonicNs;
  auto wall = kInitialWallNs;
  reach_normal(controller, sequence, monotonic, wall);

  auto input = healthy_input(++sequence, ++monotonic, ++wall);
  input.clock_quality = state::ClockQuality::unsafe;
  input.feed_health = state::FeedHealth::invalid;
  input.book_validity = state::BookValidity::invalid;
  input.operator_controls.kill_switch_engaged = true;
  ASSERT_EQ(controller.evaluate(input).snapshot.primary_reason,
            state::MarketStateReason::clock_unsafe);

  input = healthy_input(++sequence, ++monotonic, ++wall);
  input.feed_health = state::FeedHealth::invalid;
  input.operator_controls.kill_switch_engaged = true;
  input.news_event_state = state::NewsEventState::breaking;
  ASSERT_EQ(controller.evaluate(input).snapshot.primary_reason,
            state::MarketStateReason::feed_invalid);

  input = healthy_input(++sequence, ++monotonic, ++wall);
  input.operator_controls.kill_switch_engaged = true;
  input.operator_controls.recovery_requested = true;
  input.news_event_state = state::NewsEventState::breaking;
  ASSERT_EQ(controller.evaluate(input).snapshot.primary_reason,
            state::MarketStateReason::kill_switch_engaged);

  input = healthy_input(++sequence, ++monotonic, ++wall);
  input.feed_health = state::FeedHealth::recovering;
  input.news_event_state = state::NewsEventState::breaking;
  ASSERT_EQ(controller.evaluate(input).snapshot.state, state::MarketState::recovery);
  ASSERT_EQ(controller.snapshot().primary_reason,
            state::MarketStateReason::safety_recovery_required);
}

// GoogleTest assertion macros expand to control flow; the test remains a linear
// transition trace despite the analyzer's expanded cognitive-complexity score.
// NOLINTBEGIN(readability-function-cognitive-complexity)
TEST(MarketStateControllerTest, HaltMustPassRecovery) {
  RecordingJournal journal;
  state::MarketStateController controller{config(), journal.sink(), kInitialMonotonicNs,
                                          kInitialWallNs};
  std::uint64_t sequence{};
  auto monotonic = kInitialMonotonicNs;
  auto wall = kInitialWallNs;
  reach_normal(controller, sequence, monotonic, wall);

  auto halted = healthy_input(++sequence, ++monotonic, ++wall);
  halted.official_trading_status = state::OfficialTradingStatus::halted;
  ASSERT_EQ(controller.evaluate(halted).snapshot.state, state::MarketState::halted);
  const auto cleared =
      controller.evaluate(healthy_input(++sequence, ++monotonic, ++wall));
  ASSERT_EQ(cleared.snapshot.state, state::MarketState::recovery);
  EXPECT_NE(cleared.snapshot.state, state::MarketState::normal);

  auto stable = healthy_input(++sequence, ++monotonic, ++wall);
  EXPECT_EQ(controller.evaluate(stable).status,
            state::EvaluationStatus::held_for_dwell);
  monotonic += config().recovery_stabilization_ns;
  wall += config().recovery_stabilization_ns;
  ASSERT_EQ(
      controller.evaluate(healthy_input(++sequence, monotonic, wall)).snapshot.state,
      state::MarketState::normal);

  expect_no_direct_normal_recovery_bypass(journal);
}
// NOLINTEND(readability-function-cognitive-complexity)

TEST(MarketStateControllerTest, InvalidBookMustPassRecovery) {
  RecordingJournal journal;
  state::MarketStateController controller{config(), journal.sink(), kInitialMonotonicNs,
                                          kInitialWallNs};
  std::uint64_t sequence{};
  auto monotonic = kInitialMonotonicNs;
  auto wall = kInitialWallNs;
  reach_normal(controller, sequence, monotonic, wall);

  auto invalid = healthy_input(++sequence, ++monotonic, ++wall);
  invalid.book_validity = state::BookValidity::invalid;
  ASSERT_EQ(controller.evaluate(invalid).snapshot.state,
            state::MarketState::data_degraded);
  const auto recovered =
      controller.evaluate(healthy_input(++sequence, ++monotonic, ++wall));
  EXPECT_EQ(recovered.snapshot.state, state::MarketState::recovery);
  expect_no_direct_normal_recovery_bypass(journal);
}

TEST(MarketStateControllerTest, ReopeningAndRecoveryUseContinuousStabilization) {
  RecordingJournal journal;
  state::MarketStateController controller{config(), journal.sink(), kInitialMonotonicNs,
                                          kInitialWallNs};
  std::uint64_t sequence{};
  auto monotonic = kInitialMonotonicNs;
  auto wall = kInitialWallNs;
  reach_normal(controller, sequence, monotonic, wall);

  auto halt = healthy_input(++sequence, ++monotonic, ++wall);
  halt.official_trading_status = state::OfficialTradingStatus::halted;
  ASSERT_EQ(controller.evaluate(halt).snapshot.state, state::MarketState::halted);
  auto auction = healthy_input(++sequence, ++monotonic, ++wall);
  auction.official_trading_status = state::OfficialTradingStatus::auction;
  ASSERT_EQ(controller.evaluate(auction).snapshot.state, state::MarketState::reopening);

  auto open = healthy_input(++sequence, ++monotonic, ++wall);
  EXPECT_EQ(controller.evaluate(open).status, state::EvaluationStatus::held_for_dwell);
  monotonic += config().reopening_stabilization_ns - 1U;
  wall += config().reopening_stabilization_ns - 1U;
  EXPECT_EQ(controller.evaluate(healthy_input(++sequence, monotonic, wall)).status,
            state::EvaluationStatus::held_for_dwell);

  auto pre_open = healthy_input(++sequence, ++monotonic, ++wall);
  pre_open.official_trading_status = state::OfficialTradingStatus::pre_open;
  const auto pre_open_result = controller.evaluate(pre_open);
  EXPECT_EQ(pre_open_result.status, state::EvaluationStatus::transitioned);
  EXPECT_EQ(pre_open_result.snapshot.primary_reason,
            state::MarketStateReason::reopening_pre_open);
  EXPECT_EQ(controller.snapshot().state, state::MarketState::reopening);

  EXPECT_EQ(controller.evaluate(healthy_input(++sequence, ++monotonic, ++wall)).status,
            state::EvaluationStatus::held_for_dwell);
  monotonic += config().reopening_stabilization_ns;
  wall += config().reopening_stabilization_ns;
  ASSERT_EQ(
      controller.evaluate(healthy_input(++sequence, monotonic, wall)).snapshot.state,
      state::MarketState::recovery);
}

TEST(MarketStateControllerTest, ScheduledWindowActivatesBeforeRelease) {
  RecordingJournal journal;
  state::MarketStateController controller{config(), journal.sink(), kInitialMonotonicNs,
                                          kInitialWallNs};
  std::uint64_t sequence{};
  auto monotonic = kInitialMonotonicNs;
  auto wall = kInitialWallNs;
  reach_normal(controller, sequence, monotonic, wall);

  auto outside = healthy_input(++sequence, ++monotonic, ++wall);
  outside.earnings_calendar = {.state = state::CalendarEventState::scheduled,
                               .release_wall_clock_utc_ns =
                                   wall + config().scheduled_event_lead_ns + 1U};
  EXPECT_EQ(controller.evaluate(outside).status, state::EvaluationStatus::unchanged);

  auto inside = healthy_input(++sequence, ++monotonic, ++wall);
  inside.earnings_calendar = {.state = state::CalendarEventState::scheduled,
                              .release_wall_clock_utc_ns =
                                  wall + config().scheduled_event_lead_ns};
  const auto scheduled = controller.evaluate(inside);
  ASSERT_EQ(scheduled.snapshot.state, state::MarketState::scheduled_event);
  EXPECT_EQ(scheduled.snapshot.primary_reason,
            state::MarketStateReason::earnings_scheduled);

  auto release = healthy_input(++sequence, ++monotonic, ++wall);
  release.earnings_calendar = {.state = state::CalendarEventState::release_active,
                               .release_wall_clock_utc_ns =
                                   inside.earnings_calendar.release_wall_clock_utc_ns};
  EXPECT_EQ(controller.evaluate(release).snapshot.state,
            state::MarketState::event_price_discovery);
}

TEST(MarketStateControllerTest, EventExitUsesDwellThenRecovery) {
  RecordingJournal journal;
  state::MarketStateController controller{config(), journal.sink(), kInitialMonotonicNs,
                                          kInitialWallNs};
  std::uint64_t sequence{};
  auto monotonic = kInitialMonotonicNs;
  auto wall = kInitialWallNs;
  reach_normal(controller, sequence, monotonic, wall);

  auto breaking = healthy_input(++sequence, ++monotonic, ++wall);
  breaking.news_event_state = state::NewsEventState::breaking;
  ASSERT_EQ(controller.evaluate(breaking).snapshot.state,
            state::MarketState::breaking_news);
  auto resolved = healthy_input(++sequence, ++monotonic, ++wall);
  resolved.news_event_state = state::NewsEventState::resolved;
  EXPECT_EQ(controller.evaluate(resolved).status,
            state::EvaluationStatus::held_for_dwell);
  monotonic += config().minimum_dwell_ns;
  wall += config().minimum_dwell_ns;
  resolved = healthy_input(++sequence, monotonic, wall);
  resolved.news_event_state = state::NewsEventState::resolved;
  const auto recovery = controller.evaluate(resolved);
  EXPECT_EQ(recovery.snapshot.state, state::MarketState::recovery);
  EXPECT_EQ(recovery.snapshot.primary_reason, state::MarketStateReason::event_cooldown);
}

TEST(MarketStateControllerTest, StressReasonsAreAggregatedDeterministically) {
  RecordingJournal journal;
  state::MarketStateController controller{config(), journal.sink(), kInitialMonotonicNs,
                                          kInitialWallNs};
  std::uint64_t sequence{};
  auto monotonic = kInitialMonotonicNs;
  auto wall = kInitialWallNs;
  reach_normal(controller, sequence, monotonic, wall);

  auto input = healthy_input(++sequence, ++monotonic, ++wall);
  input.realized_volatility_ppm = config().volatility_spike_threshold_ppm;
  input.spread_ticks = config().spread_spike_threshold_ticks;
  input.aggregate_depth_units = config().minimum_aggregate_depth_units - 1U;
  input.model_ood_ppm = config().model_ood_threshold_ppm;
  input.model_disagreement_ppm = config().model_disagreement_threshold_ppm;
  const auto result = controller.evaluate(input);
  ASSERT_EQ(result.snapshot.state, state::MarketState::volatility_spike);
  EXPECT_EQ(result.snapshot.primary_reason,
            state::MarketStateReason::volatility_threshold);
  for (const auto reason : {state::MarketStateReason::volatility_threshold,
                            state::MarketStateReason::spread_threshold,
                            state::MarketStateReason::depth_threshold,
                            state::MarketStateReason::model_ood_threshold,
                            state::MarketStateReason::model_disagreement_threshold}) {
    EXPECT_NE(result.snapshot.reason_mask & state::reason_bit(reason), 0U);
  }
}

TEST(MarketStateControllerTest, PriorityAndRecoveryReasonMatrixIsDeterministic) {
  using Mutator = void (*)(state::MarketStateInput&) noexcept;
  class TestCase final {
  public:
    constexpr TestCase(const Mutator input_mutator,
                       const state::MarketState state_value,
                       const state::MarketStateReason reason_value) noexcept
        : mutate_(input_mutator), expected_state_(state_value),
          expected_reason_(reason_value) {}

    void apply(state::MarketStateInput& input) const noexcept { mutate_(input); }

    [[nodiscard]] state::MarketState expected_state() const noexcept {
      return expected_state_;
    }

    [[nodiscard]] state::MarketStateReason expected_reason() const noexcept {
      return expected_reason_;
    }

  private:
    Mutator mutate_;
    state::MarketState expected_state_;
    state::MarketStateReason expected_reason_;
  };
  const std::array cases{
      TestCase{[](auto& value) noexcept {
                 value.clock_quality = state::ClockQuality::unsafe;
               },
               state::MarketState::data_degraded,
               state::MarketStateReason::clock_unsafe},
      TestCase{[](auto& value) noexcept {
                 value.clock_quality = state::ClockQuality::unknown;
               },
               state::MarketState::data_degraded,
               state::MarketStateReason::clock_unknown},
      TestCase{[](auto& value) noexcept {
                 value.clock_quality = state::ClockQuality::degraded;
               },
               state::MarketState::data_degraded,
               state::MarketStateReason::clock_degraded},
      TestCase{[](auto& value) noexcept {
                 value.official_trading_status = state::OfficialTradingStatus::unknown;
               },
               state::MarketState::data_degraded,
               state::MarketStateReason::official_status_unknown},
      TestCase{
          [](auto& value) noexcept { value.feed_health = state::FeedHealth::invalid; },
          state::MarketState::data_degraded, state::MarketStateReason::feed_invalid},
      TestCase{
          [](auto& value) noexcept { value.feed_health = state::FeedHealth::stale; },
          state::MarketState::data_degraded, state::MarketStateReason::feed_stale},
      TestCase{
          [](auto& value) noexcept { value.feed_health = state::FeedHealth::unknown; },
          state::MarketState::data_degraded, state::MarketStateReason::feed_unknown},
      TestCase{
          [](auto& value) noexcept { value.feed_health = state::FeedHealth::degraded; },
          state::MarketState::data_degraded, state::MarketStateReason::feed_degraded},
      TestCase{[](auto& value) noexcept {
                 value.book_validity = state::BookValidity::invalid;
               },
               state::MarketState::data_degraded,
               state::MarketStateReason::book_invalid},
      TestCase{[](auto& value) noexcept {
                 value.operator_controls.kill_switch_engaged = true;
               },
               state::MarketState::data_degraded,
               state::MarketStateReason::kill_switch_engaged},
      TestCase{[](auto& value) noexcept {
                 value.feed_health = state::FeedHealth::recovering;
               },
               state::MarketState::recovery, state::MarketStateReason::feed_recovering},
      TestCase{[](auto& value) noexcept {
                 value.clock_quality = state::ClockQuality::syncing;
               },
               state::MarketState::recovery, state::MarketStateReason::clock_syncing},
      TestCase{[](auto& value) noexcept {
                 value.book_validity = state::BookValidity::recovering;
               },
               state::MarketState::recovery, state::MarketStateReason::book_recovering},
      TestCase{[](auto& value) noexcept {
                 value.operator_controls.recovery_requested = true;
               },
               state::MarketState::recovery,
               state::MarketStateReason::operator_recovery},
      TestCase{[](auto& value) noexcept {
                 value.news_event_state = state::NewsEventState::breaking;
               },
               state::MarketState::breaking_news,
               state::MarketStateReason::news_breaking},
      TestCase{[](auto& value) noexcept {
                 value.news_event_state = state::NewsEventState::price_discovery;
               },
               state::MarketState::event_price_discovery,
               state::MarketStateReason::event_price_discovery},
      TestCase{[](auto& value) noexcept {
                 value.official_trading_status = state::OfficialTradingStatus::pre_open;
               },
               state::MarketState::reopening,
               state::MarketStateReason::reopening_pre_open},
      TestCase{[](auto& value) noexcept {
                 value.official_trading_status = state::OfficialTradingStatus::auction;
               },
               state::MarketState::reopening,
               state::MarketStateReason::reopening_auction},
      TestCase{[](auto& value) noexcept {
                 value.official_trading_status = state::OfficialTradingStatus::closed;
               },
               state::MarketState::halted,
               state::MarketStateReason::official_market_closed},
      TestCase{[](auto& value) noexcept {
                 value.macro_calendar = {.state = state::CalendarEventState::scheduled,
                                         .release_wall_clock_utc_ns =
                                             value.wall_clock_utc_ns + 50U};
               },
               state::MarketState::scheduled_event,
               state::MarketStateReason::macro_scheduled},
      TestCase{[](auto& value) noexcept {
                 value.earnings_calendar = {
                     .state = state::CalendarEventState::scheduled,
                     .release_wall_clock_utc_ns = value.wall_clock_utc_ns + 50U};
                 value.macro_calendar = value.earnings_calendar;
               },
               state::MarketState::scheduled_event,
               state::MarketStateReason::earnings_and_macro_scheduled},
      TestCase{[](auto& value) noexcept {
                 value.macro_calendar = {.state = state::CalendarEventState::scheduled,
                                         .release_wall_clock_utc_ns =
                                             value.wall_clock_utc_ns - 1U};
               },
               state::MarketState::data_degraded,
               state::MarketStateReason::scheduled_release_overdue},
      TestCase{[](auto& value) noexcept {
                 value.spread_ticks = config().spread_spike_threshold_ticks;
               },
               state::MarketState::volatility_spike,
               state::MarketStateReason::spread_threshold},
      TestCase{[](auto& value) noexcept {
                 value.aggregate_depth_units =
                     config().minimum_aggregate_depth_units - 1U;
               },
               state::MarketState::volatility_spike,
               state::MarketStateReason::depth_threshold},
      TestCase{[](auto& value) noexcept {
                 value.model_ood_ppm = config().model_ood_threshold_ppm;
               },
               state::MarketState::volatility_spike,
               state::MarketStateReason::model_ood_threshold},
      TestCase{[](auto& value) noexcept {
                 value.model_disagreement_ppm =
                     config().model_disagreement_threshold_ppm;
               },
               state::MarketState::volatility_spike,
               state::MarketStateReason::model_disagreement_threshold},
  };

  for (const auto& test : cases) {
    RecordingJournal journal;
    state::MarketStateController controller{config(), journal.sink(),
                                            kInitialMonotonicNs, kInitialWallNs};
    std::uint64_t sequence{};
    auto monotonic = kInitialMonotonicNs;
    auto wall = kInitialWallNs;
    reach_normal(controller, sequence, monotonic, wall);
    auto input = healthy_input(++sequence, ++monotonic, ++wall);
    test.apply(input);
    const auto result = controller.evaluate(input);
    EXPECT_EQ(result.snapshot.state, test.expected_state());
    EXPECT_EQ(result.snapshot.primary_reason, test.expected_reason());
  }
}

TEST(MarketStateControllerTest, MalformedAndUnorderedInputsFailClosed) {
  RecordingJournal journal;
  state::MarketStateController controller{config(), journal.sink(), kInitialMonotonicNs,
                                          kInitialWallNs};
  auto malformed = healthy_input(1U, 101U, 1'000'001U);
  malformed.model_ood_ppm = state::kPartsPerMillion + 1U;
  auto result = controller.evaluate(malformed);
  ASSERT_EQ(result.snapshot.state, state::MarketState::data_degraded);
  EXPECT_EQ(result.snapshot.primary_reason, state::MarketStateReason::malformed_input);

  auto unordered = healthy_input(1U, 102U, 1'000'002U);
  result = controller.evaluate(unordered);
  EXPECT_EQ(result.snapshot.state, state::MarketState::data_degraded);
  EXPECT_FALSE(result.new_orders_permitted);
  EXPECT_GE(controller.metrics().invalid_input_count, 2U);

  auto unordered_halt = healthy_input(1U, 103U, 1'000'003U);
  unordered_halt.official_trading_status = state::OfficialTradingStatus::halted;
  unordered_halt.model_ood_ppm = state::kPartsPerMillion + 1U;
  result = controller.evaluate(unordered_halt);
  EXPECT_EQ(result.snapshot.state, state::MarketState::halted);
  EXPECT_EQ(result.snapshot.primary_reason, state::MarketStateReason::official_halt);
  EXPECT_FALSE(result.new_orders_permitted);

  auto invalid_enum = healthy_input(2U, 104U, 1'000'004U);
  invalid_enum.feed_health = std::bit_cast<state::FeedHealth>(std::uint8_t{255U});
  result = controller.evaluate(invalid_enum);
  EXPECT_EQ(result.snapshot.state, state::MarketState::data_degraded);
  EXPECT_EQ(result.snapshot.primary_reason, state::MarketStateReason::malformed_input);

  auto monotonic_regression = healthy_input(3U, 103U, 1'000'005U);
  result = controller.evaluate(monotonic_regression);
  EXPECT_EQ(result.snapshot.state, state::MarketState::data_degraded);
  EXPECT_EQ(result.snapshot.primary_reason,
            state::MarketStateReason::input_ordering_invalid);
}

TEST(MarketStateControllerTest, JournalBackpressurePreventsPublicationAndFailsClosed) {
  RecordingJournal journal;
  state::MarketStateController controller{config(), journal.sink(), kInitialMonotonicNs,
                                          kInitialWallNs};
  std::uint64_t sequence{};
  auto monotonic = kInitialMonotonicNs;
  auto wall = kInitialWallNs;
  reach_normal(controller, sequence, monotonic, wall);
  const auto prior = controller.snapshot();
  const auto records_before = journal.size();
  journal.reject(state::JournalAppendStatus::full);

  auto halt = healthy_input(++sequence, ++monotonic, ++wall);
  halt.official_trading_status = state::OfficialTradingStatus::halted;
  const auto result = controller.evaluate(halt);
  EXPECT_EQ(result.status, state::EvaluationStatus::journal_rejected);
  EXPECT_FALSE(result.new_orders_permitted);
  EXPECT_EQ(controller.snapshot().stable_hash, prior.stable_hash);
  EXPECT_EQ(journal.size(), records_before);
  EXPECT_EQ(controller.metrics().journal_rejection_count, 1U);
  state::MarketStateSnapshot unavailable{};
  EXPECT_EQ(controller.publisher().read(unavailable), state::ReadStatus::invalidated);

  journal.reject(state::JournalAppendStatus::accepted);
  const auto retry = controller.evaluate(halt);
  EXPECT_EQ(retry.status, state::EvaluationStatus::transitioned);
  EXPECT_EQ(retry.snapshot.state, state::MarketState::halted);
  EXPECT_EQ(controller.publisher().read(unavailable), state::ReadStatus::snapshot);
  EXPECT_EQ(controller.publisher().metrics().invalidation_count, 1U);
}

TEST(MarketStateControllerTest, ShutdownIsTerminal) {
  RecordingJournal journal;
  state::MarketStateController controller{config(), journal.sink(), kInitialMonotonicNs,
                                          kInitialWallNs};
  auto shutdown = healthy_input(1U, 101U, 1'000'001U);
  shutdown.operator_controls.shutdown_requested = true;
  ASSERT_EQ(controller.evaluate(shutdown).snapshot.state, state::MarketState::shutdown);
  auto halt = healthy_input(2U, 102U, 1'000'002U);
  halt.official_trading_status = state::OfficialTradingStatus::halted;
  EXPECT_EQ(controller.evaluate(halt).status,
            state::EvaluationStatus::shutdown_terminal);
  EXPECT_EQ(controller.snapshot().state, state::MarketState::shutdown);
}

TEST(AtomicMarketStatePublisherTest, EmptyAndInvalidSnapshotsAreRejected) {
  state::AtomicMarketStatePublisher publisher;
  state::MarketStateSnapshot output{};
  EXPECT_EQ(publisher.read(output), state::ReadStatus::empty);
  EXPECT_EQ(publisher.publish(output), state::PublishStatus::invalid_snapshot);

  auto invalid_reason = state::MarketStateSnapshot{
      .state = state::MarketState::normal,
      .primary_reason = std::bit_cast<state::MarketStateReason>(std::uint8_t{255U}),
      .reason_mask = std::uint64_t{1U} << 63U,
      .input_sequence = 1U,
      .transition_sequence = 1U,
      .observed_process_monotonic_time_ns = 1U,
      .observed_wall_clock_utc_ns = 1U,
      .state_entered_process_monotonic_time_ns = 1U};
  invalid_reason.stable_hash = state::stable_market_state_hash(invalid_reason);
  EXPECT_EQ(publisher.publish(invalid_reason), state::PublishStatus::invalid_snapshot);
}

} // namespace
