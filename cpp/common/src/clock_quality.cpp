#include "aegis/time/clock_quality.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace aegis::time {
namespace {

[[nodiscard]] constexpr std::uint64_t
integer_magnitude(const std::int64_t value) noexcept {
  if (value >= 0) {
    return static_cast<std::uint64_t>(value);
  }
  return static_cast<std::uint64_t>(-(value + 1)) + 1U;
}

[[nodiscard]] constexpr ClockOperationMode
degraded_mode(const DegradedOperationMode configured_mode) noexcept {
  return configured_mode == DegradedOperationMode::reduce_only
             ? ClockOperationMode::reduce_only
             : ClockOperationMode::blocked;
}

struct EvaluatedQuality {
  std::uint64_t absolute_offset{};
  std::uint64_t absolute_drift{};
  std::uint64_t synchronization_age{};
};

[[nodiscard]] constexpr std::optional<ClockQualityReason>
unsafe_reason(const ClockQualityObservation& observation,
              const EvaluatedQuality& evaluated,
              const ClockQualityThresholds& thresholds) noexcept {
  if (thresholds.require_hardware_timestamps &&
      !observation.hardware_timestamp_available) {
    return ClockQualityReason::hardware_timestamp_unavailable;
  }
  if (evaluated.absolute_offset >= thresholds.unsafe_min_abs_offset_ns) {
    return ClockQualityReason::offset_unsafe;
  }
  if (evaluated.absolute_drift >= thresholds.unsafe_min_abs_drift_ppb) {
    return ClockQualityReason::drift_unsafe;
  }
  if (evaluated.synchronization_age >= thresholds.unsafe_min_sync_age_ns) {
    return ClockQualityReason::sync_stale;
  }
  return std::nullopt;
}

[[nodiscard]] constexpr std::optional<ClockQualityReason>
degraded_reason(const ClockQualityObservation& observation,
                const EvaluatedQuality& evaluated,
                const ClockQualityThresholds& thresholds) noexcept {
  if (!observation.hardware_timestamp_available) {
    return ClockQualityReason::hardware_timestamp_unavailable;
  }
  if (evaluated.absolute_offset > thresholds.healthy_max_abs_offset_ns) {
    return ClockQualityReason::offset_degraded;
  }
  if (evaluated.absolute_drift > thresholds.healthy_max_abs_drift_ppb) {
    return ClockQualityReason::drift_degraded;
  }
  if (evaluated.synchronization_age > thresholds.healthy_max_sync_age_ns) {
    return ClockQualityReason::sync_aging;
  }
  return std::nullopt;
}

void increment_saturating(std::uint64_t& counter) noexcept {
  if (counter != std::numeric_limits<std::uint64_t>::max()) {
    ++counter;
  }
}

[[nodiscard]] bool source_has_changed(const std::optional<ClockSourceId>& previous,
                                      const ClockSourceId current) noexcept {
  return previous.has_value() && current != *previous;
}

} // namespace

std::optional<ClockQualityStateMachine>
ClockQualityStateMachine::create(const ClockQualityThresholds thresholds) noexcept {
  if (!thresholds.valid()) {
    return std::nullopt;
  }
  return ClockQualityStateMachine{thresholds};
}

ClockQualityStateMachine::ClockQualityStateMachine(
    const ClockQualityThresholds thresholds) noexcept
    : thresholds_(thresholds) {}

const ClockQualitySnapshot& ClockQualityStateMachine::snapshot() const noexcept {
  return snapshot_;
}

ClockQualityMetrics ClockQualityStateMachine::metrics() const noexcept {
  return {
      .offset_ns = snapshot_.ptp_offset.value(),
      .drift_ppb = snapshot_.drift_ppb,
      .source_id_high = snapshot_.source_id.high(),
      .source_id_low = snapshot_.source_id.low(),
      .synchronization_age_ns =
          static_cast<std::uint64_t>(snapshot_.synchronization_age.value()),
      .state = snapshot_.state,
      .reason = snapshot_.reason,
      .operation_mode = snapshot_.operation_mode,
      .hardware_timestamp_available = snapshot_.hardware_timestamp_available,
      .transition_count = snapshot_.transition_count,
      .observation_count = snapshot_.observation_count,
      .source_change_count = snapshot_.source_change_count,
      .monotonicity_failure_count = snapshot_.monotonicity_failure_count,
  };
}

