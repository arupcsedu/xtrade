#include "aegis/market_data/feed/bounded_queue.hpp"
#include "aegis/market_data/feed/feed_handler.hpp"
#include "aegis/market_data/feed/licensed_adapter.hpp"
#include "aegis/market_data/feed/synthetic_receiver.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace feed = aegis::market_data::feed;
namespace synthetic = aegis::market_data::synthetic;

namespace {

constexpr aegis::common::SessionId kSession{0x534D'5853'4553'534EU, 0x101U};
constexpr feed::FeedIdentity kIdentity{
    .session_id = kSession,
    .venue_id = aegis::common::VenueId{0x5645'4E55'4500'0001U, 1U},
    .channel_id = aegis::common::ChannelId{0x4348'414E'4E45'4C01U, 7U},
    .venue_number = 1U,
    .channel_number = 7U,
};

[[nodiscard]] synthetic::SyntheticEvent make_event(const std::uint64_t sequence,
                                                   const std::uint64_t ordinal) {
  synthetic::SyntheticEvent event{
      .type = synthetic::NativeMessageType::price_level,
      .side = synthetic::Side::bid,
      .action = synthetic::BookAction::add,
      .status = synthetic::TradingStatus::none,
      .data_quality = synthetic::DataQuality::valid,
      .message_flags = synthetic::kMessageFlagPriceLevel,
      .packet_flags = 0U,
      .venue_number = kIdentity.venue_number,
      .channel_number = kIdentity.channel_number,
      .instrument_number = 11U,
      .channel_sequence = sequence,
      .global_ordinal = ordinal,
      .exchange_event_time_ns =
          1'800'000'000'000'000'000LL + static_cast<std::int64_t>(ordinal),
      .nic_receive_time_ns =
          1'800'000'000'000'000'100LL + static_cast<std::int64_t>(ordinal),
      .process_monotonic_time_ns = 1'000U + ordinal,
      .synthetic_order_id = 0U,
      .price_ticks = 10'000,
      .quantity_units = 0U,
      .level_quantity_units = 100U + ordinal,
      .order_count = 1U,
      .auxiliary_code = 0U,
      .auxiliary_value = 0U,
      .event_hash = 0U,
  };
  event.event_hash = synthetic::calculate_event_hash(event);
  return event;
}

[[nodiscard]] feed::FeedHandlerConfig make_config() {
  return {
      .identity = kIdentity,
      .initial_sequence = 1U,
      .maximum_sequence = 1'000U,
      .stale_after_ns = 100U,
      .recovery_timeout_ns = 10U,
      .arbitration_window = 8U,
  };
}

class TestJournal final : public feed::RawPacketJournal {
public:
  explicit TestJournal(const std::size_t capacity = 64U) : capacity_(capacity) {}

  [[nodiscard]] feed::PublishStatus
  enqueue(const feed::RawPacket& packet) noexcept override {
    if (count_ >= capacity_) {
      return feed::PublishStatus::full;
    }
    entries_[count_] = packet;
    ++count_;
    return feed::PublishStatus::accepted;
  }

  [[nodiscard]] std::size_t count() const noexcept { return count_; }

private:
  std::array<feed::RawPacket, 64U> entries_{};
  std::size_t capacity_{};
  std::size_t count_{};
};

class TestHealthPublisher final : public feed::FeedHealthPublisher {
public:
  explicit TestHealthPublisher(const std::size_t capacity = 128U)
      : capacity_(capacity) {}

  [[nodiscard]] feed::PublishStatus
  publish(const feed::DataQualityState& state) noexcept override {
    if (count_ >= capacity_) {
      return feed::PublishStatus::full;
    }
    entries_[count_] = state;
    ++count_;
    return feed::PublishStatus::accepted;
  }

  [[nodiscard]] std::size_t count() const noexcept { return count_; }
  [[nodiscard]] const feed::DataQualityState& at(const std::size_t index) const {
    return entries_[index];
  }

private:
  std::array<feed::DataQualityState, 128U> entries_{};
  std::size_t capacity_{};
  std::size_t count_{};
};

class TestNormalizedPublisher final : public feed::NormalizedEventPublisher {
public:
  explicit TestNormalizedPublisher(const std::size_t capacity = 128U)
      : capacity_(capacity) {}

  [[nodiscard]] feed::PublishStatus
  publish(const feed::NormalizedEvent& event) noexcept override {
    if (count_ >= capacity_) {
      return feed::PublishStatus::full;
    }
    events_[count_] = event;
    ++count_;
    return feed::PublishStatus::accepted;
  }

  [[nodiscard]] feed::PublishStatus
  publish_snapshot(const feed::RecoverySnapshot& snapshot) noexcept override {
    if (snapshot_count_ >= snapshots_.size()) {
      return feed::PublishStatus::full;
    }
    snapshots_[snapshot_count_] = snapshot;
    ++snapshot_count_;
    return feed::PublishStatus::accepted;
  }

  [[nodiscard]] std::size_t count() const noexcept { return count_; }
  [[nodiscard]] std::size_t snapshot_count() const noexcept { return snapshot_count_; }
  [[nodiscard]] const feed::NormalizedEvent& at(const std::size_t index) const {
    return events_[index];
  }

private:
  std::array<feed::NormalizedEvent, 128U> events_{};
  std::array<feed::RecoverySnapshot, 4U> snapshots_{};
  std::size_t capacity_{};
  std::size_t count_{};
  std::size_t snapshot_count_{};
};

// Test fixtures intentionally expose state for direct assertions.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes,
// bugprone-easily-swappable-parameters)
class DelayedRecovery final : public feed::RecoveryProvider {
public:
  feed::RecoverySnapshot snapshot{};
  bool retransmission_available{true};
  bool snapshot_available{true};
  std::uint64_t retransmission_count{};
  std::uint64_t snapshot_count{};

  [[nodiscard]] feed::RecoveryRequestStatus
  request_retransmission(const feed::FeedIdentity& identity,
                         const feed::SequenceRange& range) noexcept override {
    static_cast<void>(identity);
    static_cast<void>(range);
    ++retransmission_count;
    return retransmission_available ? feed::RecoveryRequestStatus::accepted
                                    : feed::RecoveryRequestStatus::unavailable;
  }

  [[nodiscard]] feed::RecoveryRequestStatus
  request_snapshot(const feed::FeedIdentity& identity) noexcept override {
    static_cast<void>(identity);
    ++snapshot_count;
    snapshot_pending_ = snapshot_available;
    return snapshot_available ? feed::RecoveryRequestStatus::accepted
                              : feed::RecoveryRequestStatus::unavailable;
  }

  [[nodiscard]] feed::ReceiveStatus
  poll_snapshot(feed::RecoverySnapshot& output) noexcept override {
    if (!snapshot_pending_) {
      return feed::ReceiveStatus::empty;
    }
    output = snapshot;
    snapshot_pending_ = false;
    return feed::ReceiveStatus::packet;
  }

private:
  bool snapshot_pending_{false};
};

[[nodiscard]] feed::RecoverySnapshot make_snapshot(const std::uint64_t sequence,
                                                   const std::uint64_t observed) {
  feed::RecoverySnapshot snapshot{
      .identity = kIdentity,
      .last_sequence = sequence,
      .observed_process_monotonic_time_ns = observed,
      .books = {},
      .book_hash = 0U,
  };
  static_cast<void>(snapshot.books.register_instrument(kIdentity.venue_number, 11U));
  snapshot.book_hash = snapshot.books.stable_hash();
  return snapshot;
}

class HandlerHarness final {
public:
  explicit HandlerHarness(const std::size_t receiver_capacity = 16U,
                          const std::size_t journal_capacity = 64U,
                          const std::size_t normalized_capacity = 128U,
                          const std::size_t health_capacity = 128U,
                          feed::FeedHandlerConfig config = make_config())
      : config_(config), receiver(kIdentity, receiver_capacity, 32U),
        decoder(kIdentity), journal(journal_capacity), normalized(normalized_capacity),
        health(health_capacity),
        handler(config_, receiver, decoder, receiver, journal, normalized, health) {}

  [[nodiscard]] bool enqueue(const std::uint64_t sequence, const std::uint64_t ordinal,
                             const feed::FeedLeg leg, const std::uint64_t received,
                             const bool retain = true) {
    return receiver.enqueue(feed::make_synthetic_raw_packet(
                                make_event(sequence, ordinal), kSession, leg, received),
                            retain);
  }

  feed::FeedHandlerConfig config_;
  feed::SyntheticReceiver receiver;
  feed::SyntheticDecoder decoder;
  TestJournal journal;
  TestNormalizedPublisher normalized;
  TestHealthPublisher health;
  feed::FeedHandler handler;
};
// NOLINTEND(misc-non-private-member-variables-in-classes,
// bugprone-easily-swappable-parameters)

static_assert(std::is_trivially_copyable_v<feed::RawPacket>);
static_assert(std::is_trivially_copyable_v<feed::DataQualityState>);

} // namespace

// GoogleTest assertion macros expand into control flow that clang-tidy counts as
// test-body complexity.
// NOLINTBEGIN(readability-function-cognitive-complexity)
TEST(SequenceTrackerTest, SupportsConfiguredWrapAndRejectsOldSequence) {
  feed::SequenceTracker tracker{3U, 3U};
  EXPECT_EQ(tracker.classify(3U), feed::SequenceDisposition::next);
  EXPECT_TRUE(tracker.commit(3U));
  EXPECT_EQ(tracker.expected(), 1U);
  EXPECT_TRUE(tracker.commit(1U));
  EXPECT_EQ(tracker.expected(), 2U);
  EXPECT_EQ(tracker.classify(1U), feed::SequenceDisposition::duplicate);
}

TEST(FeedArbitratorTest, ReconcilesMatchingCopiesAndRejectsConflict) {
  feed::FeedAFeedBArbitrator arbitrator{1U, 100U, 8U};
  const auto first = make_event(1U, 1U);
  EXPECT_EQ(arbitrator.ingest({.event = first, .feed_leg = feed::FeedLeg::a}).status,
            feed::ArbitrationStatus::ready);
  feed::ArbitratedEvent output{};
  ASSERT_TRUE(arbitrator.pop_ready(output));
  EXPECT_EQ(output.event.event_hash, first.event_hash);
  EXPECT_EQ(arbitrator.ingest({.event = first, .feed_leg = feed::FeedLeg::b}).status,
            feed::ArbitrationStatus::duplicate);

  auto conflict = first;
  ++conflict.level_quantity_units;
  conflict.event_hash = synthetic::calculate_event_hash(conflict);
  EXPECT_EQ(arbitrator.ingest({.event = conflict, .feed_leg = feed::FeedLeg::b}).status,
            feed::ArbitrationStatus::conflicting_copy);
}

TEST(FeedHandlerTest, PublishesContinuousPacketsOnceAcrossAAndB) {
  auto harness = std::make_unique<HandlerHarness>();
  ASSERT_TRUE(harness->handler.start(100U));
  ASSERT_TRUE(harness->enqueue(1U, 1U, feed::FeedLeg::a, 101U));
  ASSERT_TRUE(harness->enqueue(1U, 1U, feed::FeedLeg::b, 102U));
  EXPECT_EQ(harness->handler.poll_once(101U), feed::PollResult::processed);
  EXPECT_EQ(harness->handler.state(), feed::FeedState::healthy);
  EXPECT_EQ(harness->handler.poll_once(102U), feed::PollResult::processed);
  EXPECT_EQ(harness->normalized.count(), 1U);
  EXPECT_EQ(harness->handler.metrics().duplicate_packets, 1U);
  EXPECT_EQ(harness->normalized.at(0U).data_quality.quality,
            feed::DataQualityCode::valid);
  EXPECT_EQ(harness->normalized.at(0U).data_quality.last_good_exchange_event_time_ns,
            make_event(1U, 1U).exchange_event_time_ns);
}

TEST(FeedHandlerTest, PreservesContinuityAcrossConfiguredSequenceWrap) {
  auto config = make_config();
  config.initial_sequence = 3U;
  config.maximum_sequence = 3U;
  auto harness = std::make_unique<HandlerHarness>(16U, 64U, 128U, 128U, config);
  ASSERT_TRUE(harness->enqueue(3U, 1U, feed::FeedLeg::a, 101U));
  ASSERT_TRUE(harness->enqueue(1U, 2U, feed::FeedLeg::a, 102U));
  ASSERT_TRUE(harness->handler.start(100U));
  EXPECT_EQ(harness->handler.poll_once(101U), feed::PollResult::processed);
  EXPECT_EQ(harness->handler.poll_once(102U), feed::PollResult::processed);
  ASSERT_EQ(harness->normalized.count(), 2U);
  EXPECT_EQ(harness->normalized.at(0U).event.channel_sequence, 3U);
  EXPECT_EQ(harness->normalized.at(1U).event.channel_sequence, 1U);
  EXPECT_EQ(harness->handler.state(), feed::FeedState::healthy);
}

TEST(FeedHandlerTest, OutOfOrderAndConflictingCopiesFailClosed) {
  {
    auto config = make_config();
    config.initial_sequence = 2U;
    auto harness = std::make_unique<HandlerHarness>(16U, 64U, 128U, 128U, config);
    ASSERT_TRUE(harness->enqueue(1U, 1U, feed::FeedLeg::a, 101U));
    ASSERT_TRUE(harness->handler.start(100U));
    EXPECT_EQ(harness->handler.poll_once(101U), feed::PollResult::invalid);
    EXPECT_EQ(harness->handler.metrics().out_of_order_packets, 1U);
    EXPECT_EQ(harness->handler.metrics().redundant_leg_out_of_order, 1U);
    EXPECT_EQ(harness->normalized.count(), 0U);
  }
  {
    auto harness = std::make_unique<HandlerHarness>();
    const auto first = make_event(1U, 1U);
    auto conflict = first;
    ++conflict.level_quantity_units;
    conflict.event_hash = synthetic::calculate_event_hash(conflict);
    ASSERT_TRUE(harness->receiver.enqueue(
        feed::make_synthetic_raw_packet(first, kSession, feed::FeedLeg::a, 101U)));
    ASSERT_TRUE(harness->receiver.enqueue(
        feed::make_synthetic_raw_packet(conflict, kSession, feed::FeedLeg::b, 102U)));
    ASSERT_TRUE(harness->handler.start(100U));
    EXPECT_EQ(harness->handler.poll_once(101U), feed::PollResult::processed);
    EXPECT_EQ(harness->handler.poll_once(102U), feed::PollResult::invalid);
    EXPECT_EQ(harness->handler.state(), feed::FeedState::invalid);
    EXPECT_EQ(harness->normalized.count(), 1U);
  }
}

TEST(FeedHandlerTest, RecoversGapBeforeForwardingBufferedEvent) {
  auto harness = std::make_unique<HandlerHarness>();
  const auto missing = feed::make_synthetic_raw_packet(make_event(2U, 2U), kSession,
                                                       feed::FeedLeg::a, 102U);
  ASSERT_TRUE(harness->receiver.prime_history(missing));
  ASSERT_TRUE(harness->enqueue(1U, 1U, feed::FeedLeg::a, 101U));
  ASSERT_TRUE(harness->enqueue(3U, 3U, feed::FeedLeg::a, 103U));
  ASSERT_TRUE(harness->handler.start(100U));
  EXPECT_EQ(harness->handler.poll_once(101U), feed::PollResult::processed);
  EXPECT_EQ(harness->handler.poll_once(103U), feed::PollResult::processed);
  EXPECT_EQ(harness->handler.state(), feed::FeedState::replaying_gap);
  EXPECT_EQ(harness->normalized.count(), 1U);
  EXPECT_EQ(harness->handler.poll_once(104U), feed::PollResult::processed);
  ASSERT_EQ(harness->normalized.count(), 3U);
  EXPECT_EQ(harness->normalized.at(1U).event.channel_sequence, 2U);
  EXPECT_TRUE(harness->normalized.at(1U).recovered);
  EXPECT_EQ(harness->normalized.at(1U).data_quality.quality,
            feed::DataQualityCode::degraded);
  EXPECT_EQ(harness->normalized.at(2U).event.channel_sequence, 3U);
  EXPECT_EQ(harness->handler.state(), feed::FeedState::healthy);
  EXPECT_EQ(harness->handler.metrics().canonical_gaps, 1U);
  EXPECT_EQ(harness->handler.metrics().missing_sequences, 1U);
}

TEST(FeedHandlerTest, SimultaneousABGapFallsBackToValidatedSnapshot) {
  auto harness = std::make_unique<HandlerHarness>();
  ASSERT_TRUE(harness->receiver.install_snapshot(make_snapshot(2U, 103U)));
  ASSERT_TRUE(harness->enqueue(1U, 1U, feed::FeedLeg::a, 101U));
  ASSERT_TRUE(harness->enqueue(1U, 1U, feed::FeedLeg::b, 101U));
  ASSERT_TRUE(harness->enqueue(3U, 3U, feed::FeedLeg::a, 103U, false));
  ASSERT_TRUE(harness->enqueue(3U, 3U, feed::FeedLeg::b, 103U, false));
  ASSERT_TRUE(harness->handler.start(100U));
  EXPECT_EQ(harness->handler.poll_once(101U), feed::PollResult::processed);
  EXPECT_EQ(harness->handler.poll_once(102U), feed::PollResult::processed);
  EXPECT_EQ(harness->handler.poll_once(103U), feed::PollResult::processed);
  EXPECT_EQ(harness->handler.state(), feed::FeedState::recovering);
  EXPECT_EQ(harness->handler.poll_once(104U), feed::PollResult::processed);
  EXPECT_EQ(harness->normalized.snapshot_count(), 1U);
  EXPECT_EQ(harness->handler.state(), feed::FeedState::healthy);
  EXPECT_EQ(harness->handler.poll_once(105U), feed::PollResult::processed);
  EXPECT_EQ(harness->normalized.at(1U).event.channel_sequence, 3U);
}

TEST(FeedHandlerTest, RetransmissionTimeoutUsesInjectedTimeAndSnapshot) {
  auto receiver = std::make_unique<feed::SyntheticReceiver>(kIdentity, 8U, 8U);
  const feed::SyntheticDecoder decoder{kIdentity};
  DelayedRecovery recovery;
  recovery.snapshot = make_snapshot(2U, 110U);
  TestJournal journal;
  TestNormalizedPublisher normalized;
  TestHealthPublisher health;
  auto config = make_config();
  feed::FeedHandler handler{config,  *receiver,  decoder, recovery,
                            journal, normalized, health};
  ASSERT_TRUE(receiver->enqueue(feed::make_synthetic_raw_packet(
      make_event(1U, 1U), kSession, feed::FeedLeg::a, 101U)));
  ASSERT_TRUE(receiver->enqueue(feed::make_synthetic_raw_packet(
      make_event(3U, 3U), kSession, feed::FeedLeg::a, 102U)));
  ASSERT_TRUE(handler.start(100U));
  EXPECT_EQ(handler.poll_once(101U), feed::PollResult::processed);
  EXPECT_EQ(handler.poll_once(102U), feed::PollResult::processed);
  EXPECT_EQ(handler.state(), feed::FeedState::replaying_gap);
  handler.on_timer(112U);
  EXPECT_EQ(handler.state(), feed::FeedState::recovering);
  EXPECT_EQ(handler.poll_once(113U), feed::PollResult::processed);
  EXPECT_EQ(handler.state(), feed::FeedState::healthy);
  EXPECT_EQ(recovery.retransmission_count, 1U);
  EXPECT_EQ(recovery.snapshot_count, 1U);
  EXPECT_EQ(normalized.snapshot_count(), 1U);
}

TEST(FeedHandlerTest, CorruptionAndIdentityMismatchFailClosed) {
  {
    auto harness = std::make_unique<HandlerHarness>();
    auto packet = feed::make_synthetic_raw_packet(make_event(1U, 1U), kSession,
                                                  feed::FeedLeg::a, 101U);
    packet.bytes[synthetic::kPacketHeaderBytes + 12U] ^= 0x01U;
    ASSERT_TRUE(harness->receiver.enqueue(packet));
    ASSERT_TRUE(harness->handler.start(100U));
    EXPECT_EQ(harness->handler.poll_once(101U), feed::PollResult::invalid);
    EXPECT_EQ(harness->handler.state(), feed::FeedState::invalid);
    EXPECT_EQ(harness->normalized.count(), 0U);
  }
  {
    auto harness = std::make_unique<HandlerHarness>();
    auto event = make_event(1U, 1U);
    ++event.channel_number;
    event.event_hash = synthetic::calculate_event_hash(event);
    ASSERT_TRUE(harness->receiver.enqueue(
        feed::make_synthetic_raw_packet(event, kSession, feed::FeedLeg::a, 101U)));
    ASSERT_TRUE(harness->handler.start(100U));
    EXPECT_EQ(harness->handler.poll_once(101U), feed::PollResult::invalid);
    EXPECT_EQ(harness->handler.metrics().identity_mismatches, 1U);
  }
}

TEST(FeedHandlerTest, ReceiverAndPublisherOverloadAreObservableAndFatal) {
  {
    auto harness = std::make_unique<HandlerHarness>(1U);
    ASSERT_TRUE(harness->enqueue(1U, 1U, feed::FeedLeg::a, 101U));
    EXPECT_FALSE(harness->enqueue(2U, 2U, feed::FeedLeg::a, 102U));
    ASSERT_TRUE(harness->handler.start(100U));
    EXPECT_EQ(harness->handler.poll_once(101U), feed::PollResult::invalid);
    EXPECT_EQ(harness->handler.metrics().receiver_overruns, 1U);
    EXPECT_EQ(harness->normalized.count(), 0U);
  }
  {
    auto harness = std::make_unique<HandlerHarness>(8U, 64U, 0U);
    ASSERT_TRUE(harness->enqueue(1U, 1U, feed::FeedLeg::a, 101U));
    ASSERT_TRUE(harness->handler.start(100U));
    EXPECT_EQ(harness->handler.poll_once(101U), feed::PollResult::invalid);
    EXPECT_EQ(harness->handler.metrics().normalized_publisher_drops, 1U);
  }
  {
    auto harness = std::make_unique<HandlerHarness>(8U, 0U);
    ASSERT_TRUE(harness->enqueue(1U, 1U, feed::FeedLeg::a, 101U));
    ASSERT_TRUE(harness->handler.start(100U));
    EXPECT_EQ(harness->handler.poll_once(101U), feed::PollResult::invalid);
    EXPECT_EQ(harness->handler.metrics().raw_journal_drops, 1U);
  }
  {
    auto config = make_config();
    config.arbitration_window = 2U;
    auto harness = std::make_unique<HandlerHarness>(8U, 64U, 128U, 128U, config);
    ASSERT_TRUE(harness->enqueue(3U, 3U, feed::FeedLeg::a, 101U, false));
    ASSERT_TRUE(harness->handler.start(100U));
    EXPECT_EQ(harness->handler.poll_once(101U), feed::PollResult::invalid);
    EXPECT_EQ(harness->handler.metrics().arbitration_overloads, 1U);
  }
}

TEST(FeedHandlerTest, DetectsStaleDataAndShutsDownCleanly) {
  auto harness = std::make_unique<HandlerHarness>();
  ASSERT_TRUE(harness->receiver.install_snapshot(make_snapshot(1U, 201U)));
  ASSERT_TRUE(harness->enqueue(1U, 1U, feed::FeedLeg::a, 101U));
  ASSERT_TRUE(harness->handler.start(100U));
  EXPECT_EQ(harness->handler.poll_once(101U), feed::PollResult::processed);
  harness->handler.on_timer(201U);
  EXPECT_EQ(harness->handler.state(), feed::FeedState::stale);
  EXPECT_EQ(harness->handler.data_quality().quality, feed::DataQualityCode::stale);
  EXPECT_EQ(harness->handler.poll_once(202U), feed::PollResult::processed);
  EXPECT_EQ(harness->handler.state(), feed::FeedState::healthy);
  harness->handler.stop(203U);
  EXPECT_EQ(harness->handler.state(), feed::FeedState::stopped);
  EXPECT_EQ(harness->handler.poll_once(204U), feed::PollResult::stopped);
}

TEST(FeedHandlerTest, HealthPublisherBackpressurePreventsHealthyState) {
  auto harness = std::make_unique<HandlerHarness>(8U, 64U, 128U, 1U);
  EXPECT_FALSE(harness->handler.start(100U));
  EXPECT_EQ(harness->handler.state(), feed::FeedState::invalid);
  EXPECT_EQ(harness->handler.metrics().health_publisher_drops, 1U);
}
// NOLINTEND(readability-function-cognitive-complexity)
