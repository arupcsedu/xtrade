#ifndef AEGIS_MODELS_VALIDATOR_HPP
#define AEGIS_MODELS_VALIDATOR_HPP

#include "aegis/models/types.hpp"

namespace aegis::models {

class ModelForecastValidator final {
public:
  [[nodiscard]] static ForecastValidationError
  validate_input(const ModelMetadata& metadata, const ModelInput& input,
                 std::uint64_t now_process_monotonic_time_ns) noexcept;

  [[nodiscard]] static ForecastValidationError
  validate_forecast(const ModelForecast& forecast,
                    std::uint64_t now_process_monotonic_time_ns) noexcept;

  // External adapters use binary floating point only at this boundary. Values
  // are range-checked, rejected on NaN/Inf, and converted to integer PPM before
  // publication to the common forecast contract.
  [[nodiscard]] static ForecastValidationError
  convert_external(const ExternalForecastValues& external,
                   ModelPrediction& output) noexcept;
};

} // namespace aegis::models

#endif // AEGIS_MODELS_VALIDATOR_HPP
