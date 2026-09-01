#include "aegis/models/baselines.hpp"
#include "aegis/models/cache.hpp"
#include "aegis/models/runner.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <atomic>

namespace models = aegis::models;

namespace {

class FailingModel final : public models::IForecastModel {
public:
  explicit FailingModel(models::ModelMetadata metadata) : metadata_(metadata) {}
  [[nodiscard]] const models::ModelMetadata& metadata() const noexcept override {
    return metadata_;
  }
  [[nodiscard]] models::PredictionStatus
  predict(const models::ModelInput& /*input*/,
          models::ModelPrediction& /*output*/) noexcept override {
    return models::PredictionStatus::unavailable;
  }

private:
  models::ModelMetadata metadata_;
};

class DeadlineAdvancingModel final : public models::IForecastModel {
public:
  DeadlineAdvancingModel(models::ModelMetadata metadata,
                         std::atomic<std::uint64_t>& clock)
      : metadata_(metadata), clock_(clock) {}
  [[nodiscard]] const models::ModelMetadata& metadata() const noexcept override {
    return metadata_;
  }
  [[nodiscard]] models::PredictionStatus
  predict(const models::ModelInput& input,
          models::ModelPrediction& output) noexcept override {
    output = models::baseline_prediction(input.feature_snapshot, 0);
    clock_.store(input.deadline.complete_by_process_monotonic_time_ns + 1U,
                 std::memory_order_relaxed);
    return models::PredictionStatus::ok;
  }

private:
  models::ModelMetadata metadata_;
  std::atomic<std::uint64_t>& clock_;
};

class OodModel final : public models::IForecastModel {
public:
  explicit OodModel(models::ModelMetadata metadata) : metadata_(metadata) {}
  [[nodiscard]] const models::ModelMetadata& metadata() const noexcept override {
    return metadata_;
  }
  [[nodiscard]] models::PredictionStatus
  predict(const models::ModelInput& input,
          models::ModelPrediction& output) noexcept override {
    output = models::baseline_prediction(input.feature_snapshot, 0);
    output.ood_score_ppm = 2U;
    return models::PredictionStatus::ok;
  }

private:
  models::ModelMetadata metadata_;
};

} // namespace

TEST(LocalModelRunnerTest, UsesOnlyExplicitSeparatelyIdentifiedFallback) {
  std::atomic<std::uint64_t> clock{110U};
  auto primary_metadata = models::test::metadata(models::ModelKind::external);
  FailingModel primary{primary_metadata};
  const aegis::common::ModelId fallback_id{30U, 30U};
  models::ZeroReturnModel fallback{
      models::test::metadata(models::ModelKind::zero_return, fallback_id)};

  models::LocalModelRunner fail_closed{primary, {models::test::atomic_clock, &clock}};
  EXPECT_EQ(fail_closed.run(models::test::input()).status,
            models::RunStatus::model_failure);

  models::LocalModelRunner with_fallback{
      primary,
      {models::test::atomic_clock, &clock},
      models::FallbackPolicy::explicit_model_on_failure,
      &fallback};
  const auto result = with_fallback.run(models::test::input());
  EXPECT_EQ(result.status, models::RunStatus::fallback_accepted);
  EXPECT_TRUE(result.used_fallback);
  EXPECT_EQ(result.forecast.model_id, fallback_id);
}

TEST(LocalModelRunnerTest, DisableIsImmediateAndControlUpdatesAreVersioned) {
  std::atomic<std::uint64_t> clock{110U};
  models::ZeroReturnModel model{models::test::metadata()};
  models::LocalModelRunner runner{model, {models::test::atomic_clock, &clock}};
  EXPECT_TRUE(runner.apply_control(
      {.generation = 2U, .state = models::ModelHealthState::disabled}));
  EXPECT_EQ(runner.run(models::test::input()).status, models::RunStatus::disabled);
  EXPECT_FALSE(runner.apply_control(
      {.generation = 2U, .state = models::ModelHealthState::healthy}));
  EXPECT_TRUE(runner.apply_control(
      {.generation = 3U, .state = models::ModelHealthState::healthy}));
  EXPECT_EQ(runner.run(models::test::input()).status, models::RunStatus::accepted);
  EXPECT_EQ(runner.health().generation, 3U);
}

TEST(LocalModelRunnerTest, DiscardsCompletionAfterDeadline) {
  std::atomic<std::uint64_t> clock{110U};
  DeadlineAdvancingModel model{models::test::metadata(), clock};
  models::LocalModelRunner runner{model, {models::test::atomic_clock, &clock}};
  EXPECT_EQ(runner.run(models::test::input()).status,
            models::RunStatus::deadline_missed);
}

TEST(LocalModelRunnerTest, RejectsOutputBeyondVersionedOodThreshold) {
  std::atomic<std::uint64_t> clock{110U};
  auto metadata = models::test::metadata(models::ModelKind::external);
  metadata.ood = {.method = models::OodMethod::bounded_feature_range,
                  .detector_version = aegis::common::ConfigurationVersion{40U, 40U},
                  .reject_threshold_ppm = 1U};
  OodModel model{metadata};
  models::LocalModelRunner runner{model, {models::test::atomic_clock, &clock}};
  const auto result = runner.run(models::test::input());
  EXPECT_EQ(result.status, models::RunStatus::invalid_forecast);
  EXPECT_EQ(result.validation_error, models::ForecastValidationError::invalid_score);
}

TEST(ForecastCacheTest, ValidatesExpiresAndImmediatelyInvalidatesModel) {
  models::ForecastCache<2U> cache;
  auto forecast = models::test::valid_forecast();
  EXPECT_EQ(cache.put(forecast, 120U), models::CacheStatus::ok);
  models::ModelForecast output{};
  EXPECT_EQ(cache.get(models::test::kModelId, models::test::kInstrument, 120U, output),
            models::CacheStatus::ok);
  EXPECT_EQ(output.forecast_id, forecast.forecast_id);
  EXPECT_EQ(cache.invalidate_model(models::test::kModelId), 1U);
  EXPECT_EQ(cache.size(), 0U);

  EXPECT_EQ(cache.put(forecast, 120U), models::CacheStatus::ok);
  models::ForecastExpiryManager<2U> expiry;
  EXPECT_EQ(expiry.expire(cache, 501U), 1U);
  EXPECT_EQ(expiry.expired_count(), 1U);
  EXPECT_EQ(cache.size(), 0U);
}
