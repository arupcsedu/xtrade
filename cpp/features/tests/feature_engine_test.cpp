#include "aegis/features/exponential_statistics.hpp"
#include "aegis/features/parity.hpp"
#include "aegis/features/publisher.hpp"
#include "aegis/features/rolling_window.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>

namespace features = aegis::features;
namespace book = aegis::order_book;
namespace synthetic = aegis::market_data::synthetic;

namespace {

constexpr aegis::common::InstrumentId kInstrument{1U, 10U};
constexpr aegis::common::SessionId kSession{2U, 10U};
constexpr aegis::common::SessionId kNextSession{2U, 11U};
constexpr aegis::common::ConfigurationVersion kFeatureVersion{3U, 10U};
constexpr aegis::common::VenueId kVenueOne{4U, 10U};
constexpr aegis::common::VenueId kVenueTwo{4U, 20U};
constexpr std::int64_t kSessionOpen = 1'000'000'000'000LL;
constexpr std::int64_t kSessionClose = 2'000'000'000'000LL;

[[nodiscard]] features::FeatureEngineConfig
config(const std::uint32_t venue_count = 2U) {
  features::FeatureEngineConfig result{.instrument_id = kInstrument,
                                       .session_id = kSession,
                                       .feature_definition_version = kFeatureVersion,
                                       .venue_count = venue_count,
                                       .depth_levels = 2U,
                                       .window_capacity = 16U,
                                       .minimum_book_events = venue_count,
                                       .minimum_trade_events = 2U,
                                       .rolling_window_ns = 1'000'000'000U,
                                       .stale_after_ns = 500'000'000U,
                                       .replenishment_horizon_ns = 5'000'000U,
                                       .minimum_replenishment_quantity_units = 5U,
                                       .maximum_quantity_units = 1'000'000U,
                                       .maximum_absolute_price_ticks = 1'000'000,
                                       .session_open_exchange_time_ns = kSessionOpen,
                                       .session_close_exchange_time_ns = kSessionClose};
  result.venues[0U] = {.venue_id = kVenueOne, .venue_number = 1U};
  result.venues[1U] = {.venue_id = kVenueTwo, .venue_number = 2U};
  return result;
}

// Compact fixture arguments are kept in bid/ask wire-field order.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] book::DepthSnapshot
depth(const std::int64_t bid, const std::uint64_t bid_quantity, const std::int64_t ask,
      const std::uint64_t ask_quantity, const std::uint64_t version = 1U) {
  book::DepthSnapshot result{};
  result.bids[0U] = {
      .price_ticks = bid, .quantity_units = bid_quantity, .order_count = 1U};
  result.asks[0U] = {
      .price_ticks = ask, .quantity_units = ask_quantity, .order_count = 1U};
  result.bid_count = 1U;
  result.ask_count = 1U;
  result.version = version;
  result.validity = book::BookValidity::valid;
  return result;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

[[nodiscard]] features::BookFeatureEvent
book_event(const std::uint64_t ordinal, const std::uint32_t venue_number,
           const book::DepthSnapshot& snapshot,
           const book::BookOperation operation = book::BookOperation::observe_quote,
           const synthetic::Side side = synthetic::Side::none,
           const std::int64_t affected_price = 0,
           const std::uint64_t affected_quantity = 0U) {
  return {.global_event_id = aegis::common::GlobalEventId{99U, ordinal},
          .session_id = kSession,
          .instrument_id = kInstrument,
          .venue_id = venue_number == 1U ? kVenueOne : kVenueTwo,
          .venue_number = venue_number,
          .global_ordinal = ordinal,
          .exchange_event_time_ns =
              kSessionOpen + static_cast<std::int64_t>(ordinal * 1'000'000U),
          .process_monotonic_time_ns = ordinal * 1'000'000U,
          .operation = operation,
          .side = side,
          .affected_price_ticks = affected_price,
          .affected_quantity_units = affected_quantity,
          .depth = snapshot,
          .data_quality = synthetic::DataQuality::valid};
}

[[nodiscard]] features::TradeFeatureEvent trade_event(const std::uint64_t ordinal,
                                                      const synthetic::Side side,
                                                      const std::int64_t price,
                                                      const std::uint64_t quantity) {
  return {.global_event_id = aegis::common::GlobalEventId{99U, ordinal},
          .session_id = kSession,
          .instrument_id = kInstrument,
          .venue_id = kVenueOne,
          .venue_number = 1U,
          .global_ordinal = ordinal,
          .exchange_event_time_ns =
              kSessionOpen + static_cast<std::int64_t>(ordinal * 1'000'000U),
          .process_monotonic_time_ns = ordinal * 1'000'000U,
          .aggressor_side = side,
          .price_ticks = price,
          .quantity_units = quantity,
          .data_quality = synthetic::DataQuality::valid};
}

[[nodiscard]] std::int64_t value(const features::FeatureSnapshot& snapshot,
                                 const features::FeatureName name) {
  return snapshot.get(name).value;
}

// GoogleTest macros intentionally expand into control flow.
// NOLINTBEGIN(readability-function-cognitive-complexity)

TEST(RollingWindowTest, UsesRuntimeCapacityAndEvictsOldest) {
  features::RollingWindow<int, 4U> window{2U};
  EXPECT_TRUE(window.push(10));
  EXPECT_TRUE(window.push(20));
  int evicted{};
  EXPECT_TRUE(window.push(30, &evicted));
  EXPECT_EQ(evicted, 10);
  const auto* front = window.front();
  const auto* back = window.back();
  ASSERT_NE(front, nullptr);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(*front, 20);
  EXPECT_EQ(*back, 30);
  EXPECT_EQ(window.size(), 2U);
}

TEST(ExponentialMovingStatisticsTest, IsBoundedResettableAndNeverUsesNaNSentinels) {
  features::ExponentialMovingStatistics statistics{500'000U, 1'000U};
  EXPECT_TRUE(statistics.observe(10));
  EXPECT_TRUE(statistics.observe(14));
  EXPECT_TRUE(statistics.valid());
  EXPECT_EQ(statistics.mean_ppm(), 12'000'000);
  EXPECT_EQ(statistics.variance_units_squared(), 2U);
  EXPECT_FALSE(statistics.observe(1'001));
  statistics.reset();
  EXPECT_FALSE(statistics.valid());
  EXPECT_EQ(statistics.count(), 0U);
}

TEST(FeatureEngineTest, ProducesHandCalculatedFixedPointFeatures) {
  auto engine = std::make_unique<features::FeatureEngine>(config());
  auto first_depth = depth(100, 10U, 102, 30U);
  first_depth.bids[1U] = {.price_ticks = 99, .quantity_units = 20U, .order_count = 1U};
  first_depth.asks[1U] = {.price_ticks = 103, .quantity_units = 10U, .order_count = 1U};
  first_depth.bid_count = 2U;
  first_depth.ask_count = 2U;
  ASSERT_TRUE(engine->on_book(book_event(1U, 1U, first_depth)).accepted());
  ASSERT_TRUE(engine->on_book(book_event(2U, 2U, depth(99, 20U, 103, 20U))).accepted());
  ASSERT_TRUE(
      engine->on_trade(trade_event(3U, synthetic::Side::bid, 102, 10U)).accepted());
  ASSERT_TRUE(
      engine->on_trade(trade_event(4U, synthetic::Side::ask, 101, 30U)).accepted());

  features::FeatureSnapshot snapshot{};
  ASSERT_EQ(engine->make_snapshot(4'000'000U, snapshot), features::FeatureError::none);
  EXPECT_EQ(snapshot.state, features::EngineState::ready);
  EXPECT_TRUE(snapshot.snapshot_id.valid());
  EXPECT_EQ(value(snapshot, features::FeatureName::midpoint_half_ticks), 202);
  EXPECT_EQ(value(snapshot, features::FeatureName::spread_ticks), 2);
  EXPECT_EQ(value(snapshot, features::FeatureName::relative_spread_ppm), 19'801);
  EXPECT_EQ(value(snapshot, features::FeatureName::microprice_ticks_ppm), 100'500'000);
  EXPECT_EQ(value(snapshot, features::FeatureName::top_level_imbalance_ppm), -500'000);
  EXPECT_EQ(value(snapshot, features::FeatureName::multi_level_weighted_imbalance_ppm),
            -157'894);
  EXPECT_EQ(value(snapshot, features::FeatureName::signed_trade_imbalance_ppm),
            -500'000);
  EXPECT_EQ(value(snapshot, features::FeatureName::volume_units), 40);
  EXPECT_EQ(value(snapshot, features::FeatureName::vwap_ticks_ppm), 101'250'000);
  EXPECT_EQ(value(snapshot, features::FeatureName::cross_venue_divergence_half_ticks),
            0);
  EXPECT_EQ(value(snapshot, features::FeatureName::bid_leader_venue_number), 1);
  EXPECT_EQ(value(snapshot, features::FeatureName::ask_leader_venue_number), 1);
  EXPECT_EQ(snapshot.stable_hash, features::stable_snapshot_hash(snapshot));
}

TEST(FeatureEngineTest, TracksReturnsVolatilityFlowAndReplenishmentIncrementally) {
  auto local_config = config(1U);
  local_config.minimum_book_events = 2U;
  local_config.minimum_trade_events = 0U;
  auto engine = std::make_unique<features::FeatureEngine>(local_config);
  ASSERT_TRUE(engine
                  ->on_book(book_event(1U, 1U, depth(100, 10U, 102, 10U),
                                       book::BookOperation::partial_cancel,
                                       synthetic::Side::bid, 100, 5U))
                  .accepted());
  ASSERT_TRUE(
      engine
          ->on_book(book_event(2U, 1U, depth(101, 10U, 103, 10U),
                               book::BookOperation::add, synthetic::Side::bid, 100, 5U))
          .accepted());
  features::FeatureSnapshot snapshot{};
  ASSERT_EQ(engine->make_snapshot(2'000'000U, snapshot), features::FeatureError::none);
  EXPECT_EQ(value(snapshot, features::FeatureName::rolling_return_ppm), 9'900);
  EXPECT_EQ(value(snapshot, features::FeatureName::realized_volatility_ppm), 7'000);
  EXPECT_NE(value(snapshot, features::FeatureName::order_flow_imbalance_units), 0);
  EXPECT_GT(value(snapshot, features::FeatureName::replenishment_rate_millihertz), 0);
}

TEST(FeatureEngineTest, ExposesWarmupMissingStaleAndGapStates) {
  auto engine = std::make_unique<features::FeatureEngine>(config(1U));
  ASSERT_TRUE(
      engine->on_book(book_event(1U, 1U, depth(100, 10U, 102, 10U))).accepted());
  features::FeatureSnapshot warming{};
  ASSERT_EQ(engine->make_snapshot(1'000'000U, warming), features::FeatureError::none);
  EXPECT_EQ(warming.state, features::EngineState::warming);
  EXPECT_EQ(warming.get(features::FeatureName::vwap_ticks_ppm).validity,
            features::FeatureValidity::warming);

  features::FeatureSnapshot stale{};
  ASSERT_EQ(engine->make_snapshot(600'000'001U, stale), features::FeatureError::none);
  EXPECT_EQ(stale.state, features::EngineState::stale);
  EXPECT_EQ(stale.get(features::FeatureName::midpoint_half_ticks).validity,
            features::FeatureValidity::stale);
  EXPECT_EQ(value(stale, features::FeatureName::data_age_ns), 599'000'001);

  EXPECT_TRUE(engine->mark_gap(700'000'000U).accepted());
  features::FeatureSnapshot invalid{};
  ASSERT_EQ(engine->make_snapshot(700'000'000U, invalid), features::FeatureError::none);
  EXPECT_EQ(invalid.state, features::EngineState::invalid);
  EXPECT_EQ(invalid.get(features::FeatureName::midpoint_half_ticks).validity,
            features::FeatureValidity::invalid);
  EXPECT_EQ(invalid.get(features::FeatureName::vwap_ticks_ppm).validity,
            features::FeatureValidity::invalid);
  EXPECT_TRUE(engine->begin_recovery(700'000'001U).accepted());
  EXPECT_EQ(engine->make_snapshot(700'000'001U, invalid),
            features::FeatureError::no_snapshot);
}

TEST(FeatureEngineTest, TradeActivityCannotRefreshStaleVenueBooks) {
  auto local_config = config();
  local_config.minimum_trade_events = 0U;
  auto engine = std::make_unique<features::FeatureEngine>(local_config);
  ASSERT_TRUE(
      engine->on_book(book_event(1U, 1U, depth(100, 10U, 102, 10U))).accepted());
  ASSERT_TRUE(engine->on_book(book_event(2U, 2U, depth(99, 10U, 103, 10U))).accepted());
  auto recent_trade = trade_event(3U, synthetic::Side::bid, 102, 1U);
  recent_trade.process_monotonic_time_ns = 600'000'001U;
  ASSERT_TRUE(engine->on_trade(recent_trade).accepted());
  features::FeatureSnapshot snapshot{};
  ASSERT_EQ(engine->make_snapshot(600'000'001U, snapshot),
            features::FeatureError::none);
  EXPECT_EQ(snapshot.state, features::EngineState::stale);
  EXPECT_EQ(snapshot.get(features::FeatureName::midpoint_half_ticks)
                .as_of_process_monotonic_time_ns,
            2'000'000U);
  EXPECT_EQ(snapshot.get(features::FeatureName::vwap_ticks_ppm)
                .as_of_process_monotonic_time_ns,
            600'000'001U);
}

TEST(FeatureEngineTest, ResetsAtSessionBoundaryAndRejectsPriorSession) {
  auto engine = std::make_unique<features::FeatureEngine>(config(1U));
  ASSERT_TRUE(
      engine->on_book(book_event(1U, 1U, depth(100, 10U, 102, 10U))).accepted());
  ASSERT_TRUE(engine
                  ->reset_session(kNextSession, kSessionOpen + 10, kSessionClose + 10,
                                  2'000'000U)
                  .accepted());
  auto old_event = book_event(2U, 1U, depth(100, 10U, 102, 10U));
  EXPECT_EQ(engine->on_book(old_event).error, features::FeatureError::invalid_session);
}

TEST(FeatureEngineTest, FailsClosedOnAggregateOverflow) {
  auto local_config = config();
  local_config.maximum_quantity_units =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  auto engine = std::make_unique<features::FeatureEngine>(local_config);
  const auto maximum = local_config.maximum_quantity_units;
  ASSERT_TRUE(
      engine->on_book(book_event(1U, 1U, depth(100, maximum, 102, 1U))).accepted());
  const auto overflow =
      engine->on_book(book_event(2U, 2U, depth(100, maximum, 103, 1U)));
  EXPECT_EQ(overflow.error, features::FeatureError::numeric_overflow);
  EXPECT_EQ(overflow.state, features::EngineState::invalid);
}

struct Sink {
  features::FeatureSnapshot snapshot;
  bool accept{true};
};

[[nodiscard]] bool publish_to_sink(void* context,
                                   const features::FeatureSnapshot& snapshot) noexcept {
  auto& sink = *static_cast<Sink*>(context);
  sink.snapshot = snapshot;
  return sink.accept;
}

TEST(FeatureSnapshotPublisherTest, ReportsBoundedSinkBackpressure) {
  auto local_config = config(1U);
  local_config.minimum_book_events = 1U;
  local_config.minimum_trade_events = 0U;
  auto engine = std::make_unique<features::FeatureEngine>(local_config);
  ASSERT_TRUE(
      engine->on_book(book_event(1U, 1U, depth(100, 10U, 102, 10U))).accepted());
  Sink sink{};
  features::FeatureSnapshotPublisher publisher{&sink, publish_to_sink};
  EXPECT_EQ(publisher.publish(*engine, 1'000'000U), features::FeatureError::none);
  EXPECT_EQ(publisher.published_count(), 1U);
  sink.accept = false;
  EXPECT_EQ(publisher.publish(*engine, 1'000'000U),
            features::FeatureError::publisher_backpressure);
  EXPECT_EQ(publisher.backpressure_count(), 1U);
}

// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
