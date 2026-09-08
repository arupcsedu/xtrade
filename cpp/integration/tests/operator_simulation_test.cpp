#include "aegis/integration/operator_simulation.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace {

TEST(OperatorSimulationTest, HierarchicalKillsBlockAndRecoveryRequiresAuthority) {
  const auto report = aegis::integration::run_operator_simulation();

  EXPECT_TRUE(report.passed);
  EXPECT_TRUE(report.paper_mode_only);
  EXPECT_FALSE(report.production_activation_attempted);
  EXPECT_TRUE(report.baseline_approved);
  EXPECT_TRUE(report.every_kill_blocked);
  EXPECT_TRUE(report.unauthorized_clear_rejected);
  EXPECT_TRUE(report.authorized_recovery_succeeded);
  EXPECT_TRUE(report.decision_journal_complete);
  EXPECT_TRUE(report.audit_chain_valid);
  EXPECT_EQ(report.extracted_risk_decisions,
            aegis::integration::kOperatorSimulationRiskDecisionCount);
  EXPECT_EQ(report.risk_journal_records_remaining, 0U);
  EXPECT_EQ(report.audit_records.size(), 21U);

  constexpr std::array expected_scopes{
      aegis::risk::KillSwitchScope::symbol, aegis::risk::KillSwitchScope::strategy,
      aegis::risk::KillSwitchScope::venue, aegis::risk::KillSwitchScope::firm};
  for (std::size_t index = 0U; index < expected_scopes.size(); ++index) {
    EXPECT_EQ(report.scopes[index].scope, expected_scopes[index]);
    EXPECT_TRUE(report.scopes[index].passed);
    EXPECT_EQ(report.scopes[index].unauthorized_clear_status,
              aegis::risk::StateUpdateStatus::invalid);
  }
}

TEST(OperatorSimulationTest, FixedSeedProducesStableDecisionAndAuditHashes) {
  const auto first = aegis::integration::run_operator_simulation(20'260'908U);
  const auto second = aegis::integration::run_operator_simulation(20'260'908U);

  ASSERT_TRUE(first.passed);
  ASSERT_TRUE(second.passed);
  EXPECT_EQ(first.baseline_decision.stable_hash, second.baseline_decision.stable_hash);
  EXPECT_EQ(first.audit_extract_sha256, second.audit_extract_sha256);
  EXPECT_EQ(first.audit_chain_final_sha256, second.audit_chain_final_sha256);
  ASSERT_EQ(first.audit_records.size(), second.audit_records.size());
  for (std::size_t index = 0U; index < first.audit_records.size(); ++index) {
    EXPECT_EQ(first.audit_records[index].record_sha256,
              second.audit_records[index].record_sha256);
  }
}

} // namespace
