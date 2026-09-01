#ifndef AEGIS_RISK_SERIALIZATION_HPP
#define AEGIS_RISK_SERIALIZATION_HPP

#include "aegis/common/audit_envelope.hpp"
#include "aegis/risk/portfolio_types.hpp"
#include "aegis/risk/types.hpp"

#include <flatbuffers/flatbuffer_builder.h>

#include <cstddef>

namespace aegis::risk {

[[nodiscard]] flatbuffers::DetachedBuffer
build_risk_decision_contract(const RiskDecision& decision);

[[nodiscard]] flatbuffers::DetachedBuffer
build_position_snapshot_contract(const portfolio::PortfolioSnapshot& snapshot,
                                 std::size_t position_index,
                                 common::GlobalEventId global_event_id);

} // namespace aegis::risk

#endif // AEGIS_RISK_SERIALIZATION_HPP
