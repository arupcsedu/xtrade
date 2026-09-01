#ifndef AEGIS_EXECUTION_DECODERS_HPP
#define AEGIS_EXECUTION_DECODERS_HPP

#include "aegis/execution/types.hpp"

namespace aegis::execution {

class AcknowledgementDecoder final {
public:
  [[nodiscard]] static AdapterStatus decode(const GatewayEvent& event,
                                            oms::OmsInput& output) noexcept;
};

class RejectDecoder final {
public:
  [[nodiscard]] static AdapterStatus decode(const GatewayEvent& event,
                                            oms::OmsInput& output) noexcept;
};

class ExecutionDecoder final {
public:
  [[nodiscard]] static AdapterStatus decode(const GatewayEvent& event,
                                            oms::OmsInput& output) noexcept;
};

} // namespace aegis::execution

#endif // AEGIS_EXECUTION_DECODERS_HPP
