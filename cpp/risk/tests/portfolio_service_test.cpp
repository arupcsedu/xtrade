#include "portfolio_test_support.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

namespace portfolio = aegis::risk::portfolio;
namespace support = aegis::risk::portfolio::test;

namespace {

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct Fixture {
  portfolio::PortfolioJournal journal;
  std::unique_ptr<portfolio::PortfolioRiskService> service;

  explicit Fixture(const bool require_drop_copy = false)
      : service(std::make_unique<portfolio::PortfolioRiskService>(
            support::configuration(require_drop_copy), journal)) {}
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

TEST(PortfolioConfigurationTest, RejectsMalformedAndHashMismatchedValues) {
  auto value = support::configuration();
  EXPECT_TRUE(portfolio::valid_configuration(value));
  value.instruments[0U].tick_value_currency_nanos = 0U;
  value.stable_hash = portfolio::stable_configuration_hash(value);
  EXPECT_FALSE(portfolio::valid_configuration(value));
  value = support::configuration();
  ++value.stable_hash;
  EXPECT_FALSE(portfolio::valid_configuration(value));
}

TEST(PortfolioServiceTest, StartsFailClosedUntilEveryConfiguredMarkArrives) {
  Fixture fixture;
  ASSERT_TRUE(fixture.service->initialized());
  EXPECT_FALSE(fixture.service->ready());
  EXPECT_EQ(fixture.service->health(), portfolio::PortfolioHealth::starting);
  EXPECT_EQ(fixture.service->apply(support::mark(1U)).status,
            portfolio::ApplyStatus::applied);
  EXPECT_TRUE(fixture.service->ready());
  const auto snapshot = fixture.service->snapshot_for_quiescent_inspection();
  EXPECT_TRUE(snapshot.ready);
  EXPECT_EQ(snapshot.health, portfolio::PortfolioHealth::healthy);
  EXPECT_EQ(snapshot.stable_hash, portfolio::stable_snapshot_hash(snapshot));
}

TEST(PortfolioServiceTest, AppliesLogicalFillExactlyOnceAndReconcilesDropCopy) {
  Fixture fixture(true);
  ASSERT_EQ(fixture.service->apply(support::mark(1U)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(fixture.service->apply(support::order(2U)).status,
            portfolio::ApplyStatus::applied);
  const auto primary = support::fill(3U, 1U);
  ASSERT_EQ(fixture.service->apply(primary).status, portfolio::ApplyStatus::applied);
  EXPECT_FALSE(fixture.service->ready());
  EXPECT_EQ(fixture.service->apply(primary).status, portfolio::ApplyStatus::duplicate);
  const auto drop_copy = support::fill(4U, 1U, portfolio::Side::buy, 100, 10U, 0,
                                       portfolio::FillSource::drop_copy);
  EXPECT_EQ(fixture.service->apply(drop_copy).status,
            portfolio::ApplyStatus::reconciled);
  EXPECT_TRUE(fixture.service->ready());
  const auto snapshot = fixture.service->snapshot_for_quiescent_inspection();
  EXPECT_EQ(snapshot.instruments[0U].net_quantity_units, 10);
  EXPECT_EQ(snapshot.active_fill_count, 1U);
  EXPECT_EQ(snapshot.unmatched_primary_fill_count, 0U);
  EXPECT_EQ(snapshot.unmatched_drop_copy_fill_count, 0U);
  EXPECT_EQ(fixture.service->metrics().duplicates, 1U);
  EXPECT_EQ(fixture.service->metrics().reconciled, 1U);
}

TEST(PortfolioServiceTest, MaintainsAverageCostAndRealizedAndUnrealizedPnl) {
  Fixture fixture;
  ASSERT_EQ(fixture.service->apply(support::mark(1U, 130)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(fixture.service->apply(support::order(2U)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(
      fixture.service->apply(support::fill(3U, 1U, portfolio::Side::buy, 100, 10U, 5))
          .status,
      portfolio::ApplyStatus::applied);
  ASSERT_EQ(
      fixture.service->apply(support::fill(4U, 2U, portfolio::Side::buy, 120, 10U, 5))
          .status,
      portfolio::ApplyStatus::applied);

  auto snapshot = fixture.service->snapshot_for_quiescent_inspection();
  EXPECT_EQ(snapshot.instruments[0U].net_quantity_units, 20);
  EXPECT_EQ(snapshot.realized_pnl_currency_nanos, -10);
  EXPECT_EQ(snapshot.unrealized_pnl_currency_nanos, 40'000);
  EXPECT_EQ(snapshot.total_pnl_currency_nanos, 39'990);
  EXPECT_EQ(snapshot.gross_exposure_currency_nanos, 260'000U);
  EXPECT_EQ(snapshot.net_exposure_currency_nanos, 260'000);
  EXPECT_EQ(snapshot.beta_exposure_currency_nanos, 312'000);
  EXPECT_EQ(snapshot.liquidity_adjusted_exposure_currency_nanos, 390'000U);
  EXPECT_EQ(snapshot.event_exposure_currency_nanos, 520'000U);
  ASSERT_EQ(snapshot.position_count, 1U);
  EXPECT_EQ(snapshot.positions[0U].average_price_ticks, 110);
  EXPECT_EQ(snapshot.positions[0U].open_cost_currency_nanos, 220'000);
  EXPECT_EQ(snapshot.sectors[0U].net_exposure_currency_nanos, 260'000);
  EXPECT_EQ(snapshot.strategies[0U].pnl_currency_nanos, 39'990);
  EXPECT_EQ(snapshot.accounts[0U].pnl_currency_nanos, 39'990);
}

TEST(PortfolioServiceTest, UnknownDropCopyReconstructsButInhibitsUntilOrderReconciles) {
  Fixture fixture;
  ASSERT_EQ(fixture.service->apply(support::mark(1U)).status,
            portfolio::ApplyStatus::applied);
  const auto drop_copy = support::fill(2U, 1U, portfolio::Side::buy, 100, 10U, 0,
                                       portfolio::FillSource::drop_copy);
  ASSERT_EQ(fixture.service->apply(drop_copy).status, portfolio::ApplyStatus::applied);
  auto snapshot = fixture.service->snapshot_for_quiescent_inspection();
  EXPECT_EQ(snapshot.instruments[0U].net_quantity_units, 10);
  EXPECT_EQ(snapshot.orphan_fill_count, 1U);
  EXPECT_FALSE(snapshot.ready);
  ASSERT_EQ(
      fixture.service
          ->apply(support::order(3U, portfolio::OrderLifecycleState::open, 0U, 100U))
          .status,
      portfolio::ApplyStatus::applied);
  snapshot = fixture.service->snapshot_for_quiescent_inspection();
  EXPECT_EQ(snapshot.orphan_fill_count, 0U);
  EXPECT_TRUE(snapshot.ready);
}

TEST(PortfolioServiceTest, CancelledOrderReleasesPendingExposure) {
  Fixture fixture;
  ASSERT_EQ(fixture.service->apply(support::mark(1U)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(fixture.service->apply(support::order(2U)).status,
            portfolio::ApplyStatus::applied);
  EXPECT_EQ(fixture.service->snapshot_for_quiescent_inspection()
                .instruments[0U]
                .pending_buy_quantity_units,
            100U);
  ASSERT_EQ(
      fixture.service
          ->apply(support::order(3U, portfolio::OrderLifecycleState::cancelled, 0U, 0U))
          .status,
      portfolio::ApplyStatus::applied);
  EXPECT_EQ(fixture.service->snapshot_for_quiescent_inspection()
                .instruments[0U]
                .pending_buy_quantity_units,
            0U);
}

TEST(PortfolioServiceTest, CorrectionAndBustRebuildAccountingInApplicationOrder) {
  Fixture fixture;
  ASSERT_EQ(fixture.service->apply(support::mark(1U, 110)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(fixture.service->apply(support::order(2U)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(fixture.service->apply(support::fill(3U, 1U)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(fixture.service->apply(support::correction(4U, 1U, 90, 20U)).status,
            portfolio::ApplyStatus::applied);
  auto snapshot = fixture.service->snapshot_for_quiescent_inspection();
  EXPECT_EQ(snapshot.instruments[0U].net_quantity_units, 20);
  EXPECT_EQ(snapshot.unrealized_pnl_currency_nanos, 40'000);
  ASSERT_EQ(fixture.service->apply(support::bust(5U, 1U)).status,
            portfolio::ApplyStatus::applied);
  snapshot = fixture.service->snapshot_for_quiescent_inspection();
  EXPECT_EQ(snapshot.instruments[0U].net_quantity_units, 0);
  EXPECT_EQ(snapshot.total_pnl_currency_nanos, 0);
  EXPECT_EQ(snapshot.active_fill_count, 0U);
  EXPECT_EQ(fixture.service->metrics().corrections, 1U);
  EXPECT_EQ(fixture.service->metrics().busts, 1U);
}

TEST(PortfolioServiceTest, ConflictingDropCopyFailsClosedWithoutDoubleApplying) {
  Fixture fixture;
  ASSERT_EQ(fixture.service->apply(support::mark(1U)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(fixture.service->apply(support::order(2U)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(fixture.service->apply(support::fill(3U, 1U)).status,
            portfolio::ApplyStatus::applied);
  const auto conflict = support::fill(4U, 1U, portfolio::Side::buy, 101, 10U, 0,
                                      portfolio::FillSource::drop_copy);
  EXPECT_EQ(fixture.service->apply(conflict).status, portfolio::ApplyStatus::conflict);
  EXPECT_FALSE(fixture.service->ready());
  EXPECT_EQ(fixture.service->health(), portfolio::PortfolioHealth::unsafe);
  EXPECT_EQ(fixture.service->snapshot_for_quiescent_inspection()
                .instruments[0U]
                .net_quantity_units,
            10);
}

TEST(PortfolioServiceTest,
     AppliesExactSplitAndRequiresExternalReconciliationForOldBust) {
  Fixture fixture;
  ASSERT_EQ(fixture.service->apply(support::mark(1U, 100)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(fixture.service->apply(support::order(2U)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(fixture.service->apply(support::fill(3U, 1U)).status,
            portfolio::ApplyStatus::applied);
  portfolio::PortfolioEvent action;
  action.event_id = {20U, 4U};
  action.session_id = support::kSession;
  action.configuration_version = support::kConfiguration;
  action.instrument_id = support::kInstrument;
  action.kind = portfolio::PortfolioEventKind::corporate_action;
  action.corporate_action_kind = portfolio::CorporateActionKind::split;
  action.corporate_action_numerator = 2U;
  action.corporate_action_denominator = 1U;
  action.process_monotonic_time_ns = support::kStart + 4U;
  action = support::finalize(action);
  ASSERT_EQ(fixture.service->apply(action).status, portfolio::ApplyStatus::applied);
  const auto snapshot = fixture.service->snapshot_for_quiescent_inspection();
  EXPECT_EQ(snapshot.instruments[0U].net_quantity_units, 20);
  EXPECT_EQ(snapshot.instruments[0U].mark_price_ticks, 50);
  EXPECT_EQ(fixture.service->apply(support::bust(5U, 1U)).status,
            portfolio::ApplyStatus::requires_reconciliation);
}

TEST(PortfolioServiceTest, CarriesPositionsAcrossSessionAndRequiresFreshMarks) {
  Fixture fixture;
  ASSERT_EQ(fixture.service->apply(support::mark(1U)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(fixture.service->apply(support::order(2U)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(
      fixture.service->apply(support::fill(3U, 1U, portfolio::Side::buy, 100, 100U))
          .status,
      portfolio::ApplyStatus::applied);
  portfolio::PortfolioEvent rollover;
  rollover.event_id = {21U, 4U};
  rollover.session_id = support::kSession;
  rollover.next_session_id = support::kNextSession;
  rollover.configuration_version = support::kConfiguration;
  rollover.kind = portfolio::PortfolioEventKind::session_rollover;
  rollover.rollover_policy = portfolio::RolloverPolicy::carry_positions;
  rollover.process_monotonic_time_ns = support::kStart + 4U;
  rollover = support::finalize(rollover);
  ASSERT_EQ(fixture.service->apply(rollover).status, portfolio::ApplyStatus::applied);
  const auto snapshot = fixture.service->snapshot_for_quiescent_inspection();
  EXPECT_EQ(snapshot.session_id, support::kNextSession);
  EXPECT_EQ(snapshot.instruments[0U].net_quantity_units, 100);
  EXPECT_FALSE(snapshot.ready);
  EXPECT_EQ(snapshot.realized_pnl_currency_nanos, 0);
}

TEST(PortfolioServiceTest, EvaluatesAllSixFixedPointStressScenarios) {
  Fixture fixture;
  ASSERT_EQ(fixture.service->apply(support::mark(1U, 100)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(fixture.service->apply(support::order(2U)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(fixture.service->apply(support::fill(3U, 1U)).status,
            portfolio::ApplyStatus::applied);
  portfolio::StressConfiguration configuration;
  configuration.stable_hash =
      portfolio::stable_stress_configuration_hash(configuration);
  const auto result = fixture.service->evaluate_stress(configuration);
  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.outcomes[0U].pnl_impact_currency_nanos, -10'000);
  EXPECT_EQ(result.outcomes[1U].pnl_impact_currency_nanos, -15'000);
  EXPECT_EQ(result.outcomes[2U].pnl_impact_currency_nanos, -20'000);
  EXPECT_EQ(result.outcomes[3U].pnl_impact_currency_nanos, -30'000);
  EXPECT_EQ(result.outcomes[4U].pnl_impact_currency_nanos, -25'000);
  EXPECT_EQ(result.outcomes[5U].pnl_impact_currency_nanos, -7'000);
  EXPECT_EQ(result.stable_hash, portfolio::stable_stress_result_hash(result));
}

TEST(PortfolioServiceTest, ArithmeticOverflowFailsClosed) {
  auto overflow_configuration = support::configuration();
  overflow_configuration.instruments[0U].tick_value_currency_nanos =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  overflow_configuration.stable_hash =
      portfolio::stable_configuration_hash(overflow_configuration);
  auto overflow_journal = std::make_unique<portfolio::PortfolioJournal>();
  auto overflow_service = std::make_unique<portfolio::PortfolioRiskService>(
      overflow_configuration, *overflow_journal);
  ASSERT_EQ(overflow_service->apply(support::mark(1U)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(overflow_service->apply(support::order(2U)).status,
            portfolio::ApplyStatus::applied);
  EXPECT_EQ(overflow_service->apply(support::fill(3U, 1U)).status,
            portfolio::ApplyStatus::arithmetic_overflow);
  EXPECT_EQ(overflow_service->health(), portfolio::PortfolioHealth::unsafe);
}

TEST(PortfolioServiceTest, JournalExhaustionFailsClosed) {
  auto full_journal = std::make_unique<portfolio::PortfolioJournal>();
  for (std::size_t index = 0U; index < portfolio::kPortfolioJournalCapacity; ++index) {
    const auto reservation = full_journal->try_reserve();
    ASSERT_TRUE(reservation.valid);
    full_journal->commit(reservation, support::mark(1U), {});
  }
  auto blocked_service = std::make_unique<portfolio::PortfolioRiskService>(
      support::configuration(), *full_journal);
  EXPECT_EQ(blocked_service->apply(support::mark(1U)).status,
            portfolio::ApplyStatus::journal_unavailable);
  EXPECT_EQ(blocked_service->health(), portfolio::PortfolioHealth::unsafe);
}

TEST(PortfolioServiceTest, GracefulShutdownPublishesTerminalUnreadiness) {
  Fixture fixture;
  ASSERT_EQ(fixture.service->apply(support::mark(1U)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_TRUE(fixture.service->verify_invariants());
  EXPECT_FALSE(fixture.service->build_info().live_trading_capable);
  fixture.service->shutdown(support::kStart + 2U);
  EXPECT_EQ(fixture.service->health(), portfolio::PortfolioHealth::stopped);
  EXPECT_FALSE(fixture.service->ready());
  EXPECT_EQ(fixture.service->apply(support::mark(3U)).status,
            portfolio::ApplyStatus::stopped);
}

} // namespace
