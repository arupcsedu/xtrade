#include "test_support.hpp"

#include "aegis/journal/async_journal.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace aegis::journal {
namespace {

// GoogleTest assertion macros inflate integration tests' apparent branch counts.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(AsyncJournal, AdvisoryOverflowRejectsWithoutOverwrite) {
  const test::TemporaryDirectory temporary;
  const auto configuration = test::config(temporary.path() / "journal");
  AsyncJournal journal;
  ASSERT_EQ(journal.open(configuration, BackpressurePolicy::reject_newest), Status::ok);
  const std::array<std::uint8_t, 1> payload{0x42U};
  for (std::uint64_t index = 0U; index < kAsyncQueueCapacity; ++index) {
    const auto result = journal.try_publish(
        {.metadata =
             test::metadata(index, RecordKind::raw_packet_metadata,
                            PayloadEncoding::opaque_binary, RecordPriority::advisory),
         .payload = payload});
    ASSERT_EQ(result.status, Status::ok);
  }
  const auto overflow = journal.try_publish(
      {.metadata =
           test::metadata(999U, RecordKind::raw_packet_metadata,
                          PayloadEncoding::opaque_binary, RecordPriority::advisory),
       .payload = payload});
  EXPECT_EQ(overflow.status, Status::queue_full);
  std::size_t drained{};
  EXPECT_EQ(journal.drain_once(kAsyncQueueCapacity, drained), Status::ok);
  EXPECT_EQ(drained, kAsyncQueueCapacity);
  EXPECT_EQ(journal.shutdown(std::chrono::seconds(1)), Status::ok);
  const auto report = RecoveryScanner::scan_directory(configuration.directory);
  EXPECT_EQ(report.status, Status::ok);
  EXPECT_EQ(report.valid_record_count, kAsyncQueueCapacity);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(AsyncJournal, MandatoryOverflowPermanentlyInhibitsPublisher) {
  const test::TemporaryDirectory temporary;
  const auto configuration = test::config(temporary.path() / "journal");
  AsyncJournal journal;
  ASSERT_EQ(journal.open(configuration, BackpressurePolicy::reject_newest), Status::ok);
  const std::array<std::uint8_t, 1> payload{0x42U};
  for (std::uint64_t index = 0U; index < kAsyncQueueCapacity; ++index) {
    ASSERT_EQ(
        journal.try_publish({.metadata = test::metadata(index), .payload = payload})
            .status,
        Status::ok);
  }
  EXPECT_EQ(journal.try_publish({.metadata = test::metadata(999U), .payload = payload})
                .status,
            Status::inhibited);
  EXPECT_EQ(journal.health(), HealthState::unsafe);
  EXPECT_EQ(
      journal.try_publish({.metadata = test::metadata(1'000U), .payload = payload})
          .status,
      Status::inhibited);
  EXPECT_EQ(journal.shutdown(std::chrono::seconds(2)), Status::inhibited);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(AsyncJournal, MultipleProducersPersistThroughOneWriter) {
  const test::TemporaryDirectory temporary;
  const auto configuration = test::config(temporary.path() / "journal");
  AsyncJournal journal;
  ASSERT_EQ(journal.open(configuration), Status::ok);
  ASSERT_EQ(journal.start(), Status::ok);
  constexpr std::uint64_t kProducerCount = 4U;
  constexpr std::uint64_t kRecordsPerProducer = 32U;
  std::vector<std::thread> producers;
  producers.reserve(kProducerCount);
  for (std::uint64_t producer = 0U; producer < kProducerCount; ++producer) {
    producers.emplace_back([&, producer] {
      const std::array<std::uint8_t, 8> payload{
          static_cast<std::uint8_t>(producer), 1U, 2U, 3U, 4U, 5U, 6U, 7U};
      for (std::uint64_t index = 0U; index < kRecordsPerProducer; ++index) {
        const auto ordinal = (producer * kRecordsPerProducer) + index;
        const auto result = journal.try_publish(
            {.metadata = test::metadata(ordinal), .payload = payload});
        EXPECT_EQ(result.status, Status::ok);
      }
    });
  }
  for (auto& producer : producers) {
    producer.join();
  }
  ASSERT_EQ(journal.shutdown(std::chrono::seconds(5)), Status::ok);
  const auto report = RecoveryScanner::scan_directory(configuration.directory);
  EXPECT_EQ(report.status, Status::ok);
  EXPECT_EQ(report.valid_record_count, kProducerCount * kRecordsPerProducer);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(AsyncJournal, OversizedMandatoryRecordFailsClosedWithoutAllocationFallback) {
  const test::TemporaryDirectory temporary;
  const auto configuration = test::config(temporary.path() / "journal");
  AsyncJournal journal;
  ASSERT_EQ(journal.open(configuration), Status::ok);
  std::vector<std::uint8_t> oversized(kMaximumHotPayloadBytes + 1U, 0x1U);
  EXPECT_EQ(journal.try_publish({.metadata = test::metadata(1U), .payload = oversized})
                .status,
            Status::inhibited);
  EXPECT_EQ(journal.health(), HealthState::unsafe);
  EXPECT_EQ(journal.shutdown(std::chrono::nanoseconds(0)), Status::inhibited);
}

} // namespace
} // namespace aegis::journal
