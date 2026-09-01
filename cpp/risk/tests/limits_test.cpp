#include "aegis/risk/engine.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

namespace risk = aegis::risk;

TEST(RiskLimitTest, RejectsTamperingAndConfigurationRollback) {
  auto limits = risk::test::limits();
  ASSERT_TRUE(risk::valid_limit_snapshot(limits));
  ++limits.maximum_orders_per_window;
  EXPECT_FALSE(risk::valid_limit_snapshot(limits));

  risk::RiskDecisionJournal journal;
  auto current = risk::test::limits();
  risk::DeterministicPreTradeRiskEngine engine{current, journal};
  auto rollback = current;
  rollback.revision = 0U;
  rollback.stable_hash = risk::stable_limit_snapshot_hash(rollback);
  EXPECT_EQ(engine.install_limits(rollback), risk::InstallStatus::invalid);

  auto conflict = current;
  conflict.allow_paper = false;
  conflict.stable_hash = risk::stable_limit_snapshot_hash(conflict);
  EXPECT_EQ(engine.install_limits(conflict), risk::InstallStatus::revision_conflict);

  auto next = current;
  next.revision = 2U;
  next.authority_epoch = 11U;
  next.stable_hash = risk::stable_limit_snapshot_hash(next);
  EXPECT_EQ(engine.install_limits(next), risk::InstallStatus::installed);

  current.stable_hash = risk::stable_limit_snapshot_hash(current);
  EXPECT_EQ(engine.install_limits(current), risk::InstallStatus::rollback_rejected);
}

TEST(RiskLimitTest, RejectsDuplicateScopesAndInvalidCapacity) {
  auto limits = risk::test::limits();
  limits.symbol_count = 2U;
  limits.symbols[1U] = limits.symbols[0U];
  limits.stable_hash = risk::stable_limit_snapshot_hash(limits);
  EXPECT_FALSE(risk::valid_limit_snapshot(limits));

  limits = risk::test::limits();
  limits.maximum_orders_per_window =
      static_cast<std::uint32_t>(risk::kMaximumRateEvents + 1U);
  limits.stable_hash = risk::stable_limit_snapshot_hash(limits);
  EXPECT_FALSE(risk::valid_limit_snapshot(limits));
}
