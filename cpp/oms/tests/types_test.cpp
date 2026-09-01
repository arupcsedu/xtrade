#include "aegis/oms/types.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

namespace aegis::oms::test {
namespace {

TEST(OmsTypesTest, ConfigurationIsSimulationOrPaperOnlyAndContentHashed) {
  auto value = configuration();
  EXPECT_TRUE(valid_configuration(value));
  value.trading_mode = risk::TradingMode::live;
  value.stable_hash = stable_configuration_hash(value);
  EXPECT_FALSE(valid_configuration(value));
}

TEST(OmsTypesTest, ExactApprovalAndIdentifiersAreDeterministic) {
  const auto approved = approval(7U);
  ASSERT_TRUE(exact_risk_approval(approved.intent, approved.decision, kNow + 1U));
  const auto first = deterministic_order_id(configuration(), approved.intent);
  const auto second = deterministic_order_id(configuration(), approved.intent);
  EXPECT_EQ(first, second);
  EXPECT_TRUE(first.valid());
  const auto client = deterministic_client_order_id(first);
  EXPECT_EQ(client.back(), '\0');
  EXPECT_NE(client.front(), '\0');

  auto mutated = approved.decision;
  ++mutated.approved_quantity_units;
  mutated.stable_hash = risk::stable_risk_decision_hash(mutated);
  EXPECT_FALSE(exact_risk_approval(approved.intent, mutated, kNow + 1U));
}

TEST(OmsTypesTest, InputsRejectMalformedHashesAndTimestampDomains) {
  auto value = accept(approval(), 1U);
  EXPECT_TRUE(valid_input(value));
  value.stable_hash ^= 1U;
  EXPECT_FALSE(valid_input(value));

  auto execution = fill(common::OrderId{1U, 2U}, 2U, 1U, 1U, 1U);
  EXPECT_TRUE(valid_input(execution));
  execution.exchange_event_time_ns = 0;
  execution.stable_hash = stable_input_hash(execution);
  EXPECT_FALSE(valid_input(execution));
}

TEST(OmsTypesTest, TerminalAndLiveClassificationsAreExhaustive) {
  for (auto raw = static_cast<std::uint8_t>(OrderState::created);
       raw <= static_cast<std::uint8_t>(OrderState::unknown_recovery); ++raw) {
    const auto state = static_cast<OrderState>(raw);
    ASSERT_TRUE(valid_order_state(state));
    EXPECT_FALSE(terminal_state(state) && live_state(state));
  }
  EXPECT_TRUE(terminal_state(OrderState::filled));
  EXPECT_FALSE(live_state(OrderState::unknown_recovery));
}

} // namespace
} // namespace aegis::oms::test
