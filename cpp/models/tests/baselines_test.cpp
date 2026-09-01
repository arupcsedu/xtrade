#include "aegis/models/baselines.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <array>

namespace models = aegis::models;

TEST(BaselineModelsTest, ZeroLastValueAndMovingAverageAreDeterministic) {
  models::ZeroReturnModel zero{models::test::metadata()};
  models::LastValueModel last{models::test::metadata(models::ModelKind::last_value)};
  models::MovingAverageModel average{
      models::test::metadata(models::ModelKind::moving_average), 3U, 2U};
  models::ModelPrediction output{};
  auto input = models::test::input(2'000);
  EXPECT_EQ(zero.predict(input, output), models::PredictionStatus::ok);
  EXPECT_EQ(output.expected_return_ppm, 0);
  EXPECT_EQ(last.predict(input, output), models::PredictionStatus::ok);
  EXPECT_EQ(output.expected_return_ppm, 2'000);
  EXPECT_EQ(average.predict(input, output), models::PredictionStatus::warming);
  input.feature_snapshot = models::test::snapshot(4'000);
  EXPECT_EQ(average.predict(input, output), models::PredictionStatus::ok);
  EXPECT_EQ(output.expected_return_ppm, 3'000);
}

TEST(BaselineModelsTest, LinearAndLogisticRegressionUseBoundedIntegerMath) {
  std::array<models::RegressionCoefficient, models::kMaximumRegressionFeatures>
      coefficients{};
  coefficients[0U] = {.feature = aegis::features::FeatureName::rolling_return_ppm,
                      .coefficient_ppm = 500'000};
  models::LinearRegressionModel linear{
      models::test::metadata(models::ModelKind::linear_regression), 100, coefficients,
      1U};
  models::LogisticRegressionModel logistic{
      models::test::metadata(models::ModelKind::logistic_regression), 0, coefficients,
      1U};
  models::ModelPrediction output{};
  const auto input = models::test::input(2'000);
  EXPECT_EQ(linear.predict(input, output), models::PredictionStatus::ok);
  EXPECT_EQ(output.expected_return_ppm, 1'100);
  EXPECT_EQ(logistic.predict(models::test::input(0), output),
            models::PredictionStatus::ok);
  EXPECT_EQ(output.probability_up_ppm, 500'000U);
  EXPECT_EQ(output.probability_down_ppm, 500'000U);
}

TEST(BaselineModelsTest, SeasonalNaiveWarmsThenUsesPriorBinObservation) {
  models::SeasonalNaiveModel seasonal{
      models::test::metadata(models::ModelKind::seasonal_naive), 4U};
  models::ModelPrediction output{};
  EXPECT_EQ(seasonal.predict(models::test::input(1'500), output),
            models::PredictionStatus::warming);
  EXPECT_EQ(seasonal.predict(models::test::input(3'000), output),
            models::PredictionStatus::ok);
  EXPECT_EQ(output.expected_return_ppm, 1'500);
  seasonal.reset();
  EXPECT_EQ(seasonal.predict(models::test::input(3'000), output),
            models::PredictionStatus::warming);
}
