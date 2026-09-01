#include "aegis/market_state/controller.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace state = aegis::market_state;

namespace {

inline constexpr std::uint64_t kDeterministicSeed = 20'260'828U;
inline constexpr std::size_t kPropertyIterations = 10'000U;

class SplitMix64 final {
public:
  explicit SplitMix64(const std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] std::uint64_t next() noexcept {
    auto value = (state_ += 0x9e37'79b9'7f4a'7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58'476d'1ce4'e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d0'49bb'1331'11ebULL;
    return value ^ (value >> 31U);
  }

private:
  std::uint64_t state_;
};

class PropertyJournal final {
public:
  [[nodiscard]] static state::JournalAppendStatus
  append(void* context, const state::MarketStateTransition& transition) noexcept {
    auto& journal = *static_cast<PropertyJournal*>(context);
    if (!transition.valid()) {
      journal.invalid_.store(true, std::memory_order_relaxed);
      return state::JournalAppendStatus::stopped;
    }
    if ((transition.from_state == state::MarketState::halted ||
         transition.from_state == state::MarketState::data_degraded) &&
        transition.to_state == state::MarketState::normal) {
      journal.forbidden_.store(true, std::memory_order_relaxed);
      return state::JournalAppendStatus::stopped;
    }
    journal.count_.fetch_add(1U, std::memory_order_relaxed);
    return state::JournalAppendStatus::accepted;
  }

  [[nodiscard]] state::TransitionJournalSink sink() noexcept {
    return {this, &PropertyJournal::append};
  }

