#include "aegis/time/monotonicity.hpp"
#include "aegis/time/time_types.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <type_traits>

namespace aegis::time {
namespace {

static_assert(!std::is_same_v<WallClockTimeNs, MonotonicTimeNs>);
static_assert(!std::is_same_v<ExchangeTimeNs, HardwareReceiveTimeNs>);

TEST(TimeTypesTest, AddsAndSubtractsDurationsWithoutDomainConversion) {
  constexpr MonotonicTimeNs initial{100U};
  constexpr auto advanced = checked_add(initial, DurationNs{25});
  static_assert(advanced.ok());
  EXPECT_EQ(advanced.value, MonotonicTimeNs{125U});

  constexpr auto restored = checked_subtract(advanced.value, DurationNs{25});
  static_assert(restored.ok());
  EXPECT_EQ(restored.value, initial);
}

TEST(TimeTypesTest, ReportsEveryIntegerBoundaryFailure) {
  constexpr auto signed_overflow = checked_add(
      WallClockTimeNs{std::numeric_limits<std::int64_t>::max()}, DurationNs{1});
  constexpr auto signed_underflow = checked_add(
      WallClockTimeNs{std::numeric_limits<std::int64_t>::min()}, DurationNs{-1});
  constexpr auto unsigned_overflow = checked_add(
      MonotonicTimeNs{std::numeric_limits<std::uint64_t>::max()}, DurationNs{1});
  constexpr auto unsigned_underflow = checked_add(MonotonicTimeNs{0U}, DurationNs{-1});

  EXPECT_EQ(signed_overflow.error, TimeError::overflow);
  EXPECT_EQ(signed_underflow.error, TimeError::underflow);
  EXPECT_EQ(unsigned_overflow.error, TimeError::overflow);
  EXPECT_EQ(unsigned_underflow.error, TimeError::underflow);
}

TEST(TimeTypesTest, ReportsDurationOverflowAndMinimumMagnitude) {
  constexpr auto overflow =
      checked_add(DurationNs{std::numeric_limits<std::int64_t>::max()}, DurationNs{1});
  EXPECT_EQ(overflow.error, TimeError::overflow);
  EXPECT_EQ(magnitude(DurationNs{std::numeric_limits<std::int64_t>::min()}),
            std::uint64_t{1} << 63U);
}

TEST(TimeTypesTest, ElapsedRejectsRegressionAndUnrepresentableDuration) {
  const auto regression = checked_elapsed(MonotonicTimeNs{20U}, MonotonicTimeNs{19U});
  const auto overflow =
      checked_elapsed(MonotonicTimeNs{0U}, MonotonicTimeNs{std::uint64_t{1} << 63U});

  EXPECT_EQ(regression.error, TimeError::regression);
  EXPECT_EQ(overflow.error, TimeError::overflow);
}

TEST(MonotonicityValidatorTest, StrictModeDoesNotAdvanceOnInvalidSamples) {
  MonotonicityValidator<ExchangeTimeNs> validator;

  EXPECT_EQ(validator.observe(ExchangeTimeNs{10}).status, MonotonicityStatus::first);
  EXPECT_FALSE(validator.observe(ExchangeTimeNs{10}).accepted);
  EXPECT_FALSE(validator.observe(ExchangeTimeNs{9}).accepted);
  EXPECT_EQ(validator.last(), ExchangeTimeNs{10});
  EXPECT_TRUE(validator.observe(ExchangeTimeNs{11}).accepted);
}

TEST(MonotonicityValidatorTest, EqualSamplesCanBeExplicitlyPermitted) {
  MonotonicityValidator<HardwareReceiveTimeNs> validator{true};
  EXPECT_TRUE(validator.observe(HardwareReceiveTimeNs{7}).accepted);
  const auto equal = validator.observe(HardwareReceiveTimeNs{7});
  EXPECT_EQ(equal.status, MonotonicityStatus::equal);
  EXPECT_TRUE(equal.accepted);
}

} // namespace
} // namespace aegis::time
