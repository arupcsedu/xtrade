#include "aegis/features/parity.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <random>

namespace features = aegis::features;
namespace book = aegis::order_book;
namespace synthetic = aegis::market_data::synthetic;

namespace {

constexpr std::uint64_t kDeterministicSeed = 20'260'829U;
constexpr aegis::common::InstrumentId kInstrument{100U, 1U};
constexpr aegis::common::SessionId kSession{101U, 1U};
constexpr aegis::common::VenueId kVenue{102U, 1U};

[[nodiscard]] features::FeatureEngineConfig config() {
  features::FeatureEngineConfig result{.instrument_id = kInstrument,
                                       .session_id = kSession,
                                       .feature_definition_version = {103U, 1U},
                                       .venue_count = 1U,
                                       .depth_levels = 1U,
                                       .window_capacity = 256U,
                                       .minimum_book_events = 1U,
                                       .minimum_trade_events = 0U,
                                       .rolling_window_ns = 1'000'000'000U,
                                       .stale_after_ns = 1'000'000'000U,
                                       .maximum_quantity_units = 1'000'000U,
                                       .maximum_absolute_price_ticks = 1'000'000,
                                       .session_open_exchange_time_ns = 1'000'000'000LL,
                                       .session_close_exchange_time_ns =
                                           10'000'000'000LL};
  result.venues[0U] = {.venue_id = kVenue, .venue_number = 7U};
  return result;
}

// Compact fixture arguments are kept in wire-field order.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] features::BookFeatureEvent
book_event(const std::uint64_t ordinal, const std::int64_t bid, const std::int64_t ask,
           const std::uint64_t bid_quantity, const std::uint64_t ask_quantity) {
  book::DepthSnapshot depth{};
  depth.bids[0U] = {
      .price_ticks = bid, .quantity_units = bid_quantity, .order_count = 1U};
  depth.asks[0U] = {
      .price_ticks = ask, .quantity_units = ask_quantity, .order_count = 1U};
  depth.bid_count = 1U;
  depth.ask_count = 1U;
  depth.version = ordinal;
  depth.validity = book::BookValidity::valid;
  return {.global_event_id = {200U, ordinal},
          .session_id = kSession,
          .instrument_id = kInstrument,
          .venue_id = kVenue,
          .venue_number = 7U,
          .global_ordinal = ordinal,
          .exchange_event_time_ns =
              1'000'000'000LL + static_cast<std::int64_t>(ordinal * 1'000U),
          .process_monotonic_time_ns = ordinal * 1'000U,
          .operation = book::BookOperation::observe_quote,
          .depth = depth,
          .data_quality = synthetic::DataQuality::valid};
}
// NOLINTEND(bugprone-easily-swappable-parameters)

// Seed: 20260829. This compares the incremental hot engine with direct formulas.
// GoogleTest macros intentionally expand into control flow.
// NOLINTBEGIN(readability-function-cognitive-complexity)
TEST(FeaturePropertyTest, RandomizedTopFeaturesMatchReferenceArithmetic) {
  auto engine = std::make_unique<features::FeatureEngine>(config());
  std::mt19937_64 generator{kDeterministicSeed};
  std::uniform_int_distribution<std::int64_t> price_distribution{1'000, 2'000};
  std::uniform_int_distribution<std::uint64_t> quantity_distribution{1U, 1'000U};
  for (std::uint64_t ordinal = 1U; ordinal <= 200U; ++ordinal) {
    const auto bid = price_distribution(generator);
    const auto ask = bid + static_cast<std::int64_t>((generator() % 5U) + 1U);
    const auto bid_quantity = quantity_distribution(generator);
    const auto ask_quantity = quantity_distribution(generator);
    ASSERT_TRUE(
        engine->on_book(book_event(ordinal, bid, ask, bid_quantity, ask_quantity))
            .accepted());
    features::FeatureSnapshot snapshot{};
    ASSERT_EQ(engine->make_snapshot(ordinal * 1'000U, snapshot),
              features::FeatureError::none);
    EXPECT_EQ(snapshot.get(features::FeatureName::midpoint_half_ticks).value,
              bid + ask);
    EXPECT_EQ(snapshot.get(features::FeatureName::spread_ticks).value, ask - bid);
    const auto reference_imbalance =
        (static_cast<std::int64_t>(bid_quantity) -
         static_cast<std::int64_t>(ask_quantity)) *
        features::kPartsPerMillion /
        static_cast<std::int64_t>(bid_quantity + ask_quantity);
    EXPECT_EQ(snapshot.get(features::FeatureName::top_level_imbalance_ppm).value,
              reference_imbalance);
  }
}

TEST(FeatureParityTestHarnessTest, OnlineAndReplayAreByteContractEquivalent) {
  auto harness = std::make_unique<features::FeatureParityTestHarness>(config());
  std::mt19937_64 generator{kDeterministicSeed};
  for (std::uint64_t ordinal = 1U; ordinal <= 128U; ++ordinal) {
    const auto bid = 1'000 + static_cast<std::int64_t>(generator() % 20U);
    const auto ask = bid + 2;
    ASSERT_TRUE(harness
                    ->on_book(book_event(ordinal, bid, ask, (generator() % 500U) + 1U,
                                         (generator() % 500U) + 1U))
                    .accepted());
  }
  const auto parity = harness->verify(128'000U);
  EXPECT_TRUE(parity.matched());
  EXPECT_EQ(parity.event_count, 128U);
  EXPECT_NE(parity.online_hash, 0U);
}

TEST(FeatureEnginePropertyTest, CapacityTruncationIsExplicitlyDegraded) {
  auto local_config = config();
  local_config.window_capacity = 2U;
  auto engine = std::make_unique<features::FeatureEngine>(local_config);
  ASSERT_TRUE(engine->on_book(book_event(1U, 100, 102, 10U, 10U)).accepted());
  ASSERT_TRUE(engine->on_book(book_event(2U, 100, 102, 11U, 10U)).accepted());
  ASSERT_TRUE(engine->on_book(book_event(3U, 100, 102, 12U, 10U)).accepted());
  features::FeatureSnapshot snapshot{};
  ASSERT_EQ(engine->make_snapshot(3'000U, snapshot), features::FeatureError::none);
  EXPECT_EQ(snapshot.state, features::EngineState::degraded);
  EXPECT_EQ(snapshot.get(features::FeatureName::quote_intensity_millihertz).validity,
            features::FeatureValidity::degraded);
}
// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
