#include "aegis/market_data/synthetic/generator.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace synthetic = aegis::market_data::synthetic;

// GoogleTest assertion macros expand into control flow that clang-tidy counts as
// test-body complexity and cannot use to prove subsequent optional access safe.
// NOLINTBEGIN(readability-function-cognitive-complexity,bugprone-unchecked-optional-access)
TEST(SyntheticGenerator, SeedProducesIdenticalStreamAndFinalBook) {
  auto config = synthetic::make_default_config();
  config.seed = 0x1234'5678'9ABC'DEF0ULL;
  config.event_count = 5'000U;
  auto first = synthetic::SyntheticExchangeGenerator::create(config);
  auto second = synthetic::SyntheticExchangeGenerator::create(config);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_LT(first->bounded_state_bytes(), 1U << 20U);

  for (std::uint64_t index = 0; index < config.event_count; ++index) {
    synthetic::SyntheticEvent left{};
    synthetic::SyntheticEvent right{};
    ASSERT_TRUE(first->next(left).ok()) << index;
    ASSERT_TRUE(second->next(right).ok()) << index;
    EXPECT_EQ(left.event_hash, right.event_hash) << index;
    EXPECT_EQ(left.global_ordinal, index + 1U);
    EXPECT_EQ(left.event_hash, synthetic::calculate_event_hash(left));
  }
  EXPECT_EQ(first->expected_book().stable_hash(),
            second->expected_book().stable_hash());
  synthetic::SyntheticEvent ignored{};
  EXPECT_EQ(first->next(ignored).error, synthetic::GenerationError::complete);
}

TEST(SyntheticGenerator, DifferentSeedsDiverge) {
  auto first_config = synthetic::make_default_config();
  auto second_config = first_config;
  first_config.event_count = 100U;
  second_config.event_count = 100U;
  ++second_config.seed;
  auto first = synthetic::SyntheticExchangeGenerator::create(first_config);
  auto second = synthetic::SyntheticExchangeGenerator::create(second_config);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  std::uint64_t differences = 0U;
  for (std::uint64_t index = 0; index < first_config.event_count; ++index) {
    synthetic::SyntheticEvent left{};
    synthetic::SyntheticEvent right{};
    ASSERT_TRUE(first->next(left).ok());
    ASSERT_TRUE(second->next(right).ok());
    differences += left.event_hash != right.event_hash ? 1U : 0U;
  }
  EXPECT_GT(differences, 0U);
}

TEST(SyntheticGenerator, PublishedSeedHasStableGoldenHashes) {
  auto config = synthetic::make_default_config();
  config.event_count = 64U;
  EXPECT_EQ(synthetic::config_hash(config), 0xF95E'9B36'386F'B501ULL);
  auto generator = synthetic::SyntheticExchangeGenerator::create(config);
  ASSERT_TRUE(generator.has_value());
  std::uint64_t first_hash = 0U;
  std::uint64_t eighth_hash = 0U;
  std::uint64_t last_hash = 0U;
  for (std::uint64_t index = 0; index < config.event_count; ++index) {
    synthetic::SyntheticEvent event{};
    ASSERT_TRUE(generator->next(event).ok());
    if (index == 0U) {
      first_hash = event.event_hash;
    }
    if (index == 7U) {
      eighth_hash = event.event_hash;
    }
    last_hash = event.event_hash;
  }
  EXPECT_EQ(first_hash, 0x5863'720B'B3B6'F40BULL);
  EXPECT_EQ(eighth_hash, 0x0A69'A3BC'5287'B285ULL);
  EXPECT_EQ(last_hash, 0x827B'16A5'E3AA'5BD8ULL);
  EXPECT_EQ(generator->expected_book().stable_hash(), 0x2C2A'9740'2396'DCEEULL);
}

TEST(SyntheticGenerator, RejectsUnsafeOrAmbiguousConfiguration) {
  auto config = synthetic::make_default_config();
  config.normal_message_rate_per_second = 0U;
  EXPECT_EQ(synthetic::validate_config(config),
            synthetic::ConfigError::invalid_message_rate);
  EXPECT_FALSE(synthetic::SyntheticExchangeGenerator::create(config).has_value());

  config = synthetic::make_default_config();
  config.instruments[1] = config.instruments[0];
  EXPECT_EQ(synthetic::validate_config(config),
            synthetic::ConfigError::duplicate_instrument);
}
// NOLINTEND(readability-function-cognitive-complexity,bugprone-unchecked-optional-access)
