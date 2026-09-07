#include "aegis/high_availability/coordinator.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace aegis::high_availability::test {
namespace {

TEST(FailoverIntegrationTest,
     LeaderCrashPromotesCaughtUpStandbyWithoutResendingAcknowledgedOrder) {
  const auto old_config = configuration(1U, 101U, 1U);
  const auto new_config = configuration(2U, 202U, 2U);
  event_bus::ProcessEpochState old_epoch;
  event_bus::ProcessEpochState new_epoch;
  auto recovery = std::make_unique<BoundedRecoveryStream>(RecoveryAuthority{
      .session_id = kSession, .exchange_session_epoch = kExchangeEpoch});
  auto old_leader = std::make_unique<EdgeHaCoordinator>(old_config, old_epoch);
  ASSERT_TRUE(old_leader->start(kStart));
  old_leader->update_health(healthy(kStart + 10U), recovery->snapshot());
  ASSERT_EQ(old_leader->apply_fencing_grant(
                grant(old_config, 10U, 0U, 1U, kStart + 11U, kStart + 400U),
                kStart + 11U, *recovery),
            GrantStatus::accepted);
  old_leader->update_health(healthy(kStart + 20U), recovery->snapshot());
  ASSERT_EQ(
      old_leader->complete_reconciliation(
          evidence(10U, kStart + 21U, recovery->snapshot()), recovery->snapshot()),
      ReconciliationStatus::completed);

  const auto original = emission(old_config, 10U, 1U, kStart + 30U, 9'001U);
  ASSERT_EQ(old_leader->authorize_emission(original, *recovery).status,
            EmissionStatus::permitted);
  ASSERT_EQ(recovery->apply_next(), RecoveryApplyStatus::applied);
  auto acknowledgement = payload({.session_id = kSession,
                                  .exchange_session_epoch = kExchangeEpoch,
                                  .fencing_token = 10U},
                                 RecoveryRecordKind::gateway_acknowledgement, 2U,
                                 original.command_id, original.command_hash);
  ASSERT_EQ(recovery->append(acknowledgement).status, RecoveryAppendStatus::accepted);
  ASSERT_EQ(recovery->apply_next(), RecoveryApplyStatus::applied);
  ASSERT_TRUE(recovery->snapshot().caught_up);

  // The witness has fenced the crashed owner and advances the token. The new
  // process may lead only after exact recovery and service reconciliation.
  auto standby = std::make_unique<EdgeHaCoordinator>(new_config, new_epoch);
  ASSERT_TRUE(standby->start(kStart + 40U));
  auto new_health = healthy(kStart + 41U, 7U, false);
  standby->update_health(new_health, recovery->snapshot());
  ASSERT_EQ(standby->apply_fencing_grant(
                grant(new_config, 11U, 10U, 2U, kStart + 42U, kStart + 450U),
                kStart + 42U, *recovery),
            GrantStatus::accepted);
  new_health.observed_process_monotonic_time_ns = kStart + 43U;
  standby->update_health(new_health, recovery->snapshot());
  ASSERT_EQ(
      standby->complete_reconciliation(
          evidence(11U, kStart + 44U, recovery->snapshot()), recovery->snapshot()),
      ReconciliationStatus::completed);

  auto repeated = emission(new_config, 11U, 1U, kStart + 45U, 9'001U);
  repeated.command_id = original.command_id;
  repeated.source_journal_sequence = 3U;
  repeated.source_journal_hash = 10'003U;
  repeated.stable_hash = stable_emission_request_hash(repeated);
  EXPECT_EQ(standby->authorize_emission(repeated, *recovery).status,
            EmissionStatus::duplicate_suppressed);

  // A paused old process retains local memory but not recovery/gateway
  // authority. Its next attempted emission is fenced and latches unsafe.
  const auto stale = old_leader->authorize_emission(
      emission(old_config, 10U, 2U, kStart + 46U, 9'002U), *recovery);
  EXPECT_EQ(stale.status, EmissionStatus::stale_authority);
  EXPECT_EQ(old_leader->snapshot().state, LeadershipState::unsafe);
}

TEST(FailoverIntegrationTest, RecoveryLagBlocksPromotionAndPreventsDualEmitters) {
  const auto leader_config = configuration(1U, 101U, 1U);
  const auto standby_config = configuration(2U, 202U, 2U);
  event_bus::ProcessEpochState leader_epoch;
  event_bus::ProcessEpochState standby_epoch;
  auto recovery = std::make_unique<BoundedRecoveryStream>(RecoveryAuthority{
      .session_id = kSession, .exchange_session_epoch = kExchangeEpoch});
  auto leader = std::make_unique<EdgeHaCoordinator>(leader_config, leader_epoch);
  ASSERT_TRUE(leader->start(kStart));
  leader->update_health(healthy(kStart + 10U), recovery->snapshot());
  ASSERT_EQ(leader->apply_fencing_grant(
                grant(leader_config, 10U, 0U, 1U, kStart + 11U, kStart + 400U),
                kStart + 11U, *recovery),
            GrantStatus::accepted);
  leader->update_health(healthy(kStart + 20U), recovery->snapshot());
  ASSERT_EQ(
      leader->complete_reconciliation(evidence(10U, kStart + 21U, recovery->snapshot()),
                                      recovery->snapshot()),
      ReconciliationStatus::completed);
  ASSERT_EQ(leader
                ->authorize_emission(emission(leader_config, 10U, 1U, kStart + 30U),
                                     *recovery)
                .status,
            EmissionStatus::permitted);
  ASSERT_FALSE(recovery->snapshot().caught_up);

  auto standby = std::make_unique<EdgeHaCoordinator>(standby_config, standby_epoch);
  ASSERT_TRUE(standby->start(kStart + 40U));
  standby->update_health(healthy(kStart + 41U, 7U, false), recovery->snapshot());
  EXPECT_EQ(standby->apply_fencing_grant(
                grant(standby_config, 11U, 10U, 2U, kStart + 42U, kStart + 450U),
                kStart + 42U, *recovery),
            GrantStatus::recovery_stream_not_ready);
  EXPECT_FALSE(standby->snapshot().can_emit_orders);
  EXPECT_EQ(standby
                ->authorize_emission(emission(standby_config, 11U, 2U, kStart + 43U),
                                     *recovery)
                .status,
            EmissionStatus::not_leader);
}

} // namespace
} // namespace aegis::high_availability::test
