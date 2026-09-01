#include "aegis/market_data/synthetic/generator.hpp"
#include "aegis/market_data/synthetic/mock_protocol.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace synthetic = aegis::market_data::synthetic;

// GoogleTest assertion macros expand into control flow that clang-tidy counts as
// test-body complexity and cannot use to prove subsequent optional access safe.
// NOLINTBEGIN(readability-function-cognitive-complexity,bugprone-unchecked-optional-access)
TEST(SyntheticMockProtocol, RoundTripsEveryGeneratedMessageField) {
  auto config = synthetic::make_default_config();
  config.event_count = 2'000U;
  auto generator = synthetic::SyntheticExchangeGenerator::create(config);
  ASSERT_TRUE(generator.has_value());
  for (std::uint64_t index = 0; index < config.event_count; ++index) {
    synthetic::SyntheticEvent expected{};
    ASSERT_TRUE(generator->next(expected).ok()) << index;
    const auto packet = synthetic::encode_packet(expected);
    synthetic::SyntheticEvent actual{};
    ASSERT_EQ(synthetic::decode_packet(packet, actual), synthetic::DecodeError::none)
        << index;
    EXPECT_EQ(actual.event_hash, expected.event_hash);
    EXPECT_EQ(actual.global_ordinal, expected.global_ordinal);
    EXPECT_EQ(actual.channel_sequence, expected.channel_sequence);
    EXPECT_EQ(actual.type, expected.type);
    EXPECT_EQ(actual.price_ticks, expected.price_ticks);
    EXPECT_EQ(actual.level_quantity_units, expected.level_quantity_units);
  }
}

TEST(SyntheticMockProtocol, RejectsCorruptionAndTrailingBytes) {
  auto config = synthetic::make_default_config();
  auto generator = synthetic::SyntheticExchangeGenerator::create(config);
  ASSERT_TRUE(generator.has_value());
  synthetic::SyntheticEvent event{};
  ASSERT_TRUE(generator->next(event).ok());
  auto packet = synthetic::encode_packet(event);
  packet[synthetic::kPacketHeaderBytes + 12U] ^= 0x01U;
  synthetic::SyntheticEvent decoded{};
  EXPECT_EQ(synthetic::decode_packet(packet, decoded),
            synthetic::DecodeError::checksum_mismatch);
  EXPECT_EQ(
      synthetic::decode_packet(
          std::span<const std::uint8_t>{packet}.first(packet.size() - 1U), decoded),
      synthetic::DecodeError::truncated);
}
// NOLINTEND(readability-function-cognitive-complexity,bugprone-unchecked-optional-access)
