#ifndef AEGIS_ENSEMBLE_SERIALIZATION_HPP
#define AEGIS_ENSEMBLE_SERIALIZATION_HPP

#include "aegis/common/audit_envelope.hpp"
#include "aegis/ensemble/types.hpp"

#include <flatbuffers/flatbuffer_builder.h>

namespace aegis::ensemble {

struct EnsembleContractInput {
  common::GlobalEventId record_id;
  EnsembleForecast forecast;
};

// Serialization is a control/journal boundary and may allocate inside the
// FlatBuffer builder. The gate itself never calls this function.
[[nodiscard]] flatbuffers::DetachedBuffer
build_ensemble_forecast_contract(const EnsembleContractInput& input);

} // namespace aegis::ensemble

#endif // AEGIS_ENSEMBLE_SERIALIZATION_HPP