  [[nodiscard]] bool valid() const noexcept {
    return !invalid_.load(std::memory_order_relaxed) &&
           !forbidden_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<std::uint64_t> count_{0U};
  std::atomic<bool> invalid_{false};
  std::atomic<bool> forbidden_{false};
};

[[nodiscard]] constexpr state::MarketStateConfig config() noexcept {
  return {.minimum_dwell_ns = 3U,
          .recovery_stabilization_ns = 5U,
          .reopening_stabilization_ns = 7U,
          .scheduled_event_lead_ns = 11U,
          .volatility_spike_threshold_ppm = 200'000U,
          .spread_spike_threshold_ticks = 10U,
          .minimum_aggregate_depth_units = 1'000U,
          .model_ood_threshold_ppm = 800'000U,
          .model_disagreement_threshold_ppm = 700'000U};
}

[[nodiscard]] constexpr state::MarketStateInput
base_input(const std::uint64_t sequence, const std::uint64_t time) noexcept {
  return {.input_sequence = sequence,
          .process_monotonic_time_ns = time,
          .wall_clock_utc_ns = 1'000'000U + time,
          .official_trading_status = state::OfficialTradingStatus::open,
          .feed_health = state::FeedHealth::healthy,
          .book_validity = state::BookValidity::valid,
          .clock_quality = state::ClockQuality::healthy,
          .news_event_state = state::NewsEventState::none,
          .earnings_calendar = {},
          .macro_calendar = {},
          .realized_volatility_ppm = 1U,
          .spread_ticks = 1U,
          .aggregate_depth_units = 10'000U,
          .model_ood_ppm = 1U,
          .model_disagreement_ppm = 1U,
          .operator_controls = {}};
}

[[nodiscard]] bool expected_transition(const state::MarketState from,
                                       const state::MarketState to) noexcept {
  if (from == to) {
    return true;
  }
  if (from == state::MarketState::shutdown) {
    return false;
  }
  if (to == state::MarketState::halted || to == state::MarketState::data_degraded ||
      to == state::MarketState::shutdown) {
    return true;
  }
  switch (from) {
  case state::MarketState::startup:
  case state::MarketState::halted:
  case state::MarketState::data_degraded:
    return to == state::MarketState::reopening || to == state::MarketState::recovery;
  case state::MarketState::reopening:
    return to == state::MarketState::recovery;
  case state::MarketState::recovery:
    return to == state::MarketState::normal ||
           to == state::MarketState::scheduled_event ||
           to == state::MarketState::breaking_news ||
           to == state::MarketState::event_price_discovery ||
           to == state::MarketState::volatility_spike ||
           to == state::MarketState::reopening;
  case state::MarketState::normal:
  case state::MarketState::scheduled_event:
  case state::MarketState::breaking_news:
  case state::MarketState::event_price_discovery:
  case state::MarketState::volatility_spike:
    return to == state::MarketState::normal ||
           to == state::MarketState::scheduled_event ||
           to == state::MarketState::breaking_news ||
           to == state::MarketState::event_price_discovery ||
           to == state::MarketState::volatility_spike ||
           to == state::MarketState::reopening || to == state::MarketState::recovery;
  case state::MarketState::shutdown:
    return false;
  }
  return false;
}

void read_until_stopped(const state::AtomicMarketStatePublisher& publisher,
                        const std::atomic<bool>& stop,
                        std::atomic<bool>& invalid) noexcept {
  while (!stop.load(std::memory_order_acquire)) {
    state::MarketStateSnapshot value{};
    const auto status = publisher.read(value);
    if (status == state::ReadStatus::snapshot && !value.valid()) {
      invalid.store(true, std::memory_order_release);
    }
    if (status == state::ReadStatus::corrupt) {
      invalid.store(true, std::memory_order_release);
    }
  }
}

void publish_test_snapshots(state::AtomicMarketStatePublisher& publisher,
                            const state::MarketStateSnapshot& initial) {
  for (std::uint64_t sequence = 2U; sequence <= 20'000U; ++sequence) {
    auto next = initial;
    next.input_sequence = sequence;
    next.transition_sequence = sequence;
    next.observed_process_monotonic_time_ns = sequence;
    next.observed_wall_clock_utc_ns = 1'000'000U + sequence;
    next.stable_hash = state::stable_market_state_hash(next);
    ASSERT_EQ(publisher.publish(next), state::PublishStatus::published);
  }
}

TEST(MarketStatePropertyTest, EntireTransitionMatrixMatchesPublishedTable) {
  for (std::size_t from = 0U; from < state::kMarketStateCount; ++from) {
    for (std::size_t to = 0U; to < state::kMarketStateCount; ++to) {
      const auto from_state = static_cast<state::MarketState>(from);
      const auto to_state = static_cast<state::MarketState>(to);
      EXPECT_EQ(state::transition_permitted(from_state, to_state),
                expected_transition(from_state, to_state))
          << "from=" << from << " to=" << to;
    }
  }
  EXPECT_FALSE(state::transition_permitted(static_cast<state::MarketState>(255U),
                                           state::MarketState::normal));
  EXPECT_FALSE(state::transition_permitted(state::MarketState::normal,
                                           static_cast<state::MarketState>(255U)));
}

// Deterministic property generation is deliberately kept in one bounded loop.
// NOLINTBEGIN(readability-function-cognitive-complexity)
TEST(MarketStatePropertyTest, RandomizedInputsNeverPublishForbiddenTransitions) {
  PropertyJournal journal;
  state::MarketStateController controller{config(), journal.sink(), 1U, 1'000'001U};
  ASSERT_TRUE(controller.initialized());
  SplitMix64 random{kDeterministicSeed};
  auto time = std::uint64_t{1U};
  for (std::size_t index = 1U; index <= kPropertyIterations; ++index) {
    time += 1U + (random.next() % 3U);
    auto input = base_input(index, time);
    input.official_trading_status =
        static_cast<state::OfficialTradingStatus>(random.next() % 6U);
    input.feed_health = static_cast<state::FeedHealth>(random.next() % 6U);
    input.book_validity = static_cast<state::BookValidity>(random.next() % 3U);
    input.clock_quality = static_cast<state::ClockQuality>(random.next() % 5U);
    input.news_event_state = static_cast<state::NewsEventState>(random.next() % 4U);
    input.realized_volatility_ppm =
        static_cast<std::uint32_t>(random.next() % (state::kPartsPerMillion + 1U));
    input.spread_ticks = random.next() % 20U;
    input.aggregate_depth_units = random.next() % 20'000U;
    input.model_ood_ppm =
        static_cast<std::uint32_t>(random.next() % (state::kPartsPerMillion + 1U));
    input.model_disagreement_ppm =
        static_cast<std::uint32_t>(random.next() % (state::kPartsPerMillion + 1U));
    input.operator_controls.kill_switch_engaged = (random.next() % 17U) == 0U;
    input.operator_controls.recovery_requested = (random.next() % 19U) == 0U;

    const auto prior = controller.snapshot();
    const auto result = controller.evaluate(input);
    if (result.transitioned) {
      EXPECT_TRUE(state::transition_permitted(prior.state, result.snapshot.state))
          << "seed=" << kDeterministicSeed << " iteration=" << index;
      EXPECT_FALSE(prior.state == state::MarketState::halted &&
                   result.snapshot.state == state::MarketState::normal);
      EXPECT_FALSE(prior.state == state::MarketState::data_degraded &&
                   result.snapshot.state == state::MarketState::normal);
    }
    ASSERT_NE(result.status, state::EvaluationStatus::journal_rejected);
  }
  EXPECT_TRUE(journal.valid());
}
// NOLINTEND(readability-function-cognitive-complexity)

TEST(MarketStatePropertyTest, AtomicReadersNeverObserveTornSnapshots) {
  state::AtomicMarketStatePublisher publisher;
  auto initial = state::MarketStateSnapshot{
      .state = state::MarketState::normal,
      .primary_reason = state::MarketStateReason::normal_conditions_stable,
      .reason_mask =
          state::reason_bit(state::MarketStateReason::normal_conditions_stable),
      .input_sequence = 1U,
      .transition_sequence = 1U,
      .observed_process_monotonic_time_ns = 1U,
      .observed_wall_clock_utc_ns = 1'000'001U,
      .state_entered_process_monotonic_time_ns = 1U};
  initial.stable_hash = state::stable_market_state_hash(initial);
  ASSERT_EQ(publisher.publish(initial), state::PublishStatus::published);

  std::atomic<bool> stop{false};
  std::atomic<bool> invalid{false};
  std::array<std::thread, 3U> readers;
  for (auto& reader : readers) {
    reader = std::thread([&publisher, &stop, &invalid]() {
      read_until_stopped(publisher, stop, invalid);
    });
  }
  publish_test_snapshots(publisher, initial);
  stop.store(true, std::memory_order_release);
  for (auto& reader : readers) {
    reader.join();
  }
  EXPECT_FALSE(invalid.load(std::memory_order_acquire));
  EXPECT_EQ(publisher.metrics().publish_count, 20'000U);
}

} // namespace
