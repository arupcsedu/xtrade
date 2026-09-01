#include "aegis/risk/engine.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>

namespace risk = aegis::risk;

TEST(RiskPropertyTest, MultipleFailuresUseTheDocumentedEvaluationOrder) {
  risk::RiskDecisionJournal journal;
  risk::DeterministicPreTradeRiskEngine engine{risk::test::limits(), journal};
  risk::test::initialize_state(engine);

  auto request = risk::test::request(300U);
  request.context.trading_mode = risk::TradingMode::live;
  request.context.operator_authorized = false;
  request.context.stable_hash = risk::stable_risk_context_hash(request.context);
  auto result = engine.evaluate(request);
  EXPECT_EQ(result.decision.failed_check, risk::RiskCheck::trading_mode_authorization);

  request = risk::test::request(301U);
  request.context.operator_authorized = false;
  request.intent.strategy_id = aegis::common::StrategyId{99U, 99U};
  request.intent.stable_hash = risk::stable_risk_intent_hash(request.intent);
  request.context.stable_hash = risk::stable_risk_context_hash(request.context);
  result = engine.evaluate(request);
  EXPECT_EQ(result.decision.failed_check,
            risk::RiskCheck::operator_session_authorization);

  ASSERT_EQ(engine.update_kill_switch({.scope = risk::KillSwitchScope::firm,
                                       .target_index = 0U,
                                       .authority_epoch = 10U,
                                       .command_sequence = 1U,
                                       .engaged = true,
                                       .operator_authorized = false}),
            risk::StateUpdateStatus::applied);
  request = risk::test::request(302U);
  request.intent.configuration_version = aegis::common::ConfigurationVersion{98U, 98U};
  request.intent.stable_hash = risk::stable_risk_intent_hash(request.intent);
  result = engine.evaluate(request);
  EXPECT_EQ(result.decision.failed_check, risk::RiskCheck::kill_switch);
}

TEST(RiskPropertyTest, FixedSeedReplayProducesIdenticalDecisionHashes) {
  constexpr std::uint64_t kSeed = 20'260'828U;
  auto limits = risk::test::limits();
  limits.maximum_orders_per_window = 2'000U;
  limits.symbols[0U].maximum_absolute_position_units = 100'000U;
  limits.maximum_gross_exposure_currency_nanos = 100'000'000'000U;
  limits.maximum_absolute_net_exposure_currency_nanos = 100'000'000'000U;
  limits.maximum_sector_exposure_currency_nanos[0U] = 100'000'000'000U;
  limits.maximum_factor_exposure_currency_nanos[0U] = 100'000'000'000U;
  limits.credit_capital_limit_currency_nanos = 100'000'000'000U;
  limits.stable_hash = risk::stable_limit_snapshot_hash(limits);
  risk::RiskDecisionJournal first_journal;
  risk::RiskDecisionJournal second_journal;
  risk::DeterministicPreTradeRiskEngine first{limits, first_journal};
  risk::DeterministicPreTradeRiskEngine second{limits, second_journal};
  risk::test::initialize_state(first);
  risk::test::initialize_state(second);
  std::mt19937_64 random{kSeed};
  std::uniform_int_distribution<std::uint64_t> quantity{1U, 20U};
  std::uniform_int_distribution<std::int64_t> price{95, 105};

  for (std::uint64_t index = 0U; index < 1'000U; ++index) {
    auto request = risk::test::request(10'000U + index);
    request.intent.quantity_units = quantity(random);
    request.intent.limit_price_ticks = price(random);
    request.intent.stable_hash = risk::stable_risk_intent_hash(request.intent);
    const auto first_result = first.evaluate(request);
    const auto second_result = second.evaluate(request);
    ASSERT_EQ(first_result.status, risk::EvaluationStatus::journaled)
        << "seed=" << kSeed << " iteration=" << index;
    ASSERT_EQ(second_result.status, risk::EvaluationStatus::journaled)
        << "seed=" << kSeed << " iteration=" << index;
    EXPECT_EQ(first_result.decision.stable_hash, second_result.decision.stable_hash)
        << "seed=" << kSeed << " iteration=" << index;
    EXPECT_TRUE(risk::valid_risk_decision(first_result.decision));
  }
}
