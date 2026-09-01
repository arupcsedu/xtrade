#ifndef AEGIS_OMS_SERIALIZATION_HPP
#define AEGIS_OMS_SERIALIZATION_HPP

#include "aegis/common/audit_envelope.hpp"
#include "aegis/oms/types.hpp"

#include <flatbuffers/flatbuffer_builder.h>

namespace aegis::oms {

// Serialization allocates at the journal/export boundary. The OMS state
// transition and gateway-command path never invokes this builder.
[[nodiscard]] flatbuffers::DetachedBuffer
build_order_event_contract(const OmsInput& input, const ApplyResult& result,
                           const OmsOrderSnapshot& order);

} // namespace aegis::oms

#endif // AEGIS_OMS_SERIALIZATION_HPP
