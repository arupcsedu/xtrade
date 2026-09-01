#include "aegis/models/calibration.hpp"

#include <gtest/gtest.h>

#include <array>
#include <limits>

namespace models = aegis::models;

TEST(CalibrationTest, IsotonicRejectsNonMonotonicConfigAndCalibrates) {
  std::array<std::uint32_t, models::kMaximumCalibrationPoints> thresholds{};
  std::array<std::uint32_t, models::kMaximumCalibrationPoints> calibrated{};
  thresholds[0U] = 200'000U;
  thresholds[1U] = 600'000U;
  thresholds[2U] = 1'000'000U;
  calibrated[0U] = 100'000U;
  calibrated[1U] = 550'000U;
  calibrated[2U] = 900'000U;
  models::IsotonicCalibrator calibrator;
  EXPECT_EQ(calibrator.configure(thresholds, calibrated, 3U),
            models::CalibrationStatus::ok);
  std::uint32_t output{};
  EXPECT_EQ(calibrator.calibrate(400'000U, output), models::CalibrationStatus::ok);
  EXPECT_EQ(output, 550'000U);
  calibrated[1U] = 50'000U;
  EXPECT_EQ(calibrator.configure(thresholds, calibrated, 3U),
            models::CalibrationStatus::invalid_configuration);
}

TEST(CalibrationTest, PlattScalingRejectsNonFiniteAndQuantizes) {
  const models::PlattScaler scaler{-1.0, 0.0};
  std::uint32_t output{};
  EXPECT_EQ(scaler.calibrate(0.0, output), models::CalibrationStatus::ok);
  EXPECT_EQ(output, 500'000U);
  EXPECT_EQ(scaler.calibrate(std::numeric_limits<double>::infinity(), output),
            models::CalibrationStatus::non_finite_input);
}

TEST(CalibrationTest, QuantileCoverageReportsFixedPointCoverage) {
  models::QuantileCoverageChecker checker{800'000U, 1U};
  EXPECT_EQ(checker.observe(-10, 10, 0), models::CalibrationStatus::ok);
  EXPECT_EQ(checker.observe(-10, 10, 5), models::CalibrationStatus::ok);
  EXPECT_EQ(checker.observe(-10, 10, -5), models::CalibrationStatus::ok);
  EXPECT_EQ(checker.observe(-10, 10, 9), models::CalibrationStatus::ok);
  EXPECT_EQ(checker.observe(-10, 10, 11), models::CalibrationStatus::ok);
  const auto snapshot = checker.snapshot();
  EXPECT_EQ(snapshot.observations, 5U);
  EXPECT_EQ(snapshot.covered, 4U);
  EXPECT_EQ(snapshot.coverage_ppm, 800'000U);
  EXPECT_TRUE(snapshot.within_tolerance);
}
