#ifndef AEGIS_MODELS_BASELINES_HPP
#define AEGIS_MODELS_BASELINES_HPP

#include "aegis/models/model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace aegis::models {

[[nodiscard]] ModelPrediction
baseline_prediction(const features::FeatureSnapshot& snapshot,
                    std::int64_t expected_return_ppm) noexcept;

class ZeroReturnModel final : public IForecastModel {
public:
  explicit ZeroReturnModel(ModelMetadata metadata) noexcept : metadata_(metadata) {}
  [[nodiscard]] const ModelMetadata& metadata() const noexcept override {
    return metadata_;
  }
  [[nodiscard]] PredictionStatus predict(const ModelInput& input,
                                         ModelPrediction& output) noexcept override;

private:
  ModelMetadata metadata_;
};

class LastValueModel final : public IForecastModel {
public:
  explicit LastValueModel(ModelMetadata metadata) noexcept : metadata_(metadata) {}
  [[nodiscard]] const ModelMetadata& metadata() const noexcept override {
    return metadata_;
  }
  [[nodiscard]] PredictionStatus predict(const ModelInput& input,
                                         ModelPrediction& output) noexcept override;

private:
  ModelMetadata metadata_;
};

class MovingAverageModel final : public IForecastModel {
public:
  MovingAverageModel(ModelMetadata metadata, std::size_t window_size,
                     std::size_t minimum_samples) noexcept;
  [[nodiscard]] const ModelMetadata& metadata() const noexcept override {
    return metadata_;
  }
  [[nodiscard]] PredictionStatus predict(const ModelInput& input,
                                         ModelPrediction& output) noexcept override;
  void reset() noexcept;

private:
  ModelMetadata metadata_;
  std::array<std::int64_t, kMaximumMovingAverageSamples> values_{};
  std::size_t window_size_{};
  std::size_t minimum_samples_{};
  std::size_t size_{};
  std::size_t next_{};
  std::int64_t sum_{};
};

struct RegressionCoefficient {
  features::FeatureName feature{features::FeatureName::count};
  std::int64_t coefficient_ppm{};
};

class LinearRegressionModel final : public IForecastModel {
public:
  LinearRegressionModel(
      ModelMetadata metadata, std::int64_t intercept_ppm,
      std::array<RegressionCoefficient, kMaximumRegressionFeatures> coefficients,
      std::size_t coefficient_count) noexcept;
  [[nodiscard]] const ModelMetadata& metadata() const noexcept override {
    return metadata_;
  }
  [[nodiscard]] PredictionStatus predict(const ModelInput& input,
                                         ModelPrediction& output) noexcept override;

private:
  [[nodiscard]] PredictionStatus score(const ModelInput& input,
                                       std::int64_t& output) const noexcept;

  ModelMetadata metadata_;
  std::int64_t intercept_ppm_{};
  std::array<RegressionCoefficient, kMaximumRegressionFeatures> coefficients_{};
  std::size_t coefficient_count_{};
};

class LogisticRegressionModel final : public IForecastModel {
public:
  LogisticRegressionModel(
      ModelMetadata metadata, std::int64_t intercept_ppm,
      std::array<RegressionCoefficient, kMaximumRegressionFeatures> coefficients,
      std::size_t coefficient_count) noexcept;
  [[nodiscard]] const ModelMetadata& metadata() const noexcept override {
    return linear_.metadata();
  }
  [[nodiscard]] PredictionStatus predict(const ModelInput& input,
                                         ModelPrediction& output) noexcept override;

private:
  LinearRegressionModel linear_;
};

class SeasonalNaiveModel final : public IForecastModel {
public:
  SeasonalNaiveModel(ModelMetadata metadata, std::size_t bins) noexcept;
  [[nodiscard]] const ModelMetadata& metadata() const noexcept override {
    return metadata_;
  }
  [[nodiscard]] PredictionStatus predict(const ModelInput& input,
                                         ModelPrediction& output) noexcept override;
  void reset() noexcept;

private:
  ModelMetadata metadata_;
  std::array<std::int64_t, kMaximumSeasonalBins> values_{};
  std::array<bool, kMaximumSeasonalBins> populated_{};
  std::size_t bins_{};
};

} // namespace aegis::models

#endif // AEGIS_MODELS_BASELINES_HPP
