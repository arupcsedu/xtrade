#include "aegis/ensemble/gate.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>

namespace ensemble = aegis::ensemble;

TEST(MixtureOfExpertsGatePropertyTest, EveryGateKindProducesAValidNormalDecision) {
  constexpr std::array gate_kinds{ensemble::GateKind::rule_based,
                                  ensemble::GateKind::linear,
                                  ensemble::GateKind::quantized_learned};
  for (const auto kind : gate_kinds) {
    const ensemble::MixtureOfExpertsGate gate{ensemble::test::config(kind)};
    const auto result = gate.evaluate(ensemble::test::request());
    EXPECT_EQ(result.status, ensemble::EvaluationStatus::produced);
    EXPECT_TRUE(ensemble::valid_ensemble_forecast(result.forecast));
  }
}

TEST(MixtureOfExpertsGatePropertyTest, PermutationsProduceIdenticalDecisionHash) {
  constexpr std::uint64_t kSeed = 20'260'828U;
  std::mt19937_64 random{kSeed};
  const ensemble::MixtureOfExpertsGate gate{ensemble::test::config()};
  auto baseline_request = ensemble::test::request(6U);
  for (std::size_t index = 0U; index < baseline_request.expert_count; ++index) {
    baseline_request.experts[index].role =
        static_cast<ensemble::ExpertRole>((index % ensemble::kExpertRoleCount) + 1U);
    baseline_request.experts[index].forecast = ensemble::test::expert_forecast(
        index + 1U, static_cast<std::int64_t>((index + 1U) * 10'000U));
  }
  const auto baseline = gate.evaluate(baseline_request).forecast;
  ASSERT_TRUE(ensemble::valid_ensemble_forecast(baseline));

  for (std::size_t iteration = 0U; iteration < 256U; ++iteration) {
    auto permuted = baseline_request;
    std::shuffle(permuted.experts.begin(),
                 permuted.experts.begin() + permuted.expert_count, random);
    const auto result = gate.evaluate(permuted).forecast;
    EXPECT_EQ(result.stable_hash, baseline.stable_hash) << "seed=" << kSeed;
  }
}

TEST(MixtureOfExpertsGatePropertyTest, HardMaskDominatesEveryGateKind) {
  constexpr std::array gate_kinds{ensemble::GateKind::rule_based,
                                  ensemble::GateKind::linear,
                                  ensemble::GateKind::quantized_learned};
  for (const auto kind : gate_kinds) {
    auto input = ensemble::test::request();
    input.experts[0U].forecast.prediction.ood_score_ppm = 900'000U;
    input.experts[0U].forecast.stable_hash =
        aegis::models::stable_forecast_hash(input.experts[0U].forecast);
    const auto excluded_id = input.experts[0U].forecast.model_id;
    const ensemble::MixtureOfExpertsGate gate{ensemble::test::config(kind)};

    const auto result = gate.evaluate(input);
    const auto* excluded =
        ensemble::test::find_model(result.forecast.explanation, excluded_id);
    ASSERT_NE(excluded, nullptr);
    EXPECT_EQ(excluded->eligibility, ensemble::EligibilityReason::excessive_ood);
    EXPECT_EQ(excluded->weight_ppm, 0U);
  }
}

TEST(MixtureOfExpertsGatePropertyTest, RandomValidForecastsPreserveInvariants) {
  constexpr std::uint64_t kSeed = 20'260'828U;
  std::mt19937_64 random{kSeed};
  std::uniform_int_distribution<std::int64_t> return_distribution{-100'000, 100'000};
  std::uniform_int_distribution<std::uint32_t> score_distribution{500'000U, 1'000'000U};
  const ensemble::MixtureOfExpertsGate gate{ensemble::test::config()};

  for (std::size_t iteration = 0U; iteration < 1'000U; ++iteration) {
    auto input = ensemble::test::request(4U);
    for (std::size_t index = 0U; index < input.expert_count; ++index) {
      const auto value = return_distribution(random);
      auto forecast = ensemble::test::expert_forecast(index + 1U, value);
      forecast.prediction.confidence_ppm = score_distribution(random);
      forecast.prediction.calibration_score_ppm = score_distribution(random);
      forecast.prediction.data_quality_score_ppm = score_distribution(random);
      forecast.prediction.ood_score_ppm =
          ensemble::kWeightScale - score_distribution(random);
      forecast.stable_hash = aegis::models::stable_forecast_hash(forecast);
      input.experts[index].forecast = forecast;
      input.experts[index].role =
          static_cast<ensemble::ExpertRole>((index % ensemble::kExpertRoleCount) + 1U);
    }
    const auto result = gate.evaluate(input);
    EXPECT_TRUE(ensemble::valid_ensemble_forecast(result.forecast))
        << "seed=" << kSeed << " iteration=" << iteration;
    std::uint64_t weight_sum{};
    for (std::size_t index = 0U;
         index < result.forecast.explanation.supplied_expert_count; ++index) {
      weight_sum += result.forecast.explanation.experts[index].weight_ppm;
    }
    EXPECT_EQ(weight_sum, ensemble::kWeightScale);
  }
}
