#include "aegis/execution/simulated_gateway.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace aegis::execution::test {
namespace {

void rehash(GatewayRequest& value) {
  value.safety.stable_hash = stable_final_safety_state_hash(value.safety);
  value.stable_hash = stable_gateway_request_hash(value);
}

void rehash_risk(GatewayRequest& value) {
  value.risk_decision.stable_hash =
      risk::stable_risk_decision_hash(value.risk_decision);
  value.command.risk_decision_hash = value.risk_decision.stable_hash;
  value.command.stable_hash = oms::stable_gateway_command_hash(value.command);
  rehash(value);
}

// Test fixtures expose components directly to keep each gate assertion focused.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct GatewayHarness {
  explicit GatewayHarness(GatewayConfiguration config = configuration())
      : configuration_value(config), journal(std::make_unique<GatewayAuditJournal>()),
        gateway(std::make_unique<SyntheticExchangeGateway>(configuration_value,
                                                           *journal)) {}

  void start() const { ASSERT_TRUE(gateway->start(kNow - 100U)); }

  GatewayConfiguration configuration_value;
  std::unique_ptr<GatewayAuditJournal> journal;
  std::unique_ptr<SyntheticExchangeGateway> gateway;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

} // namespace

TEST(GatewaySafetyGateTest, AcceptsOnlyExactCurrentRiskBoundCommand) {
  GatewayHarness harness;
  harness.start();
  const auto result = harness.gateway->send_order(request());
  EXPECT_EQ(result.status, GatewaySubmitStatus::accepted);
  EXPECT_EQ(result.reason, GatewayReason::accepted);
  EXPECT_NE(result.journal_record_hash, 0U);
  EXPECT_EQ(result.stable_hash, stable_gateway_submit_result_hash(result));
  auto tampered = result;
  ++tampered.journal_record_hash;
  EXPECT_NE(tampered.stable_hash, stable_gateway_submit_result_hash(tampered));
  EXPECT_EQ(harness.gateway->metrics().mode, GatewayMode::simulation);
  EXPECT_TRUE(harness.journal->verify_chain());
}

TEST(GatewaySafetyGateTest, IndependentlyRejectsUnknownOrUnsafeFinalPredicates) {
  {
    GatewayHarness harness;
    harness.start();
    auto value = request();
    value.safety.clock_quality_snapshot.state = time::ClockQualityState::unsafe;
    rehash(value);
    EXPECT_EQ(harness.gateway->send_order(value).reason,
              GatewayReason::clock_unhealthy);
  }
  {
    GatewayHarness harness;
    harness.start();
    auto value = request();
    value.safety.feed_health = market_state::FeedHealth::stale;
    rehash(value);
    EXPECT_EQ(harness.gateway->send_order(value).reason, GatewayReason::feed_unhealthy);
  }
  {
    GatewayHarness harness;
    harness.start();
    auto value = request();
    value.safety.book_validity = market_state::BookValidity::invalid;
    rehash(value);
    EXPECT_EQ(harness.gateway->send_order(value).reason, GatewayReason::book_invalid);
  }
  {
    GatewayHarness harness;
    harness.start();
    auto value = request();
    value.safety.official_trading_status = market_state::OfficialTradingStatus::halted;
    rehash(value);
    EXPECT_EQ(harness.gateway->send_order(value).reason, GatewayReason::trading_halted);
  }
  {
    GatewayHarness harness;
    harness.start();
    auto value = request();
    value.safety.kill_switch_engaged = true;
    rehash(value);
    EXPECT_EQ(harness.gateway->send_order(value).reason,
              GatewayReason::kill_switch_engaged);
  }
  {
    GatewayHarness harness;
    harness.start();
    auto value = request();
    value.safety.journal_ready = false;
    rehash(value);
    EXPECT_EQ(harness.gateway->send_order(value).reason,
              GatewayReason::journal_capacity);
  }
}

