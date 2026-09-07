#include "aegis/high_availability/coordinator.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

namespace aegis::high_availability::test {
namespace {

// Test fixture members intentionally remain directly visible to keep scenario
// assertions compact; production layout is unaffected.
// NOLINTBEGIN(clang-analyzer-optin.performance.Padding)
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
// NOLINTBEGIN(readability-make-member-function-const)
struct Harness {
  HaConfiguration config{configuration()};
  event_bus::ProcessEpochState epoch_state;
  std::unique_ptr<BoundedRecoveryStream> recovery{
      std::make_unique<BoundedRecoveryStream>(RecoveryAuthority{
          .session_id = kSession, .exchange_session_epoch = kExchangeEpoch})};
  std::unique_ptr<EdgeHaCoordinator> coordinator{
      std::make_unique<EdgeHaCoordinator>(config, epoch_state)};

  void activate(const std::uint64_t token = 10U) {
    ASSERT_TRUE(coordinator->start(kStart));
    coordinator->update_health(healthy(kStart + 10U), recovery->snapshot());
    ASSERT_EQ(coordinator->apply_fencing_grant(
                  grant(config, token, 0U, 1U, kStart + 11U, kStart + 400U),
                  kStart + 11U, *recovery),
              GrantStatus::accepted);
    coordinator->update_health(healthy(kStart + 20U), recovery->snapshot());
    auto proof = evidence(token, kStart + 21U, recovery->snapshot());
    ASSERT_EQ(coordinator->complete_reconciliation(proof, recovery->snapshot()),
              ReconciliationStatus::completed);
  }
};
// NOLINTEND(readability-make-member-function-const)
// NOLINTEND(misc-non-private-member-variables-in-classes)
// NOLINTEND(clang-analyzer-optin.performance.Padding)

TEST(HaCoordinatorTest, StartsAsHotStandbyAndPublishesOperatorStateAtomically) {
  Harness harness;
  ASSERT_TRUE(harness.coordinator->start(kStart));
  harness.coordinator->update_health(healthy(kStart + 10U),
                                     harness.recovery->snapshot());
  const auto snapshot = harness.coordinator->snapshot();
  EXPECT_EQ(snapshot.state, LeadershipState::hot_standby);
  EXPECT_FALSE(snapshot.can_emit_orders);
  EXPECT_EQ(snapshot.stable_hash, stable_leadership_snapshot_hash(snapshot));
  LeadershipSnapshotStore::ReadHandle read;
  ASSERT_EQ(harness.coordinator->acquire_snapshot(kStart + 11U, read),
            event_bus::SnapshotReadStatus::snapshot);
  ASSERT_TRUE(read.valid());
  EXPECT_EQ(read->state, LeadershipState::hot_standby);
}

TEST(HaCoordinatorTest, RequiresEveryReconciliationDomainBeforeLeadership) {
  Harness harness;
  ASSERT_TRUE(harness.coordinator->start(kStart));
  harness.coordinator->update_health(healthy(kStart + 10U),
                                     harness.recovery->snapshot());
  ASSERT_EQ(harness.coordinator->apply_fencing_grant(
                grant(harness.config, 10U, 0U, 1U, kStart + 11U, kStart + 400U),
                kStart + 11U, *harness.recovery),
            GrantStatus::accepted);
  harness.coordinator->update_health(healthy(kStart + 20U),
                                     harness.recovery->snapshot());
  auto incomplete = evidence(10U, kStart + 21U, harness.recovery->snapshot());
  incomplete.gateway_reconciled = false;
  incomplete.stable_hash = stable_reconciliation_evidence_hash(incomplete);
  EXPECT_EQ(harness.coordinator->complete_reconciliation(incomplete,
                                                         harness.recovery->snapshot()),
            ReconciliationStatus::incomplete);
  EXPECT_FALSE(harness.coordinator->snapshot().can_emit_orders);
}

TEST(HaCoordinatorTest, StandbyFailureDegradesRedundancyWithoutInventingAmbiguity) {
  Harness harness;
  harness.activate();
  auto no_standby = healthy(kStart + 30U, 7U, false);
  harness.coordinator->update_health(no_standby, harness.recovery->snapshot());
  const auto snapshot = harness.coordinator->snapshot();
  EXPECT_EQ(snapshot.state, LeadershipState::active_degraded);
  EXPECT_TRUE(snapshot.can_emit_orders);
  EXPECT_TRUE(snapshot.operator_attention_required);
}

TEST(HaCoordinatorTest, SplitBrainNetworkPartitionAndDelayedHeartbeatFence) {
  {
    Harness harness;
    harness.activate();
    auto split = healthy(kStart + 30U);
    split.peer.state = LeadershipState::active_leader;
    split.peer.fencing_token = 11U;
    harness.coordinator->update_health(split, harness.recovery->snapshot());
    EXPECT_EQ(harness.coordinator->snapshot().state, LeadershipState::fenced);
    EXPECT_FALSE(harness.coordinator->snapshot().can_emit_orders);
  }
  {
    Harness harness;
    harness.activate();
    ASSERT_TRUE(harness.coordinator->heartbeat(kStart + 350U));
    harness.coordinator->update_health(healthy(kStart + 401U),
                                       harness.recovery->snapshot());
    EXPECT_EQ(harness.coordinator->snapshot().reason,
              LeadershipReason::fencing_lease_expired);
    EXPECT_FALSE(harness.coordinator->snapshot().can_emit_orders);
  }
  {
    Harness harness;
    harness.activate();
    EXPECT_FALSE(harness.coordinator->heartbeat(kStart + 501U));
    EXPECT_EQ(harness.coordinator->snapshot().reason,
              LeadershipReason::delayed_local_heartbeat);
  }
}

TEST(HaCoordinatorTest, DependencyFailuresRequireExplicitRecovery) {
  {
    Harness harness;
    harness.activate();
    auto health = healthy(kStart + 30U);
    health.gateway_connected = false;
    harness.coordinator->update_health(health, harness.recovery->snapshot());
    EXPECT_EQ(harness.coordinator->snapshot().state,
              LeadershipState::leader_reconciling);
    harness.coordinator->update_health(healthy(kStart + 31U),
                                       harness.recovery->snapshot());
    EXPECT_EQ(harness.coordinator->snapshot().state,
              LeadershipState::leader_reconciling);
  }
  {
    Harness harness;
    harness.activate();
    auto health = healthy(kStart + 30U, 8U);
    harness.coordinator->update_health(health, harness.recovery->snapshot());
    EXPECT_EQ(harness.coordinator->snapshot().reason,
              LeadershipReason::risk_epoch_changed);
  }
  {
    Harness harness;
    harness.activate();
    auto health = healthy(kStart + 30U);
    health.journal_ready = false;
    harness.coordinator->update_health(health, harness.recovery->snapshot());
    EXPECT_EQ(harness.coordinator->snapshot().state, LeadershipState::unsafe);
    harness.coordinator->update_health(healthy(kStart + 31U),
                                       harness.recovery->snapshot());
    EXPECT_EQ(harness.coordinator->snapshot().state, LeadershipState::unsafe);
  }
  {
    Harness harness;
    harness.activate();
    auto health = healthy(kStart + 30U);
    health.shared_memory_current = false;
    harness.coordinator->update_health(health, harness.recovery->snapshot());
    EXPECT_EQ(harness.coordinator->snapshot().reason,
              LeadershipReason::shared_memory_stale);
    harness.coordinator->update_health(healthy(kStart + 31U),
                                       harness.recovery->snapshot());
    EXPECT_EQ(harness.coordinator->snapshot().state, LeadershipState::fenced);
  }
  {
    Harness harness;
    harness.activate();
    auto health = healthy(kStart + 30U);
    health.partial_colocation_outage = true;
    harness.coordinator->update_health(health, harness.recovery->snapshot());
    EXPECT_EQ(harness.coordinator->snapshot().reason,
              LeadershipReason::partial_colocation_outage);
  }
}

// GoogleTest assertion macros dominate the reported score; the explicit loop
// is the table of independently corrupted grant cases under test.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HaCoordinatorTest, MalformedAndStaleGrantsNeverCreateASecondLeader) {
  Harness harness;
  ASSERT_TRUE(harness.coordinator->start(kStart));
  harness.coordinator->update_health(healthy(kStart + 10U),
                                     harness.recovery->snapshot());
  const FencingGrantVerifier verifier(
      {.witness_public_key = harness.config.witness_public_key,
       .witness_key_id_sha256 = harness.config.witness_key_id_sha256,
       .witness_trust_root_sha256 = harness.config.witness_trust_root_sha256});
  for (std::uint64_t mutation = 0U; mutation < 5U; ++mutation) {
    auto bad = signed_grant(harness.config, 10U, 0U, 1U, kStart + 11U, kStart + 400U);
    if (mutation == 0U) {
      bad.quorum_confirmed = false;
    } else if (mutation == 1U) {
      bad.previous_owner_fenced = false;
    } else if (mutation == 2U) {
      bad.node_id += 1U;
    } else if (mutation == 3U) {
      bad.valid_until_process_monotonic_time_ns = kStart + 10U;
    } else {
      bad.stable_hash ^= 1U;
    }
    if (mutation != 4U) {
      ASSERT_TRUE(sign_fencing_grant(bad, kWitnessPrivateKey));
    } else {
      bad.witness_signature.front() ^= 1U;
      bad.stable_hash = stable_fencing_grant_hash(bad);
    }
    VerifiedFencingGrant verified;
    if (verifier.verify(bad, kStart + 11U, verified)) {
      EXPECT_NE(harness.coordinator->apply_fencing_grant(verified, kStart + 11U,
                                                         *harness.recovery),
                GrantStatus::accepted);
    }
    EXPECT_FALSE(harness.coordinator->snapshot().can_emit_orders);
  }
}

TEST(HaCoordinatorTest, RejectsSelfAssertedGrantWithRecomputedPublicHash) {
  Harness harness;
  ASSERT_TRUE(harness.coordinator->start(kStart));
  harness.coordinator->update_health(healthy(kStart + 10U),
                                     harness.recovery->snapshot());

  // This is deliberately created without witness-held signing authority. The
  // old boundary accepted it because every asserted fact and the FNV hash were
  // caller controlled.
  auto self_asserted =
      signed_grant(harness.config, 10U, 0U, 1U, kStart + 11U, kStart + 400U);
  self_asserted.witness_signature.fill(0U);
  self_asserted.stable_hash = stable_fencing_grant_hash(self_asserted);
  const FencingGrantVerifier verifier(
      {.witness_public_key = harness.config.witness_public_key,
       .witness_key_id_sha256 = harness.config.witness_key_id_sha256,
       .witness_trust_root_sha256 = harness.config.witness_trust_root_sha256});
  VerifiedFencingGrant verified;
  EXPECT_FALSE(verifier.verify(self_asserted, kStart + 11U, verified));
  EXPECT_FALSE(verified.valid());
  EXPECT_FALSE(harness.coordinator->snapshot().can_emit_orders);
}

TEST(HaCoordinatorTest, RemoteRegionIsPermanentlyRiskReductionOnly) {
  const auto config = configuration(9U, 901U, 1U, DeploymentLocality::remote_region);
  event_bus::ProcessEpochState epoch;
  auto recovery = std::make_unique<BoundedRecoveryStream>(RecoveryAuthority{
      .session_id = kSession, .exchange_session_epoch = kExchangeEpoch});
  auto coordinator = std::make_unique<EdgeHaCoordinator>(config, epoch);
  ASSERT_TRUE(coordinator->start(kStart));
  coordinator->update_health(healthy(kStart + 10U), recovery->snapshot());
  ASSERT_EQ(coordinator->apply_fencing_grant(
                grant(config, 10U, 0U, 1U, kStart + 11U, kStart + 400U), kStart + 11U,
                *recovery),
            GrantStatus::accepted);
  EXPECT_EQ(coordinator->snapshot().state, LeadershipState::risk_reduction_only);
  const auto denied = coordinator->authorize_emission(
      emission(config, 10U, 1U, kStart + 20U), *recovery);
  EXPECT_EQ(denied.status, EmissionStatus::not_leader);
}

} // namespace
} // namespace aegis::high_availability::test
