#include "portfolio_test_support.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace portfolio = aegis::risk::portfolio;
namespace support = aegis::risk::portfolio::test;

namespace {

void read_snapshots(portfolio::PortfolioRiskService& service,
                    const std::atomic<bool>& start, const std::atomic<bool>& done,
                    std::atomic<std::uint64_t>& failures) {
  while (!start.load(std::memory_order_acquire)) {
  }
  while (!done.load(std::memory_order_acquire)) {
    portfolio::PortfolioSnapshotStore::ReadHandle handle;
    const auto status = service.acquire_snapshot(support::kStart + 10'000U, handle);
    if (status == aegis::event_bus::SnapshotReadStatus::snapshot &&
        handle->stable_hash != portfolio::stable_snapshot_hash(*handle)) {
      failures.fetch_add(1U, std::memory_order_relaxed);
    }
  }
}

[[nodiscard]] bool publish_marks(portfolio::PortfolioRiskService& service) {
  for (std::uint64_t ordinal = 2U; ordinal < 300U; ++ordinal) {
    const auto result = service.apply(
        support::mark(ordinal, 100 + static_cast<std::int64_t>(ordinal % 7U)));
    if (result.status != portfolio::ApplyStatus::applied) {
      return false;
    }
  }
  return true;
}

TEST(PortfolioReplayTest, ReconstructsIdenticalPositionsAndHashChainFromJournal) {
  auto source_journal = std::make_unique<portfolio::PortfolioJournal>();
  auto source = std::make_unique<portfolio::PortfolioRiskService>(
      support::configuration(), *source_journal);
  ASSERT_EQ(source->apply(support::mark(1U, 125)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(source->apply(support::order(2U)).status, portfolio::ApplyStatus::applied);
  ASSERT_EQ(
      source->apply(support::fill(3U, 1U, portfolio::Side::buy, 100, 10U, 3)).status,
      portfolio::ApplyStatus::applied);
  ASSERT_EQ(source->apply(support::correction(4U, 1U, 105, 12U)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(source->apply(support::mark(5U, 130)).status,
            portfolio::ApplyStatus::applied);
  ASSERT_EQ(source
                ->apply(support::order(6U, portfolio::OrderLifecycleState::cancelled,
                                       12U, 0U))
                .status,
            portfolio::ApplyStatus::applied);
  portfolio::PortfolioEvent rollover;
  rollover.event_id = {30U, 7U};
  rollover.session_id = support::kSession;
  rollover.next_session_id = support::kNextSession;
  rollover.configuration_version = support::kConfiguration;
  rollover.kind = portfolio::PortfolioEventKind::session_rollover;
  rollover.rollover_policy = portfolio::RolloverPolicy::carry_positions;
  rollover.process_monotonic_time_ns = support::kStart + 7U;
  rollover = support::finalize(rollover);
  ASSERT_EQ(source->apply(rollover).status, portfolio::ApplyStatus::applied);
  auto next_mark = support::mark(8U, 131);
  next_mark.session_id = support::kNextSession;
  next_mark.stable_hash = portfolio::stable_event_hash(next_mark);
  ASSERT_EQ(source->apply(next_mark).status, portfolio::ApplyStatus::applied);

  auto target_journal = std::make_unique<portfolio::PortfolioJournal>();
  auto target = std::make_unique<portfolio::PortfolioRiskService>(
      support::configuration(), *target_journal);
  ASSERT_EQ(target->recover_from(*source_journal),
            portfolio::RecoveryStatus::recovered);
  const auto expected = source->snapshot_for_quiescent_inspection();
  const auto actual = target->snapshot_for_quiescent_inspection();
  EXPECT_EQ(actual.stable_hash, expected.stable_hash);
  EXPECT_EQ(actual.instruments[0U].net_quantity_units,
            expected.instruments[0U].net_quantity_units);
  EXPECT_EQ(actual.realized_pnl_currency_nanos, expected.realized_pnl_currency_nanos);
  EXPECT_EQ(actual.unrealized_pnl_currency_nanos,
            expected.unrealized_pnl_currency_nanos);
  EXPECT_EQ(target_journal->size(), source_journal->size());
  EXPECT_EQ(target_journal->last_record_hash(), source_journal->last_record_hash());
}

TEST(PortfolioJournalTest, PublishesOnlyCompleteHashChainedRecords) {
  portfolio::PortfolioJournal journal;
  const auto reservation = journal.try_reserve();
  ASSERT_TRUE(reservation.valid);
  portfolio::PortfolioJournalRecord record;
  EXPECT_FALSE(journal.read(1U, record));
  const auto event = support::mark(1U);
  const portfolio::ApplyResult result{.status = portfolio::ApplyStatus::applied,
                                      .journal_sequence = 1U,
                                      .snapshot_sequence = 2U,
                                      .snapshot_hash = 3U};
  journal.commit(reservation, event, result);
  ASSERT_TRUE(journal.read(1U, record));
  EXPECT_EQ(record.sequence, 1U);
  EXPECT_EQ(record.previous_record_hash, 0U);
  EXPECT_EQ(record.stable_hash, portfolio::stable_journal_record_hash(record));
}

TEST(PortfolioSnapshotTest, ConcurrentReadersObserveOnlyValidImmutableVersions) {
  auto journal = std::make_unique<portfolio::PortfolioJournal>();
  auto service = std::make_unique<portfolio::PortfolioRiskService>(
      support::configuration(), *journal);
  ASSERT_EQ(service->apply(support::mark(1U)).status, portfolio::ApplyStatus::applied);
  std::atomic<bool> start{false};
  std::atomic<bool> done{false};
  std::atomic<std::uint64_t> failures{0U};
  std::vector<std::thread> readers;
  readers.reserve(2U);
  for (std::size_t reader = 0U; reader < 2U; ++reader) {
    readers.emplace_back(read_snapshots, std::ref(*service), std::cref(start),
                         std::cref(done), std::ref(failures));
  }
  start.store(true, std::memory_order_release);
  EXPECT_TRUE(publish_marks(*service));
  done.store(true, std::memory_order_release);
  for (auto& reader : readers) {
    reader.join();
  }
  EXPECT_EQ(failures.load(std::memory_order_relaxed), 0U);
}

} // namespace
