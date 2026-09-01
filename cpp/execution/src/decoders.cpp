#include "aegis/execution/decoders.hpp"

namespace aegis::execution {
namespace {

[[nodiscard]] oms::OmsInput base_input(const GatewayEvent& event,
                                       const oms::InputKind kind) noexcept {
  oms::OmsInput result{.receipt_id = event.event_id,
                       .kind = kind,
                       .source = oms::EventSource::gateway,
                       .order_id = event.order_id,
                       .external_order_id = event.external_order_id,
                       .execution_id = event.execution_id,
                       .authority = event.authority,
                       .price_ticks = event.price_ticks,
                       .quantity_units = event.quantity_units,
                       .venue_sequence = event.venue_sequence,
                       .exchange_event_time_ns = event.exchange_event_time_ns,
                       .nic_receive_time_ns = event.nic_receive_time_ns,
                       .process_monotonic_time_ns = event.process_monotonic_time_ns};
  result.stable_hash = oms::stable_input_hash(result);
  return result;
}

} // namespace

AdapterStatus AcknowledgementDecoder::decode(const GatewayEvent& event,
                                             oms::OmsInput& output) noexcept {
  if (!valid_gateway_event(event)) {
    return AdapterStatus::malformed;
  }
  oms::InputKind kind{};
  switch (event.kind) {
  case GatewayResponseKind::acknowledgement:
    kind = oms::InputKind::acknowledgement;
    break;
  case GatewayResponseKind::cancel_acknowledgement:
    kind = oms::InputKind::cancel_acknowledgement;
    break;
  case GatewayResponseKind::replace_acknowledgement:
    kind = oms::InputKind::replace_acknowledgement;
    break;
  default:
    return AdapterStatus::unsupported;
  }
  output = base_input(event, kind);
  return oms::valid_input(output) ? AdapterStatus::decoded : AdapterStatus::malformed;
}

AdapterStatus RejectDecoder::decode(const GatewayEvent& event,
                                    oms::OmsInput& output) noexcept {
  if (!valid_gateway_event(event)) {
    return AdapterStatus::malformed;
  }
  oms::InputKind kind{};
  switch (event.kind) {
  case GatewayResponseKind::rejection:
    kind = oms::InputKind::rejection;
    break;
  case GatewayResponseKind::cancel_rejection:
    kind = oms::InputKind::cancel_rejection;
    break;
  case GatewayResponseKind::replace_rejection:
    kind = oms::InputKind::replace_rejection;
    break;
  default:
    return AdapterStatus::unsupported;
  }
  output = base_input(event, kind);
  return oms::valid_input(output) ? AdapterStatus::decoded : AdapterStatus::malformed;
}

AdapterStatus ExecutionDecoder::decode(const GatewayEvent& event,
                                       oms::OmsInput& output) noexcept {
  if (!valid_gateway_event(event)) {
    return AdapterStatus::malformed;
  }
  if (event.kind != GatewayResponseKind::fill) {
    return AdapterStatus::unsupported;
  }
  output = base_input(event, oms::InputKind::fill);
  return oms::valid_input(output) ? AdapterStatus::decoded : AdapterStatus::malformed;
}

} // namespace aegis::execution
