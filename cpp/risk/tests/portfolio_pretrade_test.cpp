#include "aegis/risk/engine.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

namespace risk = aegis::risk;
namespace portfolio = aegis::risk::portfolio;
namespace support = aegis::risk::test;

namespace {

[[nodiscard]] portfolio::PortfolioSnapshot snapshot() noexcept {
  portfolio::PortfolioSnapshot value;
  value.risk_snapshot_id = {90U, 1U};
  value.session_id = support::kSession;
  value.configuration_version = support::kConfiguration;
  value.sequence = 1U;
  value.source_journal_sequence = 10U;
  value.as_of_process_monotonic_time_ns = support::kNow - 10U;
  value.health = portfolio::PortfolioHealth::healthy;
  value.ready = true;
  value.instrument_count = 1U;
  value.strategy_count = 1U;
  value.account_count = 1U;
  value.sector_count = 1U;
  value.instruments[0U] = {.instrument_id = support::kInstrument,
                           .net_quantity_units = 9'991,
                           .mark_price_ticks = 100,
                           .gross_exposure_currency_nanos = 9'991'000U,
                           .net_exposure_currency_nanos = 9'991'000};
  value.strategies[0U] = {.strategy_id = support::kStrategy,
                          .gross_exposure_currency_nanos = 9'991'000U,
                          .net_exposure_currency_nanos = 9'991'000,
                          .pnl_currency_nanos = 1'000,
                          .peak_pnl_currency_nanos = 2'000,
                          .drawdown_currency_nanos = 1'000U};
  value.accounts[0U] = {.account_id = support::kAccount,
                        .gross_exposure_currency_nanos = 9'991'000U,
                        .net_exposure_currency_nanos = 9'991'000,
                        .pnl_currency_nanos = 1'000};
  value.sectors[0U] = {.sector_index = 0U,
                       .gross_exposure_currency_nanos = 9'991'000U,
                       .net_exposure_currency_nanos = 9'991'000};
  value.gross_exposure_currency_nanos = 9'991'000U;
  value.net_exposure_currency_nanos = 9'991'000;
  value.realized_pnl_currency_nanos = 1'000;
  value.total_pnl_currency_nanos = 1'000;
  value.peak_pnl_currency_nanos = 2'000;
  value.drawdown_currency_nanos = 1'000U;
  value.stable_hash = portfolio::stable_snapshot_hash(value);
  return value;
}

TEST(PortfolioPreTradeTest, ImportsOneAtomicLocalSnapshotWithoutNetworkDependency) {
  auto limits = support::limits();
  risk::RiskDecisionJournal journal;
  risk::DeterministicPreTradeRiskEngine engine(limits, journal);
  ASSERT_EQ(engine.install_portfolio_snapshot(snapshot()),
            risk::StateUpdateStatus::applied);
  engine.set_state_available(true);
  const auto result = engine.evaluate(support::request());
  ASSERT_EQ(result.status, risk::EvaluationStatus::journaled);
  EXPECT_EQ(result.decision.decision, risk::DecisionCode::rejected);
  EXPECT_EQ(result.decision.reason, risk::RiskReason::symbol_position_exceeded);
  EXPECT_EQ(result.decision.failed_check, risk::RiskCheck::symbol_position);
}

TEST(PortfolioPreTradeTest, RejectsRollbackConflictAndMalformedSnapshots) {
  risk::RiskDecisionJournal journal;
  risk::DeterministicPreTradeRiskEngine engine(support::limits(), journal);
  auto current = snapshot();
  ASSERT_EQ(engine.install_portfolio_snapshot(current),
            risk::StateUpdateStatus::applied);
  auto conflict = current;
  ++conflict.total_pnl_currency_nanos;
  conflict.stable_hash = portfolio::stable_snapshot_hash(conflict);
  EXPECT_EQ(engine.install_portfolio_snapshot(conflict),
            risk::StateUpdateStatus::unavailable);
  auto malformed = current;
  malformed.ready = false;
  malformed.stable_hash = portfolio::stable_snapshot_hash(malformed);
  EXPECT_EQ(engine.install_portfolio_snapshot(malformed),
            risk::StateUpdateStatus::invalid);
}

} // namespace
