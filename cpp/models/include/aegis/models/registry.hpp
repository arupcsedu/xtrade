#ifndef AEGIS_MODELS_REGISTRY_HPP
#define AEGIS_MODELS_REGISTRY_HPP

#include "aegis/models/types.hpp"

namespace aegis::models {

// Registry access is a control-plane boundary. Runners are constructed from a
// resolved metadata snapshot and never call this interface while forecasting.
class ModelRegistryClient {
public:
  virtual ~ModelRegistryClient() = default;

  [[nodiscard]] virtual RegistryStatus lookup(common::ModelId model_id,
                                              common::ModelVersion model_version,
                                              ModelMetadata& output) noexcept = 0;

  [[nodiscard]] virtual RegistryStatus health(common::ModelId model_id,
                                              common::ModelVersion model_version,
                                              ModelHealthSnapshot& output) noexcept = 0;
};

} // namespace aegis::models

#endif // AEGIS_MODELS_REGISTRY_HPP
