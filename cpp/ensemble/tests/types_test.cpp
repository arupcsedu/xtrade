#include "aegis/ensemble/types.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <limits>

namespace ensemble = aegis::ensemble;

TEST(EnsembleTypesTest, RejectsTamperedConfigurationAndForecast) {
  auto configuration = ensemble::test::config();
  EXPECT_TRUE(ensemble::valid_ensemble_config(configuration));
  configuration.maximum_expert_weight_ppm = 0U;
  EXPECT_FALSE(ensemble::valid_ensemble_config(configuration));

  const ensemble::MixtureOfExpertsGate gate{ensemble::test::config()};
  auto forecast = gate.evaluate(ensemble::test::request()).forecast;
  ASSERT_TRUE(ensemble::valid_ensemble_forecast(forecast));
  ++forecast.expected_return_ppm;
  EXPECT_FALSE(ensemble::valid_ensemble_forecast(forecast));

  forecast = gate.evaluate(ensemble::test::request()).forecast;
  ++forecast.net_robust_edge_ppm;
  forecast.stable_hash = ensemble::stable_ensemble_forecast_hash(forecast);
  EXPECT_FALSE(ensemble::valid_ensemble_forecast(forecast));
}

TEST(EnsembleTypesTest, RejectsUnknownEnumsAndUnsignedLearnedArtifact) {
  auto configuration = ensemble::test::config();
  configuration.gate_kind = std::bit_cast<ensemble::GateKind>(std::uint8_t{255U});
  configuration.stable_hash = ensemble::stable_ensemble_config_hash(configuration);
  EXPECT_FALSE(ensemble::valid_ensemble_config(configuration));

  configuration = ensemble::test::config(ensemble::GateKind::quantized_learned);
  configuration.learned.signature_hash = 0U;
  configuration.learned.stable_hash =
      ensemble::stable_learned_gate_hash(configuration.learned);
  configuration.stable_hash = ensemble::stable_ensemble_config_hash(configuration);
  EXPECT_FALSE(ensemble::valid_ensemble_config(configuration));
}

TEST(EnsembleTypesTest, RejectsDurationsThatWouldOverflowFreshnessScaling) {
  auto configuration = ensemble::test::config();
  configuration.maximum_forecast_age_ns = ensemble::kMaximumScaledDurationNs + 1U;
  configuration.stable_hash = ensemble::stable_ensemble_config_hash(configuration);
  EXPECT_FALSE(ensemble::valid_ensemble_config(configuration));

  configuration = ensemble::test::config();
  configuration.maximum_market_state_age_ns = std::numeric_limits<std::uint64_t>::max();
  configuration.stable_hash = ensemble::stable_ensemble_config_hash(configuration);
  EXPECT_FALSE(ensemble::valid_ensemble_config(configuration));
}

TEST(EnsembleTypesTest, FailsClosedWhenTransactionCostIsMissing) {
  auto input = ensemble::test::request();
  input.transaction_cost_present = false;
  const ensemble::MixtureOfExpertsGate gate{ensemble::test::config()};

  const auto result = gate.evaluate(input);
  EXPECT_EQ(result.status, ensemble::EvaluationStatus::abstained);
  EXPECT_TRUE(result.forecast.abstain);
  EXPECT_EQ(result.forecast.primary_reason,
            ensemble::DecisionReason::invalid_transaction_cost);
}

TEST(EnsembleTypesTest, RejectsAnAlreadyExpiredDecisionWindow) {
  auto input = ensemble::test::request();
  input.valid_until_process_monotonic_time_ns = input.now_process_monotonic_time_ns;
  const ensemble::MixtureOfExpertsGate gate{ensemble::test::config()};

  const auto result = gate.evaluate(input);
  EXPECT_EQ(result.status, ensemble::EvaluationStatus::invalid_request);
  EXPECT_TRUE(result.forecast.abstain);
  EXPECT_EQ(result.forecast.primary_reason, ensemble::DecisionReason::invalid_request);
}
