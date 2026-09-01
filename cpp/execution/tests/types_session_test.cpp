#include "aegis/execution/session.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace aegis::execution::test {

TEST(GatewayTypesTest, DefaultsAreSimulationOnlyAndConfigurationIsBound) {
  const auto config = configuration();
  EXPECT_TRUE(valid_gateway_configuration(config));
  EXPECT_EQ(config.startup_mode, GatewayMode::simulation);
  EXPECT_FALSE(kLiveTradingCompiled);
  EXPECT_EQ(gateway_mode_name(config.startup_mode), "SIMULATION");
  EXPECT_FALSE(common::current_build_info().live_trading_capable);

  auto live_like = config;
  live_like.startup_mode = GatewayMode::halted;
  live_like.stable_hash = stable_gateway_configuration_hash(live_like);
  EXPECT_FALSE(valid_gateway_configuration(live_like));
}

TEST(SequenceManagerTest, DetectsDuplicateGapAndExhaustion) {
  SequenceManager manager{1U, 10U};
  EXPECT_EQ(manager.observe_inbound(10U), SequenceStatus::in_order);
  EXPECT_EQ(manager.observe_inbound(10U), SequenceStatus::duplicate);
  EXPECT_EQ(manager.observe_inbound(12U), SequenceStatus::gap);
  EXPECT_EQ(manager.observe_inbound(9U), SequenceStatus::out_of_order);

  manager.reset(std::numeric_limits<std::uint64_t>::max(), 1U);
  std::uint64_t sequence{};
  EXPECT_EQ(manager.claim_outbound(sequence), SequenceStatus::in_order);
  EXPECT_EQ(sequence, std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(manager.claim_outbound(sequence), SequenceStatus::exhausted);
}

TEST(SessionLifecycleTest, LogonHeartbeatRecoveryAndLogoffAreExplicit) {
  SessionLifecycle session{77U, 88U, 100U, 500U};
  ASSERT_TRUE(session.request_logon(1'000U));
  ASSERT_TRUE(session.accept_logon(1'001U));
  EXPECT_TRUE(session.ready());
  EXPECT_FALSE(session.heartbeat_due(1'050U));
  EXPECT_TRUE(session.heartbeat_due(1'101U));

  std::uint64_t outbound{};
  EXPECT_EQ(session.claim_outbound(1'101U, outbound), SequenceStatus::in_order);
  EXPECT_EQ(outbound, 1U);
  EXPECT_EQ(session.observe_inbound(1'102U, 1U), SequenceStatus::first);
  EXPECT_EQ(session.observe_inbound(1'103U, 3U), SequenceStatus::gap);
  EXPECT_EQ(session.state(), SessionState::halted);
  ASSERT_TRUE(session.begin_recovery(1'104U, SessionReason::recovery_requested));
  ASSERT_TRUE(session.complete_recovery(1'105U, 3U));
  EXPECT_TRUE(session.ready());
  ASSERT_TRUE(session.request_logoff(1'106U));
  EXPECT_TRUE(session.complete_logoff(1'107U));
  EXPECT_EQ(session.state(), SessionState::stopped);
  EXPECT_NE(session.snapshot().stable_hash, 0U);
}

TEST(RateLimiterTest, UsesDeterministicInclusiveWindowBoundary) {
  FixedWindowRateLimiter limiter{100U, 2U};
  ASSERT_TRUE(limiter.valid());
  EXPECT_TRUE(limiter.allow(1'000U));
  EXPECT_TRUE(limiter.allow(1'050U));
  EXPECT_FALSE(limiter.allow(1'099U));
  EXPECT_TRUE(limiter.allow(1'100U));
  EXPECT_FALSE(limiter.allow(1'099U));
}

} // namespace aegis::execution::test
