#include "aegis/risk/journal.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace risk = aegis::risk;

namespace {

[[nodiscard]] bool fill_journal(risk::RiskDecisionJournal& journal) {
  for (std::size_t index = 0U; index < risk::kRiskDecisionJournalCapacity; ++index) {
    const auto reservation = journal.try_reserve();
    if (!reservation.valid) {
      return false;
    }
    risk::RiskDecision decision{};
    decision.evaluation_ordinal = index + 1U;
    journal.commit(reservation, decision);
  }
  return true;
}

[[nodiscard]] bool drain_in_fifo_order(risk::RiskDecisionJournal& journal) {
  for (std::size_t index = 0U; index < risk::kRiskDecisionJournalCapacity; ++index) {
    risk::RiskDecision decision{};
    if (!journal.try_pop(decision) || decision.evaluation_ordinal != index + 1U) {
      return false;
    }
  }
  return true;
}

} // namespace

TEST(RiskDecisionJournalTest, IsBoundedAndPreservesFifoOrder) {
  risk::RiskDecisionJournal journal;
  ASSERT_TRUE(fill_journal(journal));
  EXPECT_FALSE(journal.try_reserve().valid);
  EXPECT_EQ(journal.size(), risk::kRiskDecisionJournalCapacity);
  EXPECT_TRUE(drain_in_fifo_order(journal));
  EXPECT_EQ(journal.size(), 0U);
}
