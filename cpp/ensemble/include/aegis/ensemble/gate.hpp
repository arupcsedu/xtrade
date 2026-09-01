#ifndef AEGIS_ENSEMBLE_GATE_HPP
#define AEGIS_ENSEMBLE_GATE_HPP

#include "aegis/ensemble/types.hpp"

namespace aegis::ensemble {

class MixtureOfExpertsGate final {
public:
  explicit MixtureOfExpertsGate(EnsembleConfig config) noexcept;

  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] const EnsembleConfig& config() const noexcept;
  [[nodiscard]] EvaluationResult
  evaluate(const EnsembleRequest& request) const noexcept;

private:
  EnsembleConfig config_;
  bool initialized_{false};
};

} // namespace aegis::ensemble

#endif // AEGIS_ENSEMBLE_GATE_HPP
