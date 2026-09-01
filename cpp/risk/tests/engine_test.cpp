#include "aegis/risk/engine.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>

namespace risk = aegis::risk;

namespace {

// GoogleTest fixture state must be protected for generated test subclasses.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
class RiskEngineTest : public ::testing::Test {
protected:
  RiskEngineTest() : snapshot_(risk::test::limits()), engine_(snapshot_, journal_) {}

  void SetUp() override { risk::test::initialize_state(engine_); }

  risk::RiskDecisionJournal journal_;
  risk::RiskLimitSnapshot snapshot_;
  risk::DeterministicPreTradeRiskEngine engine_;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

void refresh(risk::RiskEvaluationRequest& request) {
  request.intent.stable_hash = risk::stable_risk_intent_hash(request.intent);
  request.context.stable_hash = risk::stable_risk_context_hash(request.context);
}

} // namespace

TEST_F(RiskEngineTest, ApprovesBoundaryValuesAndJournalsBeforeReturn) {
  auto request = risk::test::request();
  request.intent.quantity_units = snapshot_.symbols[0U].maximum_order_quantity_units;
  request.intent.limit_price_ticks =
      request.context.reference_price_ticks +
      static_cast<std::int64_t>(snapshot_.symbols[0U].maximum_price_deviation_ticks);
  refresh(request);

  const auto result = engine_.evaluate(request);
  ASSERT_EQ(result.status, risk::EvaluationStatus::journaled);
  EXPECT_EQ(result.decision.decision, risk::DecisionCode::approved);
  EXPECT_EQ(result.decision.reason, risk::RiskReason::within_limits);
  EXPECT_EQ(result.decision.approved_quantity_units,
            snapshot_.symbols[0U].maximum_order_quantity_units);
  EXPECT_TRUE(risk::valid_risk_decision(result.decision));

  risk::RiskDecision journaled{};
  ASSERT_TRUE(journal_.try_pop(journaled));
  EXPECT_EQ(journaled.stable_hash, result.decision.stable_hash);
}

TEST_F(RiskEngineTest, AuthorizationsAndSafetyStateFailClosedInOrder) {
  auto request = risk::test::request(10U);
  request.context.trading_mode = risk::TradingMode::live;
  refresh(request);
  EXPECT_EQ(engine_.evaluate(request).decision.reason,
            risk::RiskReason::trading_mode_unauthorized);

  request = risk::test::request(11U);
  request.context.operator_authorized = false;
  refresh(request);
  EXPECT_EQ(engine_.evaluate(request).decision.reason,
            risk::RiskReason::operator_session_unauthorized);

  request = risk::test::request(12U);
  request.intent.strategy_id = aegis::common::StrategyId{90U, 90U};
  refresh(request);
  EXPECT_EQ(engine_.evaluate(request).decision.reason,
            risk::RiskReason::strategy_unauthorized);

  request = risk::test::request(13U);
  request.intent.instrument_id = aegis::common::InstrumentId{91U, 91U};
  refresh(request);
  EXPECT_EQ(engine_.evaluate(request).decision.reason,
            risk::RiskReason::symbol_unauthorized);

  request = risk::test::request(14U);
  request.context.market_state_snapshot.state =
      aegis::market_state::MarketState::halted;
  request.context.market_state_snapshot.primary_reason =
      aegis::market_state::MarketStateReason::official_halt;
  request.context.market_state_snapshot.reason_mask = aegis::market_state::reason_bit(
      aegis::market_state::MarketStateReason::official_halt);
  request.context.market_state_snapshot.stable_hash =
      aegis::market_state::stable_market_state_hash(
          request.context.market_state_snapshot);
  refresh(request);
  EXPECT_EQ(engine_.evaluate(request).decision.reason,
            risk::RiskReason::market_state_unsafe);

  request = risk::test::request(15U);
  request.context.official_trading_status =
      aegis::market_state::OfficialTradingStatus::halted;
  refresh(request);
  EXPECT_EQ(engine_.evaluate(request).decision.reason,
            risk::RiskReason::trading_halted);

  request = risk::test::request(16U);
  request.context.clock_quality_snapshot.state = aegis::time::ClockQualityState::unsafe;
  request.context.clock_quality_snapshot.operation_mode =
      aegis::time::ClockOperationMode::blocked;
  refresh(request);
  EXPECT_EQ(engine_.evaluate(request).decision.reason,
            risk::RiskReason::clock_unhealthy);

  request = risk::test::request(17U);
  request.context.feed_health = aegis::market_state::FeedHealth::stale;
  refresh(request);
  EXPECT_EQ(engine_.evaluate(request).decision.reason,
            risk::RiskReason::feed_book_unhealthy);
}

TEST_F(RiskEngineTest, RejectsQuantityNotionalCollarTickAndArithmeticBoundaries) {
  auto request = risk::test::request(20U);
  request.intent.quantity_units =
      snapshot_.symbols[0U].maximum_order_quantity_units + 1U;
  refresh(request);
  EXPECT_EQ(engine_.evaluate(request).decision.reason,
            risk::RiskReason::order_quantity_exceeded);

  request = risk::test::request(21U);
  request.intent.limit_price_ticks = std::numeric_limits<std::int64_t>::max();
  request.intent.quantity_units = snapshot_.symbols[0U].maximum_order_quantity_units;
  refresh(request);
  EXPECT_EQ(engine_.evaluate(request).decision.reason,
            risk::RiskReason::arithmetic_overflow);

  request = risk::test::request(22U);
  request.intent.limit_price_ticks = 201;
  refresh(request);
  EXPECT_EQ(engine_.evaluate(request).decision.reason,
            risk::RiskReason::price_collar_exceeded);
}

TEST(RiskEngineStandaloneTest, RejectsInvalidTickIncrement) {
  auto limits = risk::test::limits();
  limits.symbols[0U].price_increment_ticks = 2U;
  limits.stable_hash = risk::stable_limit_snapshot_hash(limits);
  risk::RiskDecisionJournal journal;
  risk::DeterministicPreTradeRiskEngine engine{limits, journal};
  risk::test::initialize_state(engine);
  auto request = risk::test::request(23U);
  request.intent.limit_price_ticks = 101;
  refresh(request);

  EXPECT_EQ(engine.evaluate(request).decision.reason,
            risk::RiskReason::invalid_tick_size);
}

TEST(RiskEngineStandaloneTest, EnforcesRestrictedNotionalAndVenuePolicies) {
  auto limits = risk::test::limits();
  limits.symbols[0U].restricted = true;
  limits.stable_hash = risk::stable_limit_snapshot_hash(limits);
  risk::RiskDecisionJournal restricted_journal;
  risk::DeterministicPreTradeRiskEngine restricted_engine{limits, restricted_journal};
  risk::test::initialize_state(restricted_engine);
  EXPECT_EQ(restricted_engine.evaluate(risk::test::request(24U)).decision.reason,
            risk::RiskReason::restricted_instrument);

  limits = risk::test::limits();
  limits.symbols[0U].maximum_order_notional_currency_nanos = 9'999U;
  limits.stable_hash = risk::stable_limit_snapshot_hash(limits);
  risk::RiskDecisionJournal notional_journal;
  risk::DeterministicPreTradeRiskEngine notional_engine{limits, notional_journal};
  risk::test::initialize_state(notional_engine);
  EXPECT_EQ(notional_engine.evaluate(risk::test::request(25U)).decision.reason,
            risk::RiskReason::order_notional_exceeded);

  limits = risk::test::limits();
  limits.venues[0U].authorized = false;
  limits.stable_hash = risk::stable_limit_snapshot_hash(limits);
  risk::RiskDecisionJournal venue_journal;
  risk::DeterministicPreTradeRiskEngine venue_engine{limits, venue_journal};
  risk::test::initialize_state(venue_engine);
  EXPECT_EQ(venue_engine.evaluate(risk::test::request(26U)).decision.reason,
            risk::RiskReason::venue_unauthorized);
}

TEST_F(RiskEngineTest, DetectsDuplicateIntentAndOrderRate) {
  const auto request = risk::test::request(30U);
  ASSERT_EQ(engine_.evaluate(request).decision.decision, risk::DecisionCode::approved);
  const auto duplicate = engine_.evaluate(request);
  EXPECT_EQ(duplicate.decision.reason, risk::RiskReason::duplicate_intent);
}

TEST(RiskEngineStandaloneTest, EnforcesOrderAndCancelRateWindows) {
  auto limits = risk::test::limits();
  limits.maximum_orders_per_window = 1U;
  limits.maximum_cancels_per_window = 1U;
  limits.stable_hash = risk::stable_limit_snapshot_hash(limits);
  risk::RiskDecisionJournal journal;
  risk::DeterministicPreTradeRiskEngine engine{limits, journal};
  risk::test::initialize_state(engine);

  EXPECT_EQ(engine.evaluate(risk::test::request(31U)).decision.decision,
            risk::DecisionCode::approved);
  EXPECT_EQ(engine.evaluate(risk::test::request(32U)).decision.reason,
            risk::RiskReason::order_rate_exceeded);
  EXPECT_EQ(engine.evaluate(risk::test::request(33U, risk::IntentAction::cancel))
                .decision.decision,
            risk::DecisionCode::approved);
  EXPECT_EQ(engine.evaluate(risk::test::request(34U, risk::IntentAction::cancel))
                .decision.reason,
            risk::RiskReason::cancel_rate_exceeded);
}

TEST(RiskEngineStandaloneTest, EnforcesPositionExposureAndCapitalLimits) {
  auto limits = risk::test::limits();
  limits.symbols[0U].maximum_absolute_position_units = 9U;
  limits.stable_hash = risk::stable_limit_snapshot_hash(limits);
  risk::RiskDecisionJournal journal;
  risk::DeterministicPreTradeRiskEngine engine{limits, journal};
  risk::test::initialize_state(engine);
  EXPECT_EQ(engine.evaluate(risk::test::request(40U)).decision.reason,
            risk::RiskReason::symbol_position_exceeded);

  limits = risk::test::limits();
  limits.maximum_gross_exposure_currency_nanos = 9'999U;
  limits.stable_hash = risk::stable_limit_snapshot_hash(limits);
  risk::RiskDecisionJournal gross_journal;
  risk::DeterministicPreTradeRiskEngine gross_engine{limits, gross_journal};
  risk::test::initialize_state(gross_engine);
  EXPECT_EQ(gross_engine.evaluate(risk::test::request(41U)).decision.reason,
            risk::RiskReason::gross_exposure_exceeded);

  limits = risk::test::limits();
  limits.maximum_absolute_net_exposure_currency_nanos = 9'999U;
  limits.stable_hash = risk::stable_limit_snapshot_hash(limits);
  risk::RiskDecisionJournal net_journal;
  risk::DeterministicPreTradeRiskEngine net_engine{limits, net_journal};
  risk::test::initialize_state(net_engine);
  EXPECT_EQ(net_engine.evaluate(risk::test::request(42U)).decision.reason,
            risk::RiskReason::net_exposure_exceeded);

  limits = risk::test::limits();
  limits.maximum_sector_exposure_currency_nanos[0U] = 9'999U;
  limits.stable_hash = risk::stable_limit_snapshot_hash(limits);
  risk::RiskDecisionJournal sector_journal;
  risk::DeterministicPreTradeRiskEngine sector_engine{limits, sector_journal};
  risk::test::initialize_state(sector_engine);
  EXPECT_EQ(sector_engine.evaluate(risk::test::request(43U)).decision.reason,
            risk::RiskReason::sector_concentration_exceeded);

  limits = risk::test::limits();
  limits.maximum_factor_exposure_currency_nanos[0U] = 9'999U;
  limits.stable_hash = risk::stable_limit_snapshot_hash(limits);
  risk::RiskDecisionJournal factor_journal;
  risk::DeterministicPreTradeRiskEngine factor_engine{limits, factor_journal};
  risk::test::initialize_state(factor_engine);
  EXPECT_EQ(factor_engine.evaluate(risk::test::request(44U)).decision.reason,
            risk::RiskReason::factor_exposure_exceeded);

  limits = risk::test::limits();
  limits.credit_capital_limit_currency_nanos = 9'999U;
  limits.stable_hash = risk::stable_limit_snapshot_hash(limits);
  risk::RiskDecisionJournal credit_journal;
  risk::DeterministicPreTradeRiskEngine credit_engine{limits, credit_journal};
  risk::test::initialize_state(credit_engine);
  EXPECT_EQ(credit_engine.evaluate(risk::test::request(45U)).decision.reason,
            risk::RiskReason::credit_capital_exceeded);
}

TEST(RiskEngineStandaloneTest, OpposingPendingOrdersDoNotNetAwayPositionRisk) {
  auto limits = risk::test::limits();
  limits.symbols[0U].maximum_absolute_position_units = 50U;
  limits.stable_hash = risk::stable_limit_snapshot_hash(limits);
  risk::RiskDecisionJournal journal;
  risk::DeterministicPreTradeRiskEngine engine{limits, journal};
  risk::test::initialize_state(engine);
  ASSERT_EQ(
      engine.seed_position({.symbol_index = 0U,
                            .net_quantity_units = 50,
                            .mark_price_ticks = 100,
                            .as_of_process_monotonic_time_ns = risk::test::kNow - 5U}),
      risk::StateUpdateStatus::applied);

  auto sell = risk::test::request(46U, risk::IntentAction::sell);
  sell.intent.quantity_units = 100U;
  refresh(sell);
  ASSERT_EQ(engine.evaluate(sell).decision.decision, risk::DecisionCode::approved);

  auto buy = risk::test::request(47U, risk::IntentAction::buy);
  buy.intent.quantity_units = 1U;
  refresh(buy);
  EXPECT_EQ(engine.evaluate(buy).decision.reason,
            risk::RiskReason::symbol_position_exceeded);
}

TEST_F(RiskEngineTest, EnforcesLossDrawdownAndPolicyHooks) {
  ASSERT_EQ(engine_.update_profit_loss(
                {.strategy_index = 0U,
                 .firm_daily_pnl_currency_nanos = -1'000'000'001,
                 .firm_peak_pnl_currency_nanos = 0,
                 .strategy_pnl_currency_nanos = 0,
                 .strategy_peak_pnl_currency_nanos = 0,
                 .as_of_process_monotonic_time_ns = risk::test::kNow - 5U}),
            risk::StateUpdateStatus::applied);
  EXPECT_EQ(engine_.evaluate(risk::test::request(50U)).decision.reason,
            risk::RiskReason::daily_loss_exceeded);

  auto request = risk::test::request(51U, risk::IntentAction::sell);
  request.context.short_sale_locate = risk::PolicyHookResult::denied;
  refresh(request);
  // Restore P&L so evaluation reaches the locate hook.
  ASSERT_EQ(engine_.update_profit_loss(
                {.strategy_index = 0U,
                 .firm_daily_pnl_currency_nanos = 0,
                 .firm_peak_pnl_currency_nanos = 0,
                 .strategy_pnl_currency_nanos = 0,
                 .strategy_peak_pnl_currency_nanos = 0,
                 .as_of_process_monotonic_time_ns = risk::test::kNow - 4U}),
            risk::StateUpdateStatus::applied);
  EXPECT_EQ(engine_.evaluate(request).decision.reason,
            risk::RiskReason::short_sale_locate_denied);

  request = risk::test::request(52U);
  request.context.self_trade_prevention = risk::PolicyHookResult::denied;
  refresh(request);
  EXPECT_EQ(engine_.evaluate(request).decision.reason,
            risk::RiskReason::self_trade_prevention_denied);
}

TEST(RiskEngineStandaloneTest, FillProfitUpdatesDrawdownPeak) {
  auto limits = risk::test::limits();
  limits.maximum_drawdown_currency_nanos = 150U;
  limits.strategies[0U].maximum_drawdown_currency_nanos = 150U;
  limits.stable_hash = risk::stable_limit_snapshot_hash(limits);
  risk::RiskDecisionJournal journal;
  risk::DeterministicPreTradeRiskEngine engine{limits, journal};
  risk::test::initialize_state(engine);
  ASSERT_EQ(
      engine.apply_fill({.symbol_index = 0U,
                         .strategy_index = 0U,
                         .side = risk::IntentAction::buy,
                         .fill_quantity_units = 1U,
                         .release_reserved_quantity_units = 0U,
                         .realized_pnl_delta_currency_nanos = 100,
                         .as_of_process_monotonic_time_ns = risk::test::kNow - 9U}),
      risk::StateUpdateStatus::applied);
  ASSERT_EQ(
      engine.apply_fill({.symbol_index = 0U,
                         .strategy_index = 0U,
                         .side = risk::IntentAction::buy,
                         .fill_quantity_units = 1U,
                         .release_reserved_quantity_units = 0U,
                         .realized_pnl_delta_currency_nanos = -200,
                         .as_of_process_monotonic_time_ns = risk::test::kNow - 8U}),
      risk::StateUpdateStatus::applied);
  EXPECT_EQ(engine.evaluate(risk::test::request(52'000U)).decision.reason,
            risk::RiskReason::drawdown_exceeded);
}

TEST_F(RiskEngineTest, EnforcesStrategyLossDrawdownAndConfigurationFreshness) {
  ASSERT_EQ(engine_.update_profit_loss(
                {.strategy_index = 0U,
                 .firm_daily_pnl_currency_nanos = 0,
                 .firm_peak_pnl_currency_nanos = 0,
                 .strategy_pnl_currency_nanos = -500'000'001,
                 .strategy_peak_pnl_currency_nanos = 0,
                 .as_of_process_monotonic_time_ns = risk::test::kNow - 5U}),
            risk::StateUpdateStatus::applied);
  EXPECT_EQ(engine_.evaluate(risk::test::request(53U)).decision.reason,
            risk::RiskReason::strategy_loss_exceeded);

  ASSERT_EQ(engine_.update_profit_loss(
                {.strategy_index = 0U,
                 .firm_daily_pnl_currency_nanos = 0,
                 .firm_peak_pnl_currency_nanos = 1'000'000'001,
                 .strategy_pnl_currency_nanos = 0,
                 .strategy_peak_pnl_currency_nanos = 0,
                 .as_of_process_monotonic_time_ns = risk::test::kNow - 4U}),
            risk::StateUpdateStatus::applied);
  EXPECT_EQ(engine_.evaluate(risk::test::request(54U)).decision.reason,
            risk::RiskReason::drawdown_exceeded);

  ASSERT_EQ(engine_.update_profit_loss(
                {.strategy_index = 0U,
                 .firm_daily_pnl_currency_nanos = 0,
                 .firm_peak_pnl_currency_nanos = 0,
                 .strategy_pnl_currency_nanos = 0,
                 .strategy_peak_pnl_currency_nanos = 0,
                 .as_of_process_monotonic_time_ns = risk::test::kNow - 3U}),
            risk::StateUpdateStatus::applied);
  auto request = risk::test::request(55U);
  request.intent.configuration_version = aegis::common::ConfigurationVersion{99U, 99U};
  refresh(request);
  EXPECT_EQ(engine_.evaluate(request).decision.reason,
            risk::RiskReason::configuration_stale);
}

TEST_F(RiskEngineTest, StopsWithoutProducingDecisionWhenJournalIsFull) {
  for (std::size_t index = 0U; index < risk::kRiskDecisionJournalCapacity; ++index) {
    const auto reservation = journal_.try_reserve();
    ASSERT_TRUE(reservation.valid);
    journal_.commit(reservation, {});
  }
  const auto result = engine_.evaluate(risk::test::request(56U));
  EXPECT_EQ(result.status, risk::EvaluationStatus::journal_unavailable);
  EXPECT_FALSE(result.decision.global_event_id.valid());
}

TEST_F(RiskEngineTest, RejectsStaleUnavailableAndSplitBrainState) {
  constexpr auto later = risk::test::kNow + 20'000U;
  auto request = risk::test::request(60U, risk::IntentAction::buy, later);
  request.intent.created_process_monotonic_time_ns = risk::test::kNow - 100U;
  request.intent.expire_process_monotonic_time_ns = later + 1'000U;
  refresh(request);
  EXPECT_EQ(engine_.evaluate(request).decision.reason,
            risk::RiskReason::stale_position);

  ASSERT_EQ(engine_.seed_position({.symbol_index = 0U,
                                   .net_quantity_units = 0,
                                   .mark_price_ticks = 100,
                                   .as_of_process_monotonic_time_ns = later - 1U}),
            risk::StateUpdateStatus::applied);
  ASSERT_EQ(engine_.update_profit_loss({.strategy_index = 0U,
                                        .firm_daily_pnl_currency_nanos = 0,
                                        .firm_peak_pnl_currency_nanos = 0,
                                        .strategy_pnl_currency_nanos = 0,
                                        .strategy_peak_pnl_currency_nanos = 0,
                                        .as_of_process_monotonic_time_ns = later - 1U}),
            risk::StateUpdateStatus::applied);

  engine_.set_state_available(false);
  request = risk::test::request(61U, risk::IntentAction::buy, later + 1U);
  EXPECT_EQ(engine_.evaluate(request).decision.reason,
            risk::RiskReason::risk_state_unavailable);
  engine_.set_state_available(true);

  request = risk::test::request(62U, risk::IntentAction::buy, later + 2U);
  request.context.authority_epoch = 11U;
  refresh(request);
  EXPECT_EQ(engine_.evaluate(request).decision.reason,
            risk::RiskReason::split_brain_epoch);
}

TEST_F(RiskEngineTest, RejectsRegressingProcessMonotonicTime) {
  ASSERT_EQ(engine_
                .evaluate(risk::test::request(63U, risk::IntentAction::buy,
                                              risk::test::kNow + 1U))
                .decision.decision,
            risk::DecisionCode::approved);
  EXPECT_EQ(engine_.evaluate(risk::test::request(64U)).decision.reason,
            risk::RiskReason::risk_state_unavailable);
}

TEST_F(RiskEngineTest, AllKillSwitchScopesBlockAndRequireAuthorizedReset) {
  constexpr std::array scopes{
      risk::KillSwitchScope::symbol, risk::KillSwitchScope::strategy,
      risk::KillSwitchScope::venue, risk::KillSwitchScope::account,
      risk::KillSwitchScope::firm};
  std::uint64_t command_sequence = 1U;
  std::uint64_t intent_ordinal = 70U;
  for (const auto scope : scopes) {
    ASSERT_EQ(engine_.update_kill_switch({.scope = scope,
                                          .target_index = 0U,
                                          .authority_epoch = 10U,
                                          .command_sequence = command_sequence++,
                                          .engaged = true,
                                          .operator_authorized = false}),
              risk::StateUpdateStatus::applied);
    EXPECT_EQ(engine_.evaluate(risk::test::request(intent_ordinal++)).decision.reason,
              risk::RiskReason::kill_switch_engaged);
    ASSERT_EQ(engine_.update_kill_switch({.scope = scope,
                                          .target_index = 0U,
                                          .authority_epoch = 10U,
                                          .command_sequence = command_sequence++,
                                          .engaged = false,
                                          .operator_authorized = true}),
              risk::StateUpdateStatus::applied);
  }
}
