#include "aegis/execution/journal.hpp"

#include <gtest/gtest.h>

namespace aegis::execution::test {

TEST(GatewayJournalTest, PublishesAHashChainedModeMarkedAuditStream) {
  GatewayAuditJournal journal;
  auto first = journal.try_reserve();
  ASSERT_TRUE(first.valid);
  ASSERT_TRUE(journal.commit(first, {.kind = GatewayAuditKind::session_transition,
                                     .mode = GatewayMode::paper,
                                     .session_state = SessionState::active,
                                     .reason = GatewayReason::accepted,
                                     .correlation_id = {},
                                     .order_id = {},
                                     .process_monotonic_time_ns = 1U}));
  auto second = journal.try_reserve();
  ASSERT_TRUE(second.valid);
  ASSERT_TRUE(journal.commit(second, {.kind = GatewayAuditKind::command_decision,
                                      .mode = GatewayMode::paper,
                                      .session_state = SessionState::active,
                                      .reason = GatewayReason::accepted,
                                      .correlation_id = {},
                                      .order_id = {},
                                      .process_monotonic_time_ns = 2U}));

  GatewayAuditRecord record{};
  ASSERT_TRUE(journal.read(2U, record));
  EXPECT_EQ(record.mode, GatewayMode::paper);
  EXPECT_NE(record.previous_record_hash, 0U);
  EXPECT_TRUE(journal.verify_chain());
}

} // namespace aegis::execution::test
