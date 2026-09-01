#ifndef AEGIS_MODELS_CALIBRATION_HPP
#define AEGIS_MODELS_CALIBRATION_HPP

#include "aegis/models/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace aegis::models {

inline constexpr std::size_t kMaximumCalibrationPoints = 64U;

enum class CalibrationStatus : std::uint8_t {
  ok = 1,
  invalid_configuration,
  non_finite_input,
  insufficient_samples,
};

class IsotonicCalibrator final {
public:
  [[nodiscard]] CalibrationStatus
  configure(const std::array<std::uint32_t, kMaximumCalibrationPoints>& thresholds_ppm,
            const std::array<std::uint32_t, kMaximumCalibrationPoints>& calibrated_ppm,
            std::size_t count) noexcept;

  [[nodiscard]] CalibrationStatus calibrate(std::uint32_t raw_probability_ppm,
                                            std::uint32_t& output_ppm) const noexcept;

private:
  std::array<std::uint32_t, kMaximumCalibrationPoints> thresholds_{};
  std::array<std::uint32_t, kMaximumCalibrationPoints> calibrated_{};
  std::size_t count_{};
};

class PlattScaler final {
public:
  // Coefficients retain the conventional ordered (slope, intercept) form.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  PlattScaler(double slope, double intercept) noexcept
      : slope_(slope), intercept_(intercept) {}

  // This is an offline/model-adapter utility. Its floating-point output is
  // immediately quantized into PPM and never enters the canonical contract.
  [[nodiscard]] CalibrationStatus calibrate(double score,
                                            std::uint32_t& output_ppm) const noexcept;

private:
  double slope_{};
  double intercept_{};
};

struct QuantileCoverageSnapshot {
  std::uint64_t observations{};
  std::uint64_t covered{};
  std::uint32_t coverage_ppm{};
  bool within_tolerance{false};
};

class QuantileCoverageChecker final {
public:
  // Both PPM values are deliberately adjacent calibration configuration.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  QuantileCoverageChecker(std::uint32_t expected_coverage_ppm,
                          std::uint32_t tolerance_ppm) noexcept;

  [[nodiscard]] CalibrationStatus observe(std::int64_t lower_ppm,
                                          std::int64_t upper_ppm,
                                          std::int64_t realized_ppm) noexcept;
  [[nodiscard]] QuantileCoverageSnapshot snapshot() const noexcept;
  void reset() noexcept;

private:
  std::uint32_t expected_coverage_ppm_{};
  std::uint32_t tolerance_ppm_{};
  std::uint64_t observations_{};
  std::uint64_t covered_{};
};

} // namespace aegis::models

#endif // AEGIS_MODELS_CALIBRATION_HPP
