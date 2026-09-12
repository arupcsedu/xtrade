#include "aegis/integration/alpaca_paper_path.hpp"

#include <gtest/gtest.h>

namespace aegis::integration {
namespace {

TEST(AlpacaPaperPathTest, TraversesRouterRiskOmsAndFinalPaperGate) {
  const auto evidence = run_alpaca_paper_path("NVDA", 20'260'911U);
  ASSERT_TRUE(valid_alpaca_paper_path_evidence(evidence));
  EXPECT_EQ(evidence.stage, AlpacaPaperPathStage::complete);
  EXPECT_EQ(evidence.price_ticks, 1);
  EXPECT_EQ(evidence.price_increment_currency_nanos, 10'000'000U);
  EXPECT_EQ(evidence.quantity_units, 1U);
  EXPECT_NE(evidence.instrument_id_high, 0U);
  EXPECT_NE(evidence.instrument_id_low, 0U);
  EXPECT_TRUE(evidence.paper_mode);
  EXPECT_TRUE(evidence.risk_approved);
  EXPECT_TRUE(evidence.oms_command_valid);
  EXPECT_TRUE(evidence.gateway_final_gate_accepted);
  EXPECT_FALSE(evidence.live_trading_compiled);
}

TEST(AlpacaPaperPathTest, IsDeterministicAndRejectsMalformedSymbols) {
  const auto first = run_alpaca_paper_path("NVDA", 20'260'911U);
  const auto second = run_alpaca_paper_path("NVDA", 20'260'911U);
  EXPECT_EQ(first.stable_hash, second.stable_hash);
  EXPECT_EQ(first.client_order_id, second.client_order_id);

  const auto other_symbol = run_alpaca_paper_path("AAPL", 20'260'911U);
  ASSERT_TRUE(valid_alpaca_paper_path_evidence(other_symbol));
  EXPECT_NE(first.instrument_id_high, other_symbol.instrument_id_high);
  EXPECT_NE(first.instrument_id_low, other_symbol.instrument_id_low);
  EXPECT_NE(first.routing_request_hash, other_symbol.routing_request_hash);

  const auto malformed = run_alpaca_paper_path("../NVDA", 20'260'911U);
  EXPECT_EQ(malformed.stage, AlpacaPaperPathStage::invalid_symbol);
  EXPECT_FALSE(valid_alpaca_paper_path_evidence(malformed));
  EXPECT_FALSE(valid_alpaca_paper_symbol("A.B-C"));
}

TEST(AlpacaPaperPathTest, EvidenceMutationInvalidatesContract) {
  auto evidence = run_alpaca_paper_path("NVDA", 20'260'911U);
  ASSERT_TRUE(valid_alpaca_paper_path_evidence(evidence));
  evidence.quantity_units = 2U;
  EXPECT_FALSE(valid_alpaca_paper_path_evidence(evidence));
  evidence.stable_hash = stable_alpaca_paper_path_hash(evidence);
  EXPECT_FALSE(valid_alpaca_paper_path_evidence(evidence));
}

} // namespace
} // namespace aegis::integration
