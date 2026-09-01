#include "aegis/common/identifiers.hpp"

#include <gtest/gtest.h>

#include <type_traits>

namespace {

TEST(IdentifierTest, UsesDistinctTrivialTypes) {
  static_assert(std::is_trivially_copyable_v<aegis::common::VenueId>);
  static_assert(!std::is_same_v<aegis::common::VenueId, aegis::common::InstrumentId>);

  const aegis::common::VenueId identifier{0x0123456789ABCDEFU, 0xFEDCBA9876543210U};
  EXPECT_TRUE(identifier.valid());
  EXPECT_EQ(aegis::common::to_hex(identifier).data(),
            std::string_view{"0123456789abcdeffedcba9876543210"});
}

TEST(IdentifierTest, ReservesAllZero) {
  constexpr aegis::common::OrderId identifier{};
  static_assert(!identifier.valid());
  EXPECT_FALSE(identifier.valid());
}

} // namespace
