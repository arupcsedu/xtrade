#include "aegis/models/validator.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace models = aegis::models;

TEST(ModelForecastValidatorTest, AcceptsCompleteFixedPointForecast) {
  const auto forecast = models::test::valid_forecast();
  EXPECT_EQ(models::ModelForecastValidator::validate_forecast(forecast, 120U),
            models::ForecastValidationError::none);
}

TEST(ModelForecastValidatorTest, RejectsInvalidProbabilityAndExpiredOutput) {
  auto forecast = models::test::valid_forecast();
  forecast.prediction.probability_up_ppm = 599'999U;
  forecast.stable_hash = models::stable_forecast_hash(forecast);
  EXPECT_EQ(models::ModelForecastValidator::validate_forecast(forecast, 120U),
            models::ForecastValidationError::invalid_probabilities);

  forecast = models::test::valid_forecast();
  EXPECT_EQ(models::ModelForecastValidator::validate_forecast(forecast, 501U),
            models::ForecastValidationError::expired);
}

TEST(ModelForecastValidatorTest, RejectsMissingFeatureProvenanceAndStaleFeatures) {
  auto input = models::test::input();
  input.feature_snapshot.snapshot_id = {};
  EXPECT_EQ(models::ModelForecastValidator::validate_input(models::test::metadata(),
                                                           input, 110U),
            models::ForecastValidationError::invalid_feature_provenance);

  input = models::test::input();
  input.feature_snapshot
      .values[static_cast<std::size_t>(
          aegis::features::FeatureName::rolling_return_ppm)]
      .as_of_process_monotonic_time_ns = 1U;
  input.feature_snapshot.stable_hash =
      aegis::features::stable_snapshot_hash(input.feature_snapshot);
  EXPECT_EQ(models::ModelForecastValidator::validate_input(models::test::metadata(),
                                                           input, 1'002U),
            models::ForecastValidationError::late);
  input.deadline.complete_by_process_monotonic_time_ns = 2'000U;
  EXPECT_EQ(models::ModelForecastValidator::validate_input(models::test::metadata(),
                                                           input, 1'002U),
            models::ForecastValidationError::feature_stale);
}

TEST(ModelForecastValidatorTest, RejectsNanAndInfinityAtExternalBoundary) {
  models::ExternalForecastValues values{.expected_return = 0.001,
                                        .return_p10 = -0.002,
                                        .return_p50 = 0.001,
                                        .return_p90 = 0.004,
                                        .probability_down = 0.25,
                                        .probability_flat = 0.15,
                                        .probability_up = 0.60,
                                        .volatility = 0.003,
                                        .confidence = 0.8,
                                        .calibration_score = 0.9,
                                        .data_quality_score = 1.0,
                                        .ood_score = 0.01};
  models::ModelPrediction prediction{};
  EXPECT_EQ(models::ModelForecastValidator::convert_external(values, prediction),
            models::ForecastValidationError::none);
  EXPECT_EQ(prediction.expected_return_ppm, 1'000);

  values.confidence = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(models::ModelForecastValidator::convert_external(values, prediction),
            models::ForecastValidationError::non_finite_value);
  values.confidence = std::numeric_limits<double>::infinity();
  EXPECT_EQ(models::ModelForecastValidator::convert_external(values, prediction),
            models::ForecastValidationError::non_finite_value);
}