TEST(GatewaySafetyGateTest, RejectsStaleRiskModeScopeAndFencing) {
  {
    GatewayHarness harness;
    harness.start();
    auto value = request();
    value.safety = safety(kNow + 20'000U);
    value.stable_hash = stable_gateway_request_hash(value);
    EXPECT_EQ(harness.gateway->send_order(value).reason,
              GatewayReason::risk_decision_expired);
  }
  {
    GatewayHarness harness;
    harness.start();
    auto value = request();
    value.risk_decision.trading_mode = risk::TradingMode::paper;
    rehash_risk(value);
    EXPECT_EQ(harness.gateway->send_order(value).reason, GatewayReason::mode_mismatch);
  }
  {
    GatewayHarness harness;
    harness.start();
    auto value = request();
    value.command.account_id = common::AccountId{999U, 999U};
    value.command.stable_hash = oms::stable_gateway_command_hash(value.command);
    value.stable_hash = stable_gateway_request_hash(value);
    EXPECT_EQ(harness.gateway->send_order(value).reason, GatewayReason::scope_mismatch);
  }
  {
    GatewayHarness harness;
    harness.start();
    auto value = request();
    --value.command.authority.fencing_token;
    value.command.stable_hash = oms::stable_gateway_command_hash(value.command);
    value.stable_hash = stable_gateway_request_hash(value);
    EXPECT_EQ(harness.gateway->send_order(value).reason,
              GatewayReason::stale_fencing_token);
  }
}

TEST(GatewaySafetyGateTest, SessionAndRateLimitsFailClosed) {
  {
    GatewayHarness harness;
    EXPECT_EQ(harness.gateway->send_order(request()).reason,
              GatewayReason::session_not_active);
  }
  auto config = configuration();
  config.maximum_new_orders_per_window = 1U;
  config.stable_hash = stable_gateway_configuration_hash(config);
  GatewayHarness harness{config};
  harness.start();
  auto first = request(GatewayMode::simulation, oms::GatewayCommandKind::new_order, 1U,
                       kNow, {50U, 1U});
  first.safety.effective_configuration_hash = config.stable_hash;
  rehash(first);
  EXPECT_EQ(harness.gateway->send_order(first).status, GatewaySubmitStatus::accepted);
  auto second = request(GatewayMode::simulation, oms::GatewayCommandKind::new_order, 2U,
                        kNow + 1U, {50U, 2U});
  second.safety.effective_configuration_hash = config.stable_hash;
  rehash(second);
  EXPECT_EQ(harness.gateway->send_order(second).reason, GatewayReason::rate_limited);
}

TEST(GatewaySafetyGateTest, DuplicateCommandCannotEmitTwiceAndConflictIsUnsafe) {
  GatewayHarness harness;
  harness.start();
  const auto value = request();
  EXPECT_EQ(harness.gateway->send_order(value).status, GatewaySubmitStatus::accepted);
  EXPECT_EQ(harness.gateway->send_order(value).status, GatewaySubmitStatus::duplicate);
  EXPECT_EQ(harness.gateway->metrics().duplicate_commands, 1U);

  auto conflict = value;
  ++conflict.command.price_ticks;
  conflict.command.stable_hash = oms::stable_gateway_command_hash(conflict.command);
  conflict.stable_hash = stable_gateway_request_hash(conflict);
  EXPECT_EQ(harness.gateway->send_order(conflict).reason,
            GatewayReason::command_identity_conflict);
  EXPECT_EQ(harness.gateway->health(), GatewayHealth::unsafe);
  EXPECT_EQ(harness.gateway->mode(), GatewayMode::simulation);
  EXPECT_EQ(harness.gateway->metrics().mode, GatewayMode::simulation);
  GatewayAuditRecord audit{};
  ASSERT_TRUE(harness.journal->read(harness.journal->size(), audit));
  EXPECT_EQ(audit.mode, GatewayMode::simulation);
}

TEST(GatewaySafetyGateTest, ProtectiveSyntheticCancelSurvivesHaltAndKill) {
  GatewayAuditJournal journal;
  SyntheticExchangeGateway gateway{configuration(), journal};
  ASSERT_TRUE(gateway.start(kNow - 100U));
  const auto new_request = request();
  ASSERT_EQ(gateway.send_order(new_request).status, GatewaySubmitStatus::accepted);
  GatewayEvent ack{};
  ASSERT_EQ(gateway.poll_event(kNow + 100U, ack), GatewayPollStatus::event);

  auto cancel =
      request(GatewayMode::simulation, oms::GatewayCommandKind::cancel, 2U, kNow + 200U,
              new_request.command.order_id, ack.external_order_id);
  cancel.safety.kill_switch_engaged = true;
  cancel.safety.official_trading_status = market_state::OfficialTradingStatus::halted;
  cancel.safety.feed_health = market_state::FeedHealth::stale;
  cancel.safety.book_validity = market_state::BookValidity::invalid;
  cancel.safety.clock_quality_snapshot.state = time::ClockQualityState::unsafe;
  rehash(cancel);
  EXPECT_EQ(gateway.cancel_order(cancel).status, GatewaySubmitStatus::accepted);
}

} // namespace aegis::execution::test
