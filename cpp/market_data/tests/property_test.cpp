#include "aegis/market_data/synthetic/generator.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace synthetic = aegis::market_data::synthetic;

// GoogleTest assertion macros expand into control flow that clang-tidy counts as
// test-body complexity and cannot use to prove subsequent optional access safe.
// NOLINTBEGIN(readability-function-cognitive-complexity,bugprone-unchecked-optional-access)
TEST(SyntheticGeneratorProperty, DeterministicSeedMatrixPreservesInvariants) {
  constexpr std::uint64_t kPropertySeedBase = 0xAE61'5000U;
  for (std::uint64_t seed_offset = 0; seed_offset < 32U; ++seed_offset) {
    auto config = synthetic::make_default_config();
    config.seed = kPropertySeedBase + seed_offset;
    config.event_count = 1'000U;
    config.venue_count = 3U;
    config.instrument_count = 6U;
    for (std::size_t index = 0; index < config.instrument_count; ++index) {
      const auto venue = static_cast<std::uint32_t>((index % 3U) + 1U);
      config.instruments[index] = {
          .venue_number = venue,
          .channel_number = static_cast<std::uint32_t>(index + 10U),
          .instrument_number = static_cast<std::uint32_t>(index + 100U),
          .tick_value_currency_nanos = 5'000'000,
          .initial_mid_price_ticks = 20'000 + static_cast<std::int64_t>(index * 20U),
          .feed_mode = (index % 2U) == 0U ? synthetic::FeedMode::order_level
                                          : synthetic::FeedMode::price_level,
      };
    }
    auto generator = synthetic::SyntheticExchangeGenerator::create(config);
    ASSERT_TRUE(generator.has_value()) << seed_offset;
    std::uint64_t last_ordinal = 0U;
    std::uint64_t last_monotonic = 0U;
    for (std::uint64_t event_index = 0; event_index < config.event_count;
         ++event_index) {
      synthetic::SyntheticEvent event{};
      ASSERT_TRUE(generator->next(event).ok()) << seed_offset << ':' << event_index;
      EXPECT_TRUE(synthetic::valid_event_shape(event));
      EXPECT_EQ(event.event_hash, synthetic::calculate_event_hash(event));
      EXPECT_GT(event.global_ordinal, last_ordinal);
      EXPECT_GE(event.process_monotonic_time_ns, last_monotonic);
      last_ordinal = event.global_ordinal;
      last_monotonic = event.process_monotonic_time_ns;
    }
    EXPECT_TRUE(generator->expected_book().all_valid());
  }
}

TEST(SyntheticGeneratorProperty, EveryScenarioHasExactLogicalCount) {
  constexpr std::uint64_t kScenarioSeed = 0x5CE0'0001U;
  for (auto raw = static_cast<std::uint16_t>(synthetic::Scenario::normal);
       raw <=
       static_cast<std::uint16_t>(synthetic::Scenario::hidden_liquidity_replenishment);
       ++raw) {
    auto config = synthetic::make_default_config();
    config.seed = kScenarioSeed;
    config.event_count = 128U;
    config.scenario = static_cast<synthetic::Scenario>(raw);
    auto generator = synthetic::SyntheticExchangeGenerator::create(config);
    ASSERT_TRUE(generator.has_value()) << synthetic::scenario_name(config.scenario);
    for (std::uint64_t index = 0; index < config.event_count; ++index) {
      synthetic::SyntheticEvent event{};
      ASSERT_TRUE(generator->next(event).ok())
          << synthetic::scenario_name(config.scenario) << ':' << index;
    }
    synthetic::SyntheticEvent extra{};
    EXPECT_EQ(generator->next(extra).error, synthetic::GenerationError::complete);
  }
}
// NOLINTEND(readability-function-cognitive-complexity,bugprone-unchecked-optional-access)
