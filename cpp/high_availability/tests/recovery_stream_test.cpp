#include "aegis/high_availability/recovery_stream.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace aegis::high_availability::test {
namespace {

TEST(RecoveryStreamTest, PublishesAndAppliesHashChainedRecords) {
  const RecoveryAuthority authority{.session_id = kSession,
                                    .exchange_session_epoch = kExchangeEpoch,
                                    .fencing_token = 10U};
  auto stream = std::make_unique<BoundedRecoveryStream>(authority);
  const auto first =
      stream->append(payload(authority, RecoveryRecordKind::journal_commit, 1U));
  const auto second =
      stream->append(payload(authority, RecoveryRecordKind::risk_snapshot, 2U));
  ASSERT_EQ(first.status, RecoveryAppendStatus::accepted);
  ASSERT_EQ(second.status, RecoveryAppendStatus::accepted);
  EXPECT_FALSE(stream->snapshot().caught_up);
  EXPECT_EQ(stream->apply_next(), RecoveryApplyStatus::applied);
  EXPECT_EQ(stream->apply_next(), RecoveryApplyStatus::applied);
  EXPECT_EQ(stream->apply_next(), RecoveryApplyStatus::empty);
  const auto snapshot = stream->snapshot();
  EXPECT_TRUE(snapshot.healthy);
  EXPECT_TRUE(snapshot.caught_up);
  EXPECT_EQ(snapshot.published_sequence, 2U);
  EXPECT_EQ(snapshot.published_hash, snapshot.applied_hash);
  EXPECT_EQ(snapshot.stable_hash, stable_recovery_snapshot_hash(snapshot));
}

TEST(RecoveryStreamTest, DuplicateAndConflictingCommandIdentitiesFailClosed) {
  const RecoveryAuthority authority{.session_id = kSession,
                                    .exchange_session_epoch = kExchangeEpoch,
                                    .fencing_token = 10U};
  auto stream = std::make_unique<BoundedRecoveryStream>(authority);
  const common::GlobalEventId command{5U, 1U};
  const auto emission =
      payload(authority, RecoveryRecordKind::order_emission, 1U, command, 90U);
  EXPECT_EQ(stream->append(emission).status, RecoveryAppendStatus::accepted);
  EXPECT_EQ(stream->append(emission).status, RecoveryAppendStatus::duplicate);
  EXPECT_EQ(stream->apply_next(), RecoveryApplyStatus::applied);
  EXPECT_TRUE(stream->applied_command(command, 90U));

  const auto conflict =
      payload(authority, RecoveryRecordKind::order_emission, 2U, command, 91U);
  EXPECT_EQ(stream->append(conflict).status, RecoveryAppendStatus::identity_conflict);
  EXPECT_FALSE(stream->snapshot().healthy);
}

TEST(RecoveryStreamTest, AcknowledgementRequiresReplicatedEmission) {
  const RecoveryAuthority authority{.session_id = kSession,
                                    .exchange_session_epoch = kExchangeEpoch,
                                    .fencing_token = 10U};
  auto stream = std::make_unique<BoundedRecoveryStream>(authority);
  const auto acknowledgement =
      payload(authority, RecoveryRecordKind::gateway_acknowledgement, 1U,
              common::GlobalEventId{5U, 2U}, 92U);
  EXPECT_EQ(stream->append(acknowledgement).status,
            RecoveryAppendStatus::identity_conflict);
  EXPECT_FALSE(stream->snapshot().healthy);
}

TEST(RecoveryStreamTest, AuthorityRotationRequiresCaughtUpReplica) {
  const RecoveryAuthority first{.session_id = kSession,
                                .exchange_session_epoch = kExchangeEpoch,
                                .fencing_token = 10U};
  const RecoveryAuthority second{.session_id = kSession,
                                 .exchange_session_epoch = kExchangeEpoch,
                                 .fencing_token = 11U};
  auto stream = std::make_unique<BoundedRecoveryStream>(first);
  ASSERT_EQ(
      stream->append(payload(first, RecoveryRecordKind::journal_commit, 1U)).status,
      RecoveryAppendStatus::accepted);
  EXPECT_EQ(stream->rotate_authority(second), RecoveryRotateStatus::not_caught_up);
  ASSERT_EQ(stream->apply_next(), RecoveryApplyStatus::applied);
  EXPECT_EQ(stream->rotate_authority(second), RecoveryRotateStatus::rotated);
  EXPECT_EQ(
      stream->append(payload(first, RecoveryRecordKind::journal_commit, 2U)).status,
      RecoveryAppendStatus::fenced);
}

TEST(RecoveryStreamTest, BoundedOverloadInvalidatesMandatoryRecoveryPath) {
  const RecoveryAuthority authority{.session_id = kSession,
                                    .exchange_session_epoch = kExchangeEpoch,
                                    .fencing_token = 10U};
  auto stream = std::make_unique<BoundedRecoveryStream>(authority);
  for (std::uint64_t ordinal = 1U; ordinal <= kRecoveryStreamCapacity; ++ordinal) {
    ASSERT_EQ(
        stream->append(payload(authority, RecoveryRecordKind::journal_commit, ordinal))
            .status,
        RecoveryAppendStatus::accepted);
  }
  EXPECT_EQ(stream
                ->append(payload(authority, RecoveryRecordKind::journal_commit,
                                 kRecoveryStreamCapacity + 1U))
                .status,
            RecoveryAppendStatus::full);
  EXPECT_FALSE(stream->snapshot().healthy);
}

// Assertions and the two worker lambdas intentionally make the test read as a
// complete concurrency scenario.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(RecoveryStreamTest, SustainedSpscReplicationPreservesSequenceAndHash) {
  constexpr std::uint64_t kRecords = 1'000U;
  const RecoveryAuthority authority{.session_id = kSession,
                                    .exchange_session_epoch = kExchangeEpoch,
                                    .fencing_token = 10U};
  auto stream = std::make_unique<BoundedRecoveryStream>(authority);
  std::atomic<bool> begin{false};
  std::atomic<bool> producer_ok{true};
  std::atomic<bool> consumer_ok{true};

  std::thread producer([&] {
    while (!begin.load(std::memory_order_acquire)) {
    }
    for (std::uint64_t ordinal = 1U; ordinal <= kRecords; ++ordinal) {
      if (stream
              ->append(payload(authority, RecoveryRecordKind::journal_commit, ordinal))
              .status != RecoveryAppendStatus::accepted) {
        producer_ok.store(false, std::memory_order_relaxed);
        return;
      }
    }
  });
  std::thread consumer([&] {
    begin.store(true, std::memory_order_release);
    std::uint64_t applied{};
    while (applied < kRecords) {
      const auto result = stream->apply_next();
      if (result == RecoveryApplyStatus::applied) {
        ++applied;
      } else if (result != RecoveryApplyStatus::empty) {
        consumer_ok.store(false, std::memory_order_relaxed);
        return;
      }
    }
  });
  producer.join();
  consumer.join();

  EXPECT_TRUE(producer_ok.load(std::memory_order_relaxed));
  EXPECT_TRUE(consumer_ok.load(std::memory_order_relaxed));
  const auto snapshot = stream->snapshot();
  EXPECT_TRUE(snapshot.caught_up);
  EXPECT_EQ(snapshot.applied_sequence, kRecords);
  EXPECT_EQ(snapshot.published_hash, snapshot.applied_hash);
}

} // namespace
} // namespace aegis::high_availability::test
