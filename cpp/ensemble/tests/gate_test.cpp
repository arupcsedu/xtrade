#include "aegis/ensemble/gate.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace ensemble = aegis::ensemble;

namespace {

[[nodiscard]] bool all_excluded_as(const ensemble::DecisionExplanation& explanation,
                                   const ensemble::EligibilityReason reason) noexcept {
  for (std::size_t index = 0U; index < explanation.supplied_expert_count; ++index) {
    if (explanation.experts[index].weight_ppm != 0U ||
        explanation.experts[index].eligibility != reason) {
      return false;
    }
  }
  return true;
}

} // namespace

TEST(MixtureOfExpertsGateTest, CombinesEligibleForecastsAndExplainsDecision) {
  const ensemble::MixtureOfExpertsGate gate{ensemble::test::config()};
  ASSERT_TRUE(gate.initialized());

  const auto result = gate.evaluate(ensemble::test::request());
  EXPECT_EQ(result.status, ensemble::EvaluationStatus::produced);
  EXPECT_FALSE(result.forecast.abstain);
  EXPECT_EQ(result.forecast.primary_reason, ensemble::DecisionReason::combined);
  EXPECT_EQ(result.forecast.explanation.eligible_expert_count, 2U);
  EXPECT_EQ(result.forecast.explanation.contributing_expert_count, 2U);
  EXPECT_EQ(result.forecast.explanation.dominant_reason,
            ensemble::DominantExpertReason::canonical_tie_break);
  EXPECT_EQ(result.forecast.estimated_transaction_cost_ppm, 500U);
  EXPECT_EQ(result.forecast.expected_return_ppm, 50'000);
  EXPECT_EQ(result.forecast.probability_down_ppm +
                result.forecast.probability_flat_ppm +
                result.forecast.probability_up_ppm,
            ensemble::kWeightScale);
  EXPECT_TRUE(ensemble::valid_ensemble_forecast(result.forecast));
}

TEST(MixtureOfExpertsGateTest, DownweightsTimeseriesExpertDuringBreakingNews) {
  auto input =
      ensemble::test::request(2U, aegis::market_state::MarketState::breaking_news,
                              ensemble::EventState::breaking_news);
  const auto timeseries_id = input.experts[1U].forecast.model_id;
  const auto microstructure_id = input.experts[0U].forecast.model_id;
  const ensemble::MixtureOfExpertsGate gate{ensemble::test::config()};

  const auto result = gate.evaluate(input);
  const auto* timeseries =
      ensemble::test::find_model(result.forecast.explanation, timeseries_id);
  const auto* microstructure =
      ensemble::test::find_model(result.forecast.explanation, microstructure_id);
  ASSERT_NE(timeseries, nullptr);
  ASSERT_NE(microstructure, nullptr);
  EXPECT_LT(timeseries->weight_ppm, microstructure->weight_ppm);
  EXPECT_EQ(timeseries->eligibility, ensemble::EligibilityReason::eligible);
}

TEST(MixtureOfExpertsGateTest, HaltHardExcludesEveryExpert) {
  const ensemble::MixtureOfExpertsGate gate{ensemble::test::config()};
  const auto result = gate.evaluate(
      ensemble::test::request(2U, aegis::market_state::MarketState::halted));

  EXPECT_EQ(result.status, ensemble::EvaluationStatus::abstained);
  EXPECT_TRUE(result.forecast.abstain);
  EXPECT_EQ(result.forecast.primary_reason,
            ensemble::DecisionReason::unsafe_market_state);
  EXPECT_EQ(result.forecast.explanation.eligible_expert_count, 0U);
  EXPECT_TRUE(all_excluded_as(result.forecast.explanation,
                              ensemble::EligibilityReason::incompatible_state));
  EXPECT_TRUE(ensemble::valid_ensemble_forecast(result.forecast));
}

TEST(MixtureOfExpertsGateTest, StaleExpertReceivesZeroWeight) {
  auto input = ensemble::test::request();
  const auto stale_model_id = input.experts[1U].forecast.model_id;
  input.experts[1U].forecast.production_process_monotonic_time_ns =
      ensemble::test::kNow - 10'001U;
  input.experts[1U].forecast.stable_hash =
      aegis::models::stable_forecast_hash(input.experts[1U].forecast);
  auto configuration = ensemble::test::config();
  configuration.maximum_expert_weight_ppm = ensemble::kWeightScale;
  configuration.stable_hash = ensemble::stable_ensemble_config_hash(configuration);
  const ensemble::MixtureOfExpertsGate gate{configuration};

  const auto result = gate.evaluate(input);
  const auto* stale =
      ensemble::test::find_model(result.forecast.explanation, stale_model_id);
  ASSERT_NE(stale, nullptr);
  EXPECT_EQ(stale->eligibility, ensemble::EligibilityReason::expired);
  EXPECT_EQ(stale->weight_ppm, 0U);
  EXPECT_EQ(result.forecast.explanation.contributing_expert_count, 1U);
}

TEST(MixtureOfExpertsGateTest, HighDisagreementRaisesEffectiveUncertainty) {
  const ensemble::MixtureOfExpertsGate gate{ensemble::test::config()};
  auto agreeing = ensemble::test::request();
  const auto agreeing_result = gate.evaluate(agreeing);

  auto disagreeing = ensemble::test::request();
  disagreeing.experts[1U].forecast = ensemble::test::expert_forecast(2U, -50'000);
  const auto disagreeing_result = gate.evaluate(disagreeing);

  EXPECT_GT(disagreeing_result.forecast.disagreement_ppm,
            agreeing_result.forecast.disagreement_ppm);
  EXPECT_GT(disagreeing_result.forecast.effective_uncertainty_ppm,
            agreeing_result.forecast.effective_uncertainty_ppm);
}

TEST(MixtureOfExpertsGateTest, InvalidExpertsProduceAuditableAbstention) {
  auto input = ensemble::test::request();
  input.experts[0U].health.state = aegis::models::ModelHealthState::disabled;
  input.experts[1U].forecast.feature_snapshot_id = {};
  input.experts[1U].forecast.stable_hash =
      aegis::models::stable_forecast_hash(input.experts[1U].forecast);
  const ensemble::MixtureOfExpertsGate gate{ensemble::test::config()};

  const auto result = gate.evaluate(input);
  EXPECT_EQ(result.status, ensemble::EvaluationStatus::abstained);
  EXPECT_EQ(result.forecast.primary_reason,
            ensemble::DecisionReason::no_eligible_expert);
  EXPECT_TRUE(result.forecast.abstain);
  EXPECT_EQ(result.forecast.probability_flat_ppm, ensemble::kWeightScale);
  EXPECT_TRUE(ensemble::valid_ensemble_forecast(result.forecast));
}

TEST(MixtureOfExpertsGateTest, ReportsEachExpertHardExclusion) {
  const ensemble::MixtureOfExpertsGate gate{ensemble::test::config()};
  const auto verify_first = [&gate](const ensemble::EnsembleRequest& input,
                                    const ensemble::EligibilityReason expected) {
    const auto model_id = input.experts[0U].forecast.model_id;
    const auto result = gate.evaluate(input);
    const auto* explanation =
        ensemble::test::find_model(result.forecast.explanation, model_id);
    ASSERT_NE(explanation, nullptr);
    EXPECT_EQ(explanation->eligibility, expected);
    EXPECT_EQ(explanation->weight_ppm, 0U);
  };

  auto input = ensemble::test::request();
  input.experts[0U].forecast.feature_snapshot_id = {};
  input.experts[0U].forecast.stable_hash =
      aegis::models::stable_forecast_hash(input.experts[0U].forecast);
  verify_first(input, ensemble::EligibilityReason::missing_provenance);

  input = ensemble::test::request();
  input.experts[0U].health.state = aegis::models::ModelHealthState::disabled;
  verify_first(input, ensemble::EligibilityReason::model_disabled);

  input = ensemble::test::request();
  ++input.experts[0U].health.generation;
  verify_first(input, ensemble::EligibilityReason::control_generation_mismatch);

  input = ensemble::test::request();
  ++input.experts[0U].forecast.horizon_ns;
  input.experts[0U].forecast.stable_hash =
      aegis::models::stable_forecast_hash(input.experts[0U].forecast);
  verify_first(input, ensemble::EligibilityReason::scope_mismatch);

  input = ensemble::test::request();
  input.experts[0U].calibration_health = ensemble::CalibrationHealth::failed;
  verify_first(input, ensemble::EligibilityReason::calibration_failed);

  input = ensemble::test::request();
  input.experts[0U].forecast.prediction.data_quality_score_ppm = 499'999U;
  input.experts[0U].forecast.stable_hash =
      aegis::models::stable_forecast_hash(input.experts[0U].forecast);
  verify_first(input, ensemble::EligibilityReason::data_quality_failed);

  input = ensemble::test::request();
  ++input.experts[0U].forecast.prediction.probability_up_ppm;
  input.experts[0U].forecast.stable_hash =
      aegis::models::stable_forecast_hash(input.experts[0U].forecast);
  verify_first(input, ensemble::EligibilityReason::invalid_forecast);
}

TEST(MixtureOfExpertsGateTest, InvalidFeedHardExcludesEveryExpert) {
  auto input = ensemble::test::request();
  input.feed_health = aegis::market_state::FeedHealth::stale;
  const ensemble::MixtureOfExpertsGate gate{ensemble::test::config()};

  const auto result = gate.evaluate(input);
  EXPECT_EQ(result.status, ensemble::EvaluationStatus::abstained);
  EXPECT_EQ(result.forecast.primary_reason,
            ensemble::DecisionReason::invalid_input_feed);
  EXPECT_TRUE(all_excluded_as(result.forecast.explanation,
                              ensemble::EligibilityReason::invalid_input_feed));
}

TEST(MixtureOfExpertsGateTest, EnforcesPerExpertCapDuringNormalization) {
  auto configuration = ensemble::test::config();
  configuration.maximum_expert_weight_ppm = 400'000U;
  configuration.stable_hash = ensemble::stable_ensemble_config_hash(configuration);
  auto input = ensemble::test::request(3U);
  input.experts[2U].role = ensemble::ExpertRole::options;
  const ensemble::MixtureOfExpertsGate gate{configuration};

  const auto result = gate.evaluate(input);
  std::uint64_t weight_sum{};
  for (std::size_t index = 0U;
       index < result.forecast.explanation.supplied_expert_count; ++index) {
    const auto weight = result.forecast.explanation.experts[index].weight_ppm;
    EXPECT_LE(weight, configuration.maximum_expert_weight_ppm);
    weight_sum += weight;
  }
  EXPECT_EQ(weight_sum, ensemble::kWeightScale);
}

TEST(MixtureOfExpertsGateTest, AbstainsWhenTheConfiguredCapIsInfeasible) {
  const ensemble::MixtureOfExpertsGate gate{ensemble::test::config()};
  const auto result = gate.evaluate(ensemble::test::request(1U));

  EXPECT_EQ(result.status, ensemble::EvaluationStatus::abstained);
  EXPECT_EQ(result.forecast.primary_reason,
            ensemble::DecisionReason::weight_cap_infeasible);
  EXPECT_EQ(result.forecast.explanation.experts[0U].weight_ppm, 0U);
}

TEST(MixtureOfExpertsGateTest, DuplicateModelIdentityCannotEvadeTheWeightCap) {
  auto input = ensemble::test::request();
  input.experts[1U].forecast.model_id = input.experts[0U].forecast.model_id;
  input.experts[1U].forecast.stable_hash =
      aegis::models::stable_forecast_hash(input.experts[1U].forecast);
  const ensemble::MixtureOfExpertsGate gate{ensemble::test::config()};

  const auto result = gate.evaluate(input);
  EXPECT_EQ(result.forecast.primary_reason,
            ensemble::DecisionReason::no_eligible_expert);
  for (std::size_t index = 0U;
       index < result.forecast.explanation.supplied_expert_count; ++index) {
    EXPECT_EQ(result.forecast.explanation.experts[index].eligibility,
              ensemble::EligibilityReason::duplicate_expert);
    EXPECT_EQ(result.forecast.explanation.experts[index].weight_ppm, 0U);
  }
}

TEST(MixtureOfExpertsGateTest, LearnedGateCannotBypassHardStateMask) {
  auto configuration = ensemble::test::config(ensemble::GateKind::quantized_learned);
  const auto timeseries_index =
      static_cast<std::size_t>(ensemble::ExpertRole::timeseries) - 1U;
  const auto breaking_index =
      static_cast<std::size_t>(ensemble::EventState::breaking_news) - 1U;
  configuration.role_policies[timeseries_index].allowed_event_state_mask &=
      static_cast<std::uint8_t>(~(std::uint8_t{1U} << breaking_index));
  configuration.learned.role_bias_ppm[timeseries_index] =
      ensemble::kMaximumGateCoefficientPpm;
  configuration.learned.stable_hash =
      ensemble::stable_learned_gate_hash(configuration.learned);
  configuration.stable_hash = ensemble::stable_ensemble_config_hash(configuration);
  auto input =
      ensemble::test::request(2U, aegis::market_state::MarketState::breaking_news,
                              ensemble::EventState::breaking_news);
  const auto timeseries_id = input.experts[1U].forecast.model_id;
  const ensemble::MixtureOfExpertsGate gate{configuration};

  const auto result = gate.evaluate(input);
  const auto* timeseries =
      ensemble::test::find_model(result.forecast.explanation, timeseries_id);
  ASSERT_NE(timeseries, nullptr);
  EXPECT_EQ(timeseries->eligibility, ensemble::EligibilityReason::incompatible_state);
  EXPECT_EQ(timeseries->weight_ppm, 0U);
}

TEST(MixtureOfExpertsGateTest, StrictRobustEdgeRuleAbstainsAtThreshold) {
  auto configuration = ensemble::test::config();
  configuration.uncertainty_penalty_multiplier_ppm = 0U;
  configuration.safety_margin_ppm = 500U;
  configuration.stable_hash = ensemble::stable_ensemble_config_hash(configuration);
  auto input = ensemble::test::request();
  input.experts[0U].forecast = ensemble::test::expert_forecast(1U, 1'000);
  input.experts[1U].forecast = ensemble::test::expert_forecast(2U, 1'000);
  const ensemble::MixtureOfExpertsGate gate{configuration};

  const auto result = gate.evaluate(input);
  EXPECT_EQ(result.forecast.explanation.required_edge_ppm, 1'000U);
  EXPECT_TRUE(result.forecast.abstain);
  EXPECT_EQ(result.forecast.primary_reason,
            ensemble::DecisionReason::insufficient_robust_edge);
}