const ClockQualitySnapshot&
ClockQualityStateMachine::update(const ClockQualityObservation& observation) noexcept {
  increment_saturating(snapshot_.observation_count);
  record_observation(observation, DurationNs{});

  if (last_observation_time_.has_value() &&
      observation.observed_at <= *last_observation_time_) {
    increment_saturating(snapshot_.monotonicity_failure_count);
    clear_stabilization();
    transition(ClockQualityState::unsafe, ClockQualityReason::monotonic_regression,
               ClockOperationMode::blocked);
    return snapshot_;
  }
  last_observation_time_ = observation.observed_at;

  if (!observation.source_id.valid()) {
    clear_stabilization();
    transition(ClockQualityState::unsafe, ClockQualityReason::invalid_source,
               ClockOperationMode::blocked);
    return snapshot_;
  }

  const auto synchronization_age =
      checked_elapsed(observation.last_synchronization_time, observation.observed_at);
  if (!synchronization_age.ok()) {
    clear_stabilization();
    transition(ClockQualityState::unsafe,
               synchronization_age.error == TimeError::regression
                   ? ClockQualityReason::invalid_synchronization_time
                   : ClockQualityReason::arithmetic_failure,
               ClockOperationMode::blocked);
    return snapshot_;
  }
  record_observation(observation, synchronization_age.value);

  const bool changed_source =
      source_has_changed(last_source_id_, observation.source_id);
  if (changed_source) {
    increment_saturating(snapshot_.source_change_count);
  }
  last_source_id_ = observation.source_id;

  const EvaluatedQuality evaluated{
      .absolute_offset = magnitude(observation.ptp_offset),
      .absolute_drift = integer_magnitude(observation.drift_ppb),
      .synchronization_age =
          static_cast<std::uint64_t>(synchronization_age.value.value()),
  };
  if (const auto reason = unsafe_reason(observation, evaluated, thresholds_);
      reason.has_value()) {
    clear_stabilization();
    transition(ClockQualityState::unsafe, *reason, ClockOperationMode::blocked);
    return snapshot_;
  }

  if (const auto reason = degraded_reason(observation, evaluated, thresholds_);
      reason.has_value()) {
    clear_stabilization();
    transition(ClockQualityState::degraded, *reason,
               degraded_mode(thresholds_.degraded_operation));
    return snapshot_;
  }

  if (snapshot_.state == ClockQualityState::healthy && !changed_source) {
    transition(ClockQualityState::healthy, ClockQualityReason::within_thresholds,
               ClockOperationMode::normal);
    return snapshot_;
  }

  if (!stabilization_started_at_.has_value() || changed_source) {
    stabilization_started_at_ = observation.observed_at;
    transition(ClockQualityState::syncing,
               changed_source ? ClockQualityReason::source_changed
                              : ClockQualityReason::stabilizing,
               ClockOperationMode::blocked);
    return snapshot_;
  }

  const auto stabilization_age =
      checked_elapsed(*stabilization_started_at_, observation.observed_at);
  if (!stabilization_age.ok()) {
    clear_stabilization();
    transition(ClockQualityState::unsafe, ClockQualityReason::arithmetic_failure,
               ClockOperationMode::blocked);
    return snapshot_;
  }
  if (std::cmp_less(stabilization_age.value.value(),
                    thresholds_.stabilization_period_ns)) {
    transition(ClockQualityState::syncing, ClockQualityReason::stabilizing,
               ClockOperationMode::blocked);
    return snapshot_;
  }

  stabilization_started_at_.reset();
  transition(ClockQualityState::healthy, ClockQualityReason::within_thresholds,
             ClockOperationMode::normal);
  return snapshot_;
}

void ClockQualityStateMachine::transition(
    const ClockQualityState state, const ClockQualityReason reason,
    const ClockOperationMode operation_mode) noexcept {
  if (snapshot_.state != state) {
    increment_saturating(snapshot_.transition_count);
  }
  snapshot_.state = state;
  snapshot_.reason = reason;
  snapshot_.operation_mode = operation_mode;
}

void ClockQualityStateMachine::record_observation(
    const ClockQualityObservation& observation,
    const DurationNs synchronization_age) noexcept {
  snapshot_.observed_at = observation.observed_at;
  snapshot_.ptp_offset = observation.ptp_offset;
  snapshot_.drift_ppb = observation.drift_ppb;
  snapshot_.source_id = observation.source_id;
  snapshot_.last_synchronization_time = observation.last_synchronization_time;
  snapshot_.synchronization_age = synchronization_age;
  snapshot_.hardware_timestamp_available = observation.hardware_timestamp_available;
}

void ClockQualityStateMachine::clear_stabilization() noexcept {
  stabilization_started_at_.reset();
}

} // namespace aegis::time
