#include "aegis/time/latency.hpp"

namespace aegis::time {
namespace {

[[nodiscard]] LatencyDecompositionResult failure(const LatencyStage stage,
                                                 const TimeError error) noexcept {
  return {.failed_stage = stage, .error = error};
}

} // namespace

LatencyDecompositionResult decompose_latency(const LatencyTrace& trace) noexcept {
  LatencyDecomposition result{};

  const auto wire_to_decode =
      wire_to_decode_latency(trace.local_receive_time, trace.decode_complete_time);
  if (!wire_to_decode.ok()) {
    return failure(LatencyStage::wire_to_decode, wire_to_decode.error);
  }
  result.wire_to_decode = wire_to_decode.value;

  const auto decode_to_book =
      decode_to_book_latency(trace.decode_complete_time, trace.book_complete_time);
  if (!decode_to_book.ok()) {
    return failure(LatencyStage::decode_to_book, decode_to_book.error);
  }
  result.decode_to_book = decode_to_book.value;

  const auto book_to_feature =
      book_to_feature_latency(trace.book_complete_time, trace.feature_complete_time);
  if (!book_to_feature.ok()) {
    return failure(LatencyStage::book_to_feature, book_to_feature.error);
  }
  result.book_to_feature = book_to_feature.value;

  const auto feature_to_inference =
      checked_elapsed(trace.feature_complete_time, trace.inference_start_time);
  if (!feature_to_inference.ok()) {
    return failure(LatencyStage::feature_to_inference, feature_to_inference.error);
  }

  const auto inference =
      inference_latency(trace.inference_start_time, trace.inference_complete_time);
  if (!inference.ok()) {
    return failure(LatencyStage::inference, inference.error);
  }
  result.inference = inference.value;

  const auto inference_to_decision =
      checked_elapsed(trace.inference_complete_time, trace.decision_complete_time);
  if (!inference_to_decision.ok()) {
    return failure(LatencyStage::inference_to_decision, inference_to_decision.error);
  }

  const auto decision_to_send =
      decision_to_send_latency(trace.decision_complete_time, trace.send_handoff_time);
  if (!decision_to_send.ok()) {
    return failure(LatencyStage::decision_to_send, decision_to_send.error);
  }
  result.decision_to_send = decision_to_send.value;

  const auto round_trip =
      order_round_trip_latency(trace.send_handoff_time, trace.acknowledgement_time);
  if (!round_trip.ok()) {
    return failure(LatencyStage::order_round_trip, round_trip.error);
  }
  result.order_round_trip = round_trip.value;

  return {.value = result};
}

} // namespace aegis::time
