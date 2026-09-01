#ifndef AEGIS_TIME_CLOCK_QUALITY_HPP
#define AEGIS_TIME_CLOCK_QUALITY_HPP

#include "aegis/common/identifiers.hpp"
#include "aegis/time/time_types.hpp"

#include <cstdint>
#include <limits>
#include <optional>

namespace aegis::time {

struct ClockSourceIdTag;
using ClockSourceId = aegis::common::Identifier128<ClockSourceIdTag>;

enum class ClockQualityState : std::uint8_t {
  unknown = 0,
  syncing,
  healthy,
  degraded,
  unsafe,
};

enum class ClockQualityReason : std::uint8_t {
  awaiting_observation = 0,
  stabilizing,
  within_thresholds,
  offset_degraded,
  offset_unsafe,
  drift_degraded,
  drift_unsafe,
  sync_aging,
  sync_stale,
  hardware_timestamp_unavailable,
  source_changed,
  monotonic_regression,
  invalid_source,
  invalid_synchronization_time,
  arithmetic_failure,
};

enum class ClockOperationMode : std::uint8_t {
  blocked = 0,
  reduce_only,
  normal,
};

enum class DegradedOperationMode : std::uint8_t {
  blocked = 0,
  reduce_only,
};

// These types are fixed-layout value contracts. Public members intentionally
// support named aggregate initialization and allocation-free snapshots.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct ClockQualityThresholds {
  std::uint64_t healthy_max_abs_offset_ns{};
  std::uint64_t unsafe_min_abs_offset_ns{};
  std::uint64_t healthy_max_abs_drift_ppb{};
  std::uint64_t unsafe_min_abs_drift_ppb{};
  std::uint64_t healthy_max_sync_age_ns{};
  std::uint64_t unsafe_min_sync_age_ns{};
  std::uint64_t stabilization_period_ns{};
  bool require_hardware_timestamps{true};
  DegradedOperationMode degraded_operation{DegradedOperationMode::blocked};

  [[nodiscard]] constexpr bool valid() const noexcept {
    constexpr auto maximum_signed_duration =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    constexpr auto maximum_signed_magnitude = maximum_signed_duration + 1U;
    const bool valid_degraded_mode =
        degraded_operation == DegradedOperationMode::blocked ||
        degraded_operation == DegradedOperationMode::reduce_only;
    return healthy_max_abs_offset_ns < unsafe_min_abs_offset_ns &&
           healthy_max_abs_drift_ppb < unsafe_min_abs_drift_ppb &&
           healthy_max_sync_age_ns < unsafe_min_sync_age_ns &&
           unsafe_min_abs_offset_ns <= maximum_signed_magnitude &&
           unsafe_min_abs_drift_ppb <= maximum_signed_magnitude &&
           unsafe_min_sync_age_ns <= maximum_signed_duration &&
           stabilization_period_ns > 0U &&
           stabilization_period_ns <= maximum_signed_duration && valid_degraded_mode;
  }
};

struct ClockQualityObservation {
  MonotonicTimeNs observed_at;
  DurationNs ptp_offset;
  std::int64_t drift_ppb{};
  ClockSourceId source_id;
  MonotonicTimeNs last_synchronization_time;
  bool hardware_timestamp_available{false};
};

struct ClockQualitySnapshot {
  ClockQualityState state{ClockQualityState::unknown};
  ClockQualityReason reason{ClockQualityReason::awaiting_observation};
  ClockOperationMode operation_mode{ClockOperationMode::blocked};
  MonotonicTimeNs observed_at;
  DurationNs ptp_offset;
  std::int64_t drift_ppb{};
  ClockSourceId source_id;
  MonotonicTimeNs last_synchronization_time;
  DurationNs synchronization_age;
  bool hardware_timestamp_available{false};
  std::uint64_t transition_count{};
  std::uint64_t observation_count{};
  std::uint64_t source_change_count{};
  std::uint64_t monotonicity_failure_count{};
};

// Fixed-width, allocation-free metric values. Exporters may translate these to
// Prometheus/OpenTelemetry outside the critical path.
struct ClockQualityMetrics {
  std::int64_t offset_ns{};
  std::int64_t drift_ppb{};
  std::uint64_t source_id_high{};
  std::uint64_t source_id_low{};
  std::uint64_t synchronization_age_ns{};
  ClockQualityState state{ClockQualityState::unknown};
  ClockQualityReason reason{ClockQualityReason::awaiting_observation};
  ClockOperationMode operation_mode{ClockOperationMode::blocked};
  bool hardware_timestamp_available{false};
  std::uint64_t transition_count{};
  std::uint64_t observation_count{};
  std::uint64_t source_change_count{};
  std::uint64_t monotonicity_failure_count{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

class ClockQualityStateMachine {
public:
  [[nodiscard]] static std::optional<ClockQualityStateMachine>
  create(ClockQualityThresholds thresholds) noexcept;

  [[nodiscard]] const ClockQualitySnapshot& snapshot() const noexcept;
  [[nodiscard]] ClockQualityMetrics metrics() const noexcept;
  [[nodiscard]] const ClockQualitySnapshot&
  update(const ClockQualityObservation& observation) noexcept;

private:
  explicit ClockQualityStateMachine(ClockQualityThresholds thresholds) noexcept;

  void transition(ClockQualityState state, ClockQualityReason reason,
                  ClockOperationMode operation_mode) noexcept;
  void record_observation(const ClockQualityObservation& observation,
                          DurationNs synchronization_age) noexcept;
  void clear_stabilization() noexcept;

  ClockQualityThresholds thresholds_;
  ClockQualitySnapshot snapshot_;
  std::optional<MonotonicTimeNs> last_observation_time_;
  std::optional<MonotonicTimeNs> stabilization_started_at_;
  std::optional<ClockSourceId> last_source_id_;
};

static_assert(sizeof(ClockSourceId) == 16);

} // namespace aegis::time

#endif // AEGIS_TIME_CLOCK_QUALITY_HPP
