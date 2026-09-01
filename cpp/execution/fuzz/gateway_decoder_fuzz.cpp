#include "aegis/execution/decoders.hpp"
#include "aegis/execution/session.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      const std::size_t size) {
  aegis::execution::GatewayEvent event{};
  std::memcpy(&event, data, std::min(size, sizeof(event)));
  event.stable_hash = aegis::execution::stable_gateway_event_hash(event);
  aegis::oms::OmsInput output{};
  const auto acknowledgement =
      aegis::execution::AcknowledgementDecoder{}.decode(event, output);
  if (acknowledgement == aegis::execution::AdapterStatus::decoded &&
      !aegis::oms::valid_input(output)) {
    __builtin_trap();
  }
  const auto rejection = aegis::execution::RejectDecoder{}.decode(event, output);
  if (rejection == aegis::execution::AdapterStatus::decoded &&
      !aegis::oms::valid_input(output)) {
    __builtin_trap();
  }
  const auto execution = aegis::execution::ExecutionDecoder{}.decode(event, output);
  if (execution == aegis::execution::AdapterStatus::decoded &&
      !aegis::oms::valid_input(output)) {
    __builtin_trap();
  }
  if (size >= sizeof(std::uint64_t)) {
    std::uint64_t sequence{};
    std::memcpy(&sequence, data, sizeof(sequence));
    aegis::execution::SequenceManager manager{1U, 1U};
    static_cast<void>(manager.observe_inbound(sequence));
  }
  return 0;
}
