#include "aegis/common/build_info.hpp"

#include <gtest/gtest.h>

namespace {

TEST(BuildInfoTest, ExposesDeterministicIdentity) {
  const auto& info = aegis::common::current_build_info();

  EXPECT_EQ(info.project, "Aegis-MX");
  EXPECT_EQ(info.version, "0.1.0");
  EXPECT_FALSE(info.source_revision.empty());
  EXPECT_FALSE(info.compiler_id.empty());
  EXPECT_FALSE(info.compiler_version.empty());
  EXPECT_FALSE(info.build_type.empty());
}

TEST(BuildInfoTest, HasNoLiveTradingCapability) {
  EXPECT_FALSE(aegis::common::current_build_info().live_trading_capable);
}

} // namespace
