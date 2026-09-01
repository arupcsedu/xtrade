#ifndef AEGIS_FEATURES_EXPONENTIAL_STATISTICS_HPP
#define AEGIS_FEATURES_EXPONENTIAL_STATISTICS_HPP

#include <cstdint>

namespace aegis::features {

class ExponentialMovingStatistics final {
public:
  static constexpr std::uint32_t kScalePpm = 1'000'000U;

  explicit ExponentialMovingStatistics(std::uint32_t alpha_ppm,
                                       std::int64_t maximum_absolute_sample) noexcept;

  [[nodiscard]] bool observe(std::int64_t sample) noexcept;
  void reset() noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint64_t count() const noexcept;
  [[nodiscard]] std::int64_t mean_ppm() const noexcept;
  [[nodiscard]] std::uint64_t variance_units_squared() const noexcept;

private:
  std::uint32_t alpha_ppm_{};
  std::int64_t maximum_absolute_sample_{};
  std::int64_t mean_ppm_{};
  std::uint64_t variance_units_squared_{};
  std::uint64_t count_{};
  bool configuration_valid_{false};
};

} // namespace aegis::features

#endif // AEGIS_FEATURES_EXPONENTIAL_STATISTICS_HPP
