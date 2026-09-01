#include "aegis/models/baselines.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace aegis::models {
namespace {

[[nodiscard]] std::int64_t bounded_return(const std::int64_t value) noexcept {
  return std::clamp(value, -kMaximumAbsoluteReturnPpm, kMaximumAbsoluteReturnPpm);
}

[[nodiscard]] std::uint64_t
volatility(const features::FeatureSnapshot& snapshot) noexcept {
  const auto value = snapshot.get(features::FeatureName::realized_volatility_ppm).value;
  if (value <= 0) {
    return 0U;
  }
  return std::min(static_cast<std::uint64_t>(value), kMaximumVolatilityPpm);
}

[[nodiscard]] std::uint32_t sigmoid_ppm(const std::int64_t score_ppm) noexcept {
  // Fixed lookup/interpolation avoids platform-dependent exp() in replay. The
  // table samples sigmoid(x) at integer x from -8 through +8.
  constexpr std::array<std::uint32_t, 17> kSigmoid{
      335U,     911U,     2'473U,   6'693U,   17'986U,  47'426U,
      119'203U, 268'941U, 500'000U, 731'059U, 880'797U, 952'574U,
      982'014U, 993'307U, 997'527U, 999'089U, 999'665U};
  constexpr std::int64_t kStep = 1'000'000LL;
  const auto clipped = std::clamp(score_ppm, -8 * kStep, 8 * kStep);
  const auto shifted = clipped + (8 * kStep);
  const auto lower = static_cast<std::size_t>(shifted / kStep);
  if (lower >= kSigmoid.size() - 1U) {
    return kSigmoid.back();
  }
  const auto remainder = static_cast<std::uint64_t>(shifted % kStep);
  const auto base = kSigmoid[lower];
  const auto delta = kSigmoid[lower + 1U] - base;
  return base +
         static_cast<std::uint32_t>((static_cast<std::uint64_t>(delta) * remainder) /
                                    static_cast<std::uint64_t>(kStep));
}

[[nodiscard]] bool safe_accumulate(const std::int64_t value,
                                   const std::int64_t coefficient_ppm,
                                   std::int64_t& score) noexcept {
  constexpr auto kScale = static_cast<std::int64_t>(kProbabilityScale);
  if (coefficient_ppm < -kMaximumAbsoluteReturnPpm ||
      coefficient_ppm > kMaximumAbsoluteReturnPpm ||
      value < -kMaximumAbsoluteReturnPpm || value > kMaximumAbsoluteReturnPpm) {
    return false;
  }
  const auto product = value * coefficient_ppm;
  const auto term = product / kScale;
  if ((term > 0 && score > std::numeric_limits<std::int64_t>::max() - term) ||
      (term < 0 && score < std::numeric_limits<std::int64_t>::min() - term)) {
    return false;
  }
  score += term;
  return true;
}

} // namespace

ModelPrediction baseline_prediction(const features::FeatureSnapshot& snapshot,
                                    const std::int64_t expected_return_ppm) noexcept {
  ModelPrediction prediction{};
  prediction.expected_return_ppm = bounded_return(expected_return_ppm);
  prediction.return_p50_ppm = prediction.expected_return_ppm;
  const auto volatility_ppm = volatility(snapshot);
  prediction.volatility_ppm = volatility_ppm;
  const auto spread = static_cast<std::int64_t>(volatility_ppm);
  prediction.return_p10_ppm = bounded_return(prediction.expected_return_ppm - spread);
  prediction.return_p90_ppm = bounded_return(prediction.expected_return_ppm + spread);
  prediction.probability_flat_ppm = 100'000U;
  const auto directional_score = sigmoid_ppm(prediction.expected_return_ppm);
  prediction.probability_up_ppm = static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(directional_score) * 900'000U) / kProbabilityScale);
  prediction.probability_down_ppm = 900'000U - prediction.probability_up_ppm;
  prediction.confidence_ppm =
      snapshot.state == features::EngineState::ready ? 800'000U : 400'000U;
  prediction.calibration_score_ppm = 500'000U;
  prediction.data_quality_score_ppm =
      snapshot.state == features::EngineState::ready ? kProbabilityScale : 500'000U;
  prediction.ood_score_ppm = 0U;
  return prediction;
}

PredictionStatus ZeroReturnModel::predict(const ModelInput& input,
                                          ModelPrediction& output) noexcept {
  output = baseline_prediction(input.feature_snapshot, 0);
  return PredictionStatus::ok;
}

PredictionStatus LastValueModel::predict(const ModelInput& input,
                                         ModelPrediction& output) noexcept {
  output = baseline_prediction(
      input.feature_snapshot,
      input.feature_snapshot.get(features::FeatureName::rolling_return_ppm).value);
  return PredictionStatus::ok;
}

// Both sizes are explicit configuration dimensions and are validated together.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
MovingAverageModel::MovingAverageModel(ModelMetadata metadata,
                                       const std::size_t window_size,
                                       const std::size_t minimum_samples) noexcept
    : metadata_(metadata),
      window_size_(std::min(window_size, kMaximumMovingAverageSamples)),
      minimum_samples_(minimum_samples) {}
// NOLINTEND(bugprone-easily-swappable-parameters)

PredictionStatus MovingAverageModel::predict(const ModelInput& input,
                                             ModelPrediction& output) noexcept {
  if (window_size_ == 0U || minimum_samples_ == 0U || minimum_samples_ > window_size_) {
    return PredictionStatus::invalid_input;
  }
  const auto observed = bounded_return(
      input.feature_snapshot.get(features::FeatureName::rolling_return_ppm).value);
  if (size_ == window_size_) {
    sum_ -= values_[next_];
  } else {
    ++size_;
  }
  values_[next_] = observed;
  sum_ += observed;
  next_ = (next_ + 1U) % window_size_;
  if (size_ < minimum_samples_) {
    return PredictionStatus::warming;
  }
  output = baseline_prediction(input.feature_snapshot,
                               sum_ / static_cast<std::int64_t>(size_));
  return PredictionStatus::ok;
}

void MovingAverageModel::reset() noexcept {
  values_.fill(0);
  size_ = 0U;
  next_ = 0U;
  sum_ = 0;
}

LinearRegressionModel::LinearRegressionModel(
    ModelMetadata metadata, const std::int64_t intercept_ppm,
    const std::array<RegressionCoefficient, kMaximumRegressionFeatures> coefficients,
    const std::size_t coefficient_count) noexcept
    : metadata_(metadata), intercept_ppm_(intercept_ppm), coefficients_(coefficients),
      coefficient_count_(coefficient_count) {}

PredictionStatus LinearRegressionModel::score(const ModelInput& input,
                                              std::int64_t& output) const noexcept {
  if (coefficient_count_ > coefficients_.size() ||
      intercept_ppm_ < -kMaximumAbsoluteReturnPpm ||
      intercept_ppm_ > kMaximumAbsoluteReturnPpm) {
    return PredictionStatus::invalid_input;
  }
  auto score = intercept_ppm_;
  for (std::size_t index = 0U; index < coefficient_count_; ++index) {
    const auto coefficient = coefficients_[index];
    bool declared = false;
    for (std::size_t requirement_index = 0U;
         requirement_index < metadata_.requirement_count; ++requirement_index) {
      declared = declared ||
                 metadata_.requirements[requirement_index].name == coefficient.feature;
    }
    if (coefficient.feature >= features::FeatureName::count || !declared ||
        !safe_accumulate(input.feature_snapshot.get(coefficient.feature).value,
                         coefficient.coefficient_ppm, score)) {
      return PredictionStatus::numeric_error;
    }
  }
  output = bounded_return(score);
  return PredictionStatus::ok;
}

PredictionStatus LinearRegressionModel::predict(const ModelInput& input,
                                                ModelPrediction& output) noexcept {
  std::int64_t result{};
  const auto status = score(input, result);
  if (status != PredictionStatus::ok) {
    return status;
  }
  output = baseline_prediction(input.feature_snapshot, result);
  return PredictionStatus::ok;
}

LogisticRegressionModel::LogisticRegressionModel(
    ModelMetadata metadata, const std::int64_t intercept_ppm,
    const std::array<RegressionCoefficient, kMaximumRegressionFeatures> coefficients,
    const std::size_t coefficient_count) noexcept
    : linear_(metadata, intercept_ppm, coefficients, coefficient_count) {}

PredictionStatus LogisticRegressionModel::predict(const ModelInput& input,
                                                  ModelPrediction& output) noexcept {
  ModelPrediction linear_output{};
  const auto status = linear_.predict(input, linear_output);
  if (status != PredictionStatus::ok) {
    return status;
  }
  const auto up = sigmoid_ppm(linear_output.expected_return_ppm);
  output = baseline_prediction(input.feature_snapshot,
                               static_cast<std::int64_t>(up) - 500'000LL);
  output.probability_flat_ppm = 0U;
  output.probability_up_ppm = up;
  output.probability_down_ppm = kProbabilityScale - up;
  return PredictionStatus::ok;
}

SeasonalNaiveModel::SeasonalNaiveModel(ModelMetadata metadata,
                                       const std::size_t bins) noexcept
    : metadata_(metadata), bins_(bins) {}

PredictionStatus SeasonalNaiveModel::predict(const ModelInput& input,
                                             ModelPrediction& output) noexcept {
  if (bins_ == 0U || bins_ > kMaximumSeasonalBins) {
    return PredictionStatus::invalid_input;
  }
  const auto progress =
      input.feature_snapshot.get(features::FeatureName::session_progress_ppm).value;
  if (progress < 0 || progress > 1'000'000LL) {
    return PredictionStatus::invalid_input;
  }
  auto bin = static_cast<std::size_t>((static_cast<std::uint64_t>(progress) * bins_) /
                                      kProbabilityScale);
  if (bin == bins_) {
    --bin;
  }
  const auto current = bounded_return(
      input.feature_snapshot.get(features::FeatureName::rolling_return_ppm).value);
  const auto available = populated_[bin];
  const auto previous = values_[bin];
  values_[bin] = current;
  populated_[bin] = true;
  if (!available) {
    return PredictionStatus::warming;
  }
  output = baseline_prediction(input.feature_snapshot, previous);
  return PredictionStatus::ok;
}

void SeasonalNaiveModel::reset() noexcept {
  values_.fill(0);
  populated_.fill(false);
}

} // namespace aegis::models
