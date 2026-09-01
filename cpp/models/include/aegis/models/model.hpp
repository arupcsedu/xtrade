#ifndef AEGIS_MODELS_MODEL_HPP
#define AEGIS_MODELS_MODEL_HPP

#include "aegis/models/types.hpp"

namespace aegis::models {

class IForecastModel {
public:
  virtual ~IForecastModel() = default;

  [[nodiscard]] virtual const ModelMetadata& metadata() const noexcept = 0;
  [[nodiscard]] virtual PredictionStatus predict(const ModelInput& input,
                                                 ModelPrediction& output) noexcept = 0;
};

} // namespace aegis::models

#endif // AEGIS_MODELS_MODEL_HPP
