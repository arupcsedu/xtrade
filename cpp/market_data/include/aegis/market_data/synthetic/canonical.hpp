#ifndef AEGIS_MARKET_DATA_SYNTHETIC_CANONICAL_HPP
#define AEGIS_MARKET_DATA_SYNTHETIC_CANONICAL_HPP

#include "aegis/common/sha256.hpp"
#include "aegis/market_data/synthetic/config.hpp"
#include "aegis/market_data/synthetic/event.hpp"

#include <flatbuffers/detached_buffer.h>

namespace aegis::market_data::synthetic {

// Canonical builders allocate and hash at the offline/file boundary. They are
// deliberately not part of SyntheticExchangeGenerator::next().
[[nodiscard]] flatbuffers::DetachedBuffer
build_market_event_contract(const SyntheticEvent& event, const GeneratorConfig& config);

[[nodiscard]] flatbuffers::DetachedBuffer
build_market_event_envelope(const SyntheticEvent& event, const GeneratorConfig& config,
                            const aegis::common::Sha256Digest& previous_digest);

} // namespace aegis::market_data::synthetic

#endif // AEGIS_MARKET_DATA_SYNTHETIC_CANONICAL_HPP
