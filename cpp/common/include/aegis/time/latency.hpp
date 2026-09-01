#ifndef AEGIS_TIME_LATENCY_HPP
#define AEGIS_TIME_LATENCY_HPP

#include "aegis/time/time_types.hpp"

#include <cstdint>

namespace aegis::time {

// Hardware receive time is retained for event ordering and audit. The paired
// monotonic capture is the local latency origin; the two domains are never
// subtracted from one another.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes,
// readability-redundant-member-init)
struct IngressTimestampPair {
  HardwareReceiveTimeNs hardware_receive_time;
  MonotonicTimeNs local_receive_time;
};

[[nodiscard]] constexpr TimeResult<DurationNs>
wire_to_decode_latency(const MonotonicTimeNs local_receive_time,
                       const MonotonicTimeNs decode_complete_time) noexcept {
  return checked_elapsed(local_receive_time, decode_complete_time);
}

[[nodiscard]] constexpr TimeResult<DurationNs>
decode_to_book_latency(const MonotonicTimeNs decode_complete_time,
                       const MonotonicTimeNs book_complete_time) noexcept {
  return checked_elapsed(decode_complete_time, book_complete_time);
}

[[nodiscard]] constexpr TimeResult<DurationNs>
book_to_feature_latency(const MonotonicTimeNs book_complete_time,
                        const MonotonicTimeNs feature_complete_time) noexcept {
  return checked_elapsed(book_complete_time, feature_complete_time);
}

[[nodiscard]] constexpr TimeResult<DurationNs>
inference_latency(const MonotonicTimeNs inference_start_time,
                  const MonotonicTimeNs inference_complete_time) noexcept {
  return checked_elapsed(inference_start_time, inference_complete_time);
}

[[nodiscard]] constexpr TimeResult<DurationNs>
decision_to_send_latency(const MonotonicTimeNs decision_complete_time,
                         const MonotonicTimeNs send_handoff_time) noexcept {
  return checked_elapsed(decision_complete_time, send_handoff_time);
}

[[nodiscard]] constexpr TimeResult<DurationNs>
order_round_trip_latency(const MonotonicTimeNs send_handoff_time,
                         const MonotonicTimeNs acknowledgement_time) noexcept {
  return checked_elapsed(send_handoff_time, acknowledgement_time);
}

struct LatencyTrace {
  MonotonicTimeNs local_receive_time;
  MonotonicTimeNs decode_complete_time;
  MonotonicTimeNs book_complete_time;
  MonotonicTimeNs feature_complete_time;
  MonotonicTimeNs inference_start_time;
  MonotonicTimeNs inference_complete_time;
  MonotonicTimeNs decision_complete_time;
  MonotonicTimeNs send_handoff_time;
  MonotonicTimeNs acknowledgement_time;
};

struct LatencyDecomposition {
  DurationNs wire_to_decode;
  DurationNs decode_to_book;
  DurationNs book_to_feature;
  DurationNs inference;
  DurationNs decision_to_send;
  DurationNs order_round_trip;
};

enum class LatencyStage : std::uint8_t {
  none = 0,
  wire_to_decode,
  decode_to_book,
  book_to_feature,
  feature_to_inference,
  inference,
  inference_to_decision,
  decision_to_send,
  order_round_trip,
};

struct LatencyDecompositionResult {
  LatencyDecomposition value{};
  LatencyStage failed_stage{LatencyStage::none};
  TimeError error{TimeError::none};

  [[nodiscard]] constexpr bool ok() const noexcept { return error == TimeError::none; }
};
// NOLINTEND(misc-non-private-member-variables-in-classes,
// readability-redundant-member-init)

[[nodiscard]] LatencyDecompositionResult
decompose_latency(const LatencyTrace& trace) noexcept;

} // namespace aegis::time

#endif // AEGIS_TIME_LATENCY_HPP
