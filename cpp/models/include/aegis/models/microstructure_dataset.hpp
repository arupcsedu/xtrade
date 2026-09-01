#ifndef AEGIS_MODELS_MICROSTRUCTURE_DATASET_HPP
#define AEGIS_MODELS_MICROSTRUCTURE_DATASET_HPP

#include "aegis/market_data/synthetic/config.hpp"

#include <cstdint>
#include <iosfwd>

namespace aegis::models {

struct MicrostructureDatasetSummary {
  std::uint64_t seed{};
  std::uint64_t source_event_count{};
  std::uint64_t row_count{};
  std::uint64_t stable_hash{};
};

[[nodiscard]] bool write_synthetic_microstructure_dataset(
    const market_data::synthetic::GeneratorConfig& config, std::ostream& output,
    MicrostructureDatasetSummary& summary) noexcept;

} // namespace aegis::models

#endif // AEGIS_MODELS_MICROSTRUCTURE_DATASET_HPP
