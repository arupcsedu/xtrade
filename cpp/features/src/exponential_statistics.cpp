#include "aegis/features/exponential_statistics.hpp"

#include <cstdint>
#include <limits>
#include <numeric>

namespace aegis::features {
namespace {

[[nodiscard]] bool absolute(const std::int64_t value, std::uint64_t& output) noexcept {
  if (value == std::numeric_limits<std::int64_t>::min()) {
    return false;
  }
  output = static_cast<std::uint64_t>(value < 0 ? -value : value);
  return true;
}

// Internal arithmetic order is value, multiplier, divisor at every call site.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] bool multiply_divide(const std::int64_t value,
                                   const std::uint64_t multiplier,
                                   const std::uint64_t divisor,
                                   std::int64_t& output) noexcept {
  if (divisor == 0U) {
    return false;
  }
  std::uint64_t magnitude{};
  if (!absolute(value, magnitude)) {
    return false;
  }
  const auto first = std::gcd(magnitude, divisor);
  magnitude /= first;
  auto remaining_divisor = divisor / first;
  const auto second = std::gcd(multiplier, remaining_divisor);
  const auto reduced_multiplier = multiplier / second;
  remaining_divisor /= second;
  std::uint64_t product{};
  if (__builtin_mul_overflow(magnitude, reduced_multiplier, &product)) {
    return false;
  }
  const auto quotient = product / remaining_divisor;
  if (quotient > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  const auto signed_quotient = static_cast<std::int64_t>(quotient);
  output = value < 0 ? -signed_quotient : signed_quotient;
  return true;
}

} // namespace

ExponentialMovingStatistics::ExponentialMovingStatistics(
    const std::uint32_t alpha_ppm, const std::int64_t maximum_absolute_sample) noexcept
    : alpha_ppm_(alpha_ppm), maximum_absolute_sample_(maximum_absolute_sample),
      configuration_valid_(alpha_ppm != 0U && alpha_ppm <= kScalePpm &&
                           maximum_absolute_sample > 0 &&
                           maximum_absolute_sample <= 1'000'000'000LL) {}

bool ExponentialMovingStatistics::observe(const std::int64_t sample) noexcept {
  if (!configuration_valid_ || sample > maximum_absolute_sample_ ||
      sample < -maximum_absolute_sample_) {
    return false;
  }
  if (count_ == 0U) {
    if (__builtin_mul_overflow(sample, static_cast<std::int64_t>(kScalePpm),
                               &mean_ppm_)) {
      return false;
    }
    count_ = 1U;
    return true;
  }

  std::int64_t scaled_sample{};
  std::int64_t delta{};
  std::int64_t adjustment{};
  if (__builtin_mul_overflow(sample, static_cast<std::int64_t>(kScalePpm),
                             &scaled_sample) ||
      __builtin_sub_overflow(scaled_sample, mean_ppm_, &delta) ||
      !multiply_divide(delta, alpha_ppm_, kScalePpm, adjustment) ||
      __builtin_add_overflow(mean_ppm_, adjustment, &mean_ppm_)) {
    return false;
  }

  const auto rounded_mean = mean_ppm_ / static_cast<std::int64_t>(kScalePpm);
  std::int64_t residual{};
  if (__builtin_sub_overflow(sample, rounded_mean, &residual)) {
    return false;
  }
  std::uint64_t magnitude{};
  std::uint64_t squared{};
  if (!absolute(residual, magnitude) ||
      __builtin_mul_overflow(magnitude, magnitude, &squared) ||
      squared > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      variance_units_squared_ >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  const auto variance = static_cast<std::int64_t>(variance_units_squared_);
  const auto squared_signed = static_cast<std::int64_t>(squared);
  std::int64_t variance_delta{};
  std::int64_t variance_adjustment{};
  std::int64_t next_variance{};
  if (__builtin_sub_overflow(squared_signed, variance, &variance_delta) ||
      !multiply_divide(variance_delta, alpha_ppm_, kScalePpm, variance_adjustment) ||
      __builtin_add_overflow(variance, variance_adjustment, &next_variance) ||
      next_variance < 0) {
    return false;
  }
  variance_units_squared_ = static_cast<std::uint64_t>(next_variance);
  ++count_;
  return true;
}

void ExponentialMovingStatistics::reset() noexcept {
  mean_ppm_ = 0;
  variance_units_squared_ = 0U;
  count_ = 0U;
}

bool ExponentialMovingStatistics::valid() const noexcept {
  return configuration_valid_ && count_ != 0U;
}

std::uint64_t ExponentialMovingStatistics::count() const noexcept { return count_; }

std::int64_t ExponentialMovingStatistics::mean_ppm() const noexcept {
  return mean_ppm_;
}

std::uint64_t ExponentialMovingStatistics::variance_units_squared() const noexcept {
  return variance_units_squared_;
}

} // namespace aegis::features
