#ifndef AEGIS_MODELS_CACHE_HPP
#define AEGIS_MODELS_CACHE_HPP

#include "aegis/models/validator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace aegis::models {

template <std::size_t Capacity> class ForecastCache final {
public:
  static_assert(Capacity > 0U);

  [[nodiscard]] CacheStatus put(const ModelForecast& forecast,
                                const std::uint64_t now_ns) noexcept {
    if (ModelForecastValidator::validate_forecast(forecast, now_ns) !=
        ForecastValidationError::none) {
      return CacheStatus::invalid_forecast;
    }
    std::size_t free_index = Capacity;
    for (std::size_t index = 0U; index < Capacity; ++index) {
      if (occupied_[index] && entries_[index].model_id == forecast.model_id &&
          entries_[index].instrument_id == forecast.instrument_id) {
        entries_[index] = forecast;
        return CacheStatus::ok;
      }
      if (!occupied_[index] && free_index == Capacity) {
        free_index = index;
      }
    }
    if (free_index == Capacity) {
      return CacheStatus::full;
    }
    entries_[free_index] = forecast;
    occupied_[free_index] = true;
    return CacheStatus::ok;
  }

  [[nodiscard]] CacheStatus get(const common::ModelId model_id,
                                const common::InstrumentId instrument_id,
                                const std::uint64_t now_ns,
                                ModelForecast& output) noexcept {
    for (std::size_t index = 0U; index < Capacity; ++index) {
      if (!occupied_[index] || entries_[index].model_id != model_id ||
          entries_[index].instrument_id != instrument_id) {
        continue;
      }
      if (now_ns > entries_[index].expiration_process_monotonic_time_ns) {
        occupied_[index] = false;
        return CacheStatus::expired;
      }
      if (entries_[index].stable_hash != stable_forecast_hash(entries_[index])) {
        occupied_[index] = false;
        return CacheStatus::corrupt;
      }
      output = entries_[index];
      return CacheStatus::ok;
    }
    return CacheStatus::miss;
  }

  [[nodiscard]] std::size_t expire(const std::uint64_t now_ns) noexcept {
    std::size_t count = 0U;
    for (std::size_t index = 0U; index < Capacity; ++index) {
      if (occupied_[index] &&
          now_ns > entries_[index].expiration_process_monotonic_time_ns) {
        occupied_[index] = false;
        ++count;
      }
    }
    return count;
  }

  [[nodiscard]] std::size_t invalidate_model(const common::ModelId model_id) noexcept {
    std::size_t count = 0U;
    for (std::size_t index = 0U; index < Capacity; ++index) {
      if (occupied_[index] && entries_[index].model_id == model_id) {
        occupied_[index] = false;
        ++count;
      }
    }
    return count;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    std::size_t count = 0U;
    for (const auto occupied : occupied_) {
      count += occupied ? 1U : 0U;
    }
    return count;
  }

private:
  std::array<ModelForecast, Capacity> entries_{};
  std::array<bool, Capacity> occupied_{};
};

template <std::size_t Capacity> class ForecastExpiryManager final {
public:
  [[nodiscard]] std::size_t expire(ForecastCache<Capacity>& cache,
                                   const std::uint64_t now_ns) noexcept {
    const auto count = cache.expire(now_ns);
    expired_count_ += count;
    return count;
  }

  [[nodiscard]] std::uint64_t expired_count() const noexcept { return expired_count_; }

private:
  std::uint64_t expired_count_{};
};

} // namespace aegis::models

#endif // AEGIS_MODELS_CACHE_HPP
