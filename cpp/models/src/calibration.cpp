#include "aegis/models/calibration.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace aegis::models {

CalibrationStatus IsotonicCalibrator::configure(
    const std::array<std::uint32_t, kMaximumCalibrationPoints>& thresholds_ppm,
    const std::array<std::uint32_t, kMaximumCalibrationPoints>& calibrated_ppm,
    const std::size_t count) noexcept {
  if (count == 0U || count > thresholds_.size()) {
    return CalibrationStatus::invalid_configuration;
  }
  for (std::size_t index = 0U; index < count; ++index) {
    if (thresholds_ppm[index] > kProbabilityScale ||
        calibrated_ppm[index] > kProbabilityScale ||
        (index > 0U && (thresholds_ppm[index] <= thresholds_ppm[index - 1U] ||
                        calibrated_ppm[index] < calibrated_ppm[index - 1U]))) {
      return CalibrationStatus::invalid_configuration;
    }
  }
  thresholds_ = thresholds_ppm;
  calibrated_ = calibrated_ppm;
  count_ = count;
  return CalibrationStatus::ok;
}

CalibrationStatus
IsotonicCalibrator::calibrate(const std::uint32_t raw_probability_ppm,
                              std::uint32_t& output_ppm) const noexcept {
  if (count_ == 0U || raw_probability_ppm > kProbabilityScale) {
    return CalibrationStatus::invalid_configuration;
  }
  for (std::size_t index = 0U; index < count_; ++index) {
    if (raw_probability_ppm <= thresholds_[index]) {
      output_ppm = calibrated_[index];
      return CalibrationStatus::ok;
    }
  }
  output_ppm = calibrated_[count_ - 1U];
  return CalibrationStatus::ok;
}

CalibrationStatus PlattScaler::calibrate(const double score,
                                         std::uint32_t& output_ppm) const noexcept {
  if (!std::isfinite(score) || !std::isfinite(slope_) || !std::isfinite(intercept_)) {
    return CalibrationStatus::non_finite_input;
  }
  const auto logit = (slope_ * score) + intercept_;
  if (!std::isfinite(logit)) {
    return CalibrationStatus::non_finite_input;
  }
  double probability{};
  if (logit >= 0.0) {
    const auto exponential = std::exp(-logit);
    probability = exponential / (1.0 + exponential);
  } else {
    probability = 1.0 / (1.0 + std::exp(logit));
  }
  output_ppm = static_cast<std::uint32_t>(
      std::llround(std::clamp(probability, 0.0, 1.0) * kProbabilityScale));
  return CalibrationStatus::ok;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
QuantileCoverageChecker::QuantileCoverageChecker(
    const std::uint32_t expected_coverage_ppm,
    const std::uint32_t tolerance_ppm) noexcept
    : expected_coverage_ppm_(expected_coverage_ppm), tolerance_ppm_(tolerance_ppm) {}
// NOLINTEND(bugprone-easily-swappable-parameters)

CalibrationStatus
QuantileCoverageChecker::observe(const std::int64_t lower_ppm,
                                 const std::int64_t upper_ppm,
                                 const std::int64_t realized_ppm) noexcept {
  if (expected_coverage_ppm_ > kProbabilityScale ||
      tolerance_ppm_ > kProbabilityScale || lower_ppm > upper_ppm ||
      observations_ == std::numeric_limits<std::uint64_t>::max()) {
    return CalibrationStatus::invalid_configuration;
  }
  constexpr auto kRescaleThreshold =
      std::numeric_limits<std::uint64_t>::max() / kProbabilityScale;
  if (observations_ >= kRescaleThreshold) {
    observations_ /= 2U;
    covered_ /= 2U;
  }
  ++observations_;
  if (realized_ppm >= lower_ppm && realized_ppm <= upper_ppm) {
    if (covered_ == std::numeric_limits<std::uint64_t>::max()) {
      return CalibrationStatus::invalid_configuration;
    }
    ++covered_;
  }
  return CalibrationStatus::ok;
}

QuantileCoverageSnapshot QuantileCoverageChecker::snapshot() const noexcept {
  if (observations_ == 0U) {
    return {};
  }
  const auto coverage = static_cast<std::uint32_t>(
      (covered_ * static_cast<std::uint64_t>(kProbabilityScale)) / observations_);
  const auto lower = expected_coverage_ppm_ > tolerance_ppm_
                         ? expected_coverage_ppm_ - tolerance_ppm_
                         : 0U;
  const auto upper =
      std::min(kProbabilityScale, expected_coverage_ppm_ + tolerance_ppm_);
  return {.observations = observations_,
          .covered = covered_,
          .coverage_ppm = coverage,
          .within_tolerance = coverage >= lower && coverage <= upper};
}

void QuantileCoverageChecker::reset() noexcept {
  observations_ = 0U;
  covered_ = 0U;
}

} // namespace aegis::models
