#include "aegis/time/clock_quality.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>

namespace aegis::time {
namespace {

constexpr ClockSourceId kSourceOne{1U, 1U};
constexpr ClockSourceId kSourceTwo{2U, 2U};

[[nodiscard]] constexpr ClockQualityThresholds thresholds() noexcept {
  return {
      .healthy_max_abs_offset_ns = 100U,
      .unsafe_min_abs_offset_ns = 300U,
      .healthy_max_abs_drift_ppb = 10U,
      .unsafe_min_abs_drift_ppb = 30U,
      .healthy_max_sync_age_ns = 50U,
      .unsafe_min_sync_age_ns = 100U,
      .stabilization_period_ns = 20U,
      .require_hardware_timestamps = true,
      .degraded_operation = DegradedOperationMode::reduce_only,
  };
}

[[nodiscard]] constexpr ClockQualityObservation
observation(const std::uint64_t observed_at, const std::uint64_t synchronized_at,
            const std::int64_t offset = 5, const std::int64_t drift = 2,
            const ClockSourceId source = kSourceOne,
            const bool hardware_available = true) noexcept {
  return {
      .observed_at = MonotonicTimeNs{observed_at},
      .ptp_offset = DurationNs{offset},
      .drift_ppb = drift,
      .source_id = source,
      .last_synchronization_time = MonotonicTimeNs{synchronized_at},
      .hardware_timestamp_available = hardware_available,
  };
}

[[nodiscard]] ClockQualityStateMachine
machine(const ClockQualityThresholds config = thresholds()) {
  auto result = ClockQualityStateMachine::create(config);
  if (!result.has_value()) {
    ADD_FAILURE() << "test threshold configuration was rejected";
    std::abort();
  }
  return std::move(result).value();
}

TEST(ClockQualityStateMachineTest, RejectsUnorderedOrZeroStabilizationConfig) {
  auto invalid = thresholds();
  invalid.healthy_max_abs_offset_ns = invalid.unsafe_min_abs_offset_ns;
  EXPECT_FALSE(ClockQualityStateMachine::create(invalid).has_value());

  invalid = thresholds();
  invalid.stabilization_period_ns = 0U;
  EXPECT_FALSE(ClockQualityStateMachine::create(invalid).has_value());

  invalid = thresholds();
  invalid.unsafe_min_sync_age_ns = std::uint64_t{1} << 63U;
  EXPECT_FALSE(ClockQualityStateMachine::create(invalid).has_value());

  invalid = thresholds();
  // Deliberately exercise validation at an untrusted configuration boundary.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  invalid.degraded_operation = static_cast<DegradedOperationMode>(255U);
  EXPECT_FALSE(ClockQualityStateMachine::create(invalid).has_value());
}

TEST(ClockQualityStateMachineTest, StartsUnknownAndBlocksOperation) {
  const auto state_machine = machine();
  EXPECT_EQ(state_machine.snapshot().state, ClockQualityState::unknown);
  EXPECT_EQ(state_machine.snapshot().operation_mode, ClockOperationMode::blocked);
}

TEST(ClockQualityStateMachineTest, RequiresFullStabilizationBeforeHealthy) {
  auto state_machine = machine();
  EXPECT_EQ(state_machine.update(observation(100U, 90U)).state,
            ClockQualityState::syncing);
  EXPECT_EQ(state_machine.update(observation(119U, 110U)).state,
            ClockQualityState::syncing);
  const auto& recovered = state_machine.update(observation(120U, 111U));
  EXPECT_EQ(recovered.state, ClockQualityState::healthy);
  EXPECT_EQ(recovered.operation_mode, ClockOperationMode::normal);
  EXPECT_EQ(recovered.transition_count, 2U);
}

TEST(ClockQualityStateMachineTest, DegradedBandUsesConfiguredRestriction) {
  auto state_machine = machine();
  const auto& result = state_machine.update(observation(100U, 90U, 101));
  EXPECT_EQ(result.state, ClockQualityState::degraded);
  EXPECT_EQ(result.reason, ClockQualityReason::offset_degraded);
  EXPECT_EQ(result.operation_mode, ClockOperationMode::reduce_only);
}

TEST(ClockQualityStateMachineTest, UnsafeThresholdBlocksNewOrders) {
  auto state_machine = machine();
  const auto& result = state_machine.update(observation(100U, 90U, 300));
  EXPECT_EQ(result.state, ClockQualityState::unsafe);
  EXPECT_EQ(result.reason, ClockQualityReason::offset_unsafe);
  EXPECT_EQ(result.operation_mode, ClockOperationMode::blocked);
}

TEST(ClockQualityStateMachineTest, RecoveryAfterUnsafeRestartsStabilization) {
  auto state_machine = machine();
  static_cast<void>(state_machine.update(observation(100U, 90U, 300)));
  EXPECT_EQ(state_machine.update(observation(110U, 105U)).state,
            ClockQualityState::syncing);
  EXPECT_EQ(state_machine.update(observation(129U, 124U)).state,
            ClockQualityState::syncing);
  EXPECT_EQ(state_machine.update(observation(130U, 125U)).state,
            ClockQualityState::healthy);
}

TEST(ClockQualityStateMachineTest, DriftAndStaleSyncHaveDegradedAndUnsafeBands) {
  auto drift_machine = machine();
  EXPECT_EQ(drift_machine.update(observation(100U, 90U, 0, 11)).reason,
            ClockQualityReason::drift_degraded);
  EXPECT_EQ(drift_machine.update(observation(110U, 100U, 0, 30)).reason,
            ClockQualityReason::drift_unsafe);

  auto age_machine = machine();
  EXPECT_EQ(age_machine.update(observation(100U, 49U)).reason,
            ClockQualityReason::sync_aging);
  EXPECT_EQ(age_machine.update(observation(110U, 10U)).reason,
            ClockQualityReason::sync_stale);
}

TEST(ClockQualityStateMachineTest, HandlesMinimumSignedDriftWithoutOverflow) {
  auto state_machine = machine();
  const auto& result = state_machine.update(
      observation(100U, 90U, 0, std::numeric_limits<std::int64_t>::min()));
  EXPECT_EQ(result.state, ClockQualityState::unsafe);
  EXPECT_EQ(result.reason, ClockQualityReason::drift_unsafe);
}

TEST(ClockQualityStateMachineTest, RequiredHardwareTimestampFailureIsUnsafe) {
  auto state_machine = machine();
  const auto& result =
      state_machine.update(observation(100U, 90U, 0, 0, kSourceOne, false));
  EXPECT_EQ(result.state, ClockQualityState::unsafe);
  EXPECT_EQ(result.reason, ClockQualityReason::hardware_timestamp_unavailable);
}

TEST(ClockQualityStateMachineTest, OptionalHardwareTimestampFailureIsDegraded) {
  auto config = thresholds();
  config.require_hardware_timestamps = false;
  auto state_machine = machine(config);
  const auto& result =
      state_machine.update(observation(100U, 90U, 0, 0, kSourceOne, false));
  EXPECT_EQ(result.state, ClockQualityState::degraded);
  EXPECT_EQ(result.operation_mode, ClockOperationMode::reduce_only);
}

TEST(ClockQualityStateMachineTest, SourceChangeForcesRestabilization) {
  auto state_machine = machine();
  static_cast<void>(state_machine.update(observation(100U, 90U)));
  ASSERT_EQ(state_machine.update(observation(120U, 110U)).state,
            ClockQualityState::healthy);

  const auto& changed = state_machine.update(observation(121U, 115U, 0, 0, kSourceTwo));
  EXPECT_EQ(changed.state, ClockQualityState::syncing);
  EXPECT_EQ(changed.reason, ClockQualityReason::source_changed);
  EXPECT_EQ(changed.source_change_count, 1U);
}

TEST(ClockQualityStateMachineTest, MonotonicRegressionFailsClosedAndIsCounted) {
  auto state_machine = machine();
  static_cast<void>(state_machine.update(observation(100U, 90U)));
  const auto& result = state_machine.update(observation(99U, 90U));
  EXPECT_EQ(result.state, ClockQualityState::unsafe);
  EXPECT_EQ(result.reason, ClockQualityReason::monotonic_regression);
  EXPECT_EQ(result.monotonicity_failure_count, 1U);
  EXPECT_EQ(result.operation_mode, ClockOperationMode::blocked);
}

TEST(ClockQualityStateMachineTest, FutureSynchronizationTimeFailsClosed) {
  auto state_machine = machine();
  const auto& result = state_machine.update(observation(100U, 101U));
  EXPECT_EQ(result.state, ClockQualityState::unsafe);
  EXPECT_EQ(result.reason, ClockQualityReason::invalid_synchronization_time);
}

TEST(ClockQualityStateMachineTest, InvalidSourceFailsClosed) {
  auto state_machine = machine();
  const auto& result =
      state_machine.update(observation(100U, 90U, 0, 0, ClockSourceId{}));
  EXPECT_EQ(result.state, ClockQualityState::unsafe);
  EXPECT_EQ(result.reason, ClockQualityReason::invalid_source);
}

TEST(ClockQualityStateMachineTest, MetricsExposeBoundedNumericState) {
  auto state_machine = machine();
  static_cast<void>(state_machine.update(observation(100U, 90U, -7, -3)));
  const auto metrics = state_machine.metrics();
  EXPECT_EQ(metrics.offset_ns, -7);
  EXPECT_EQ(metrics.drift_ppb, -3);
  EXPECT_EQ(metrics.source_id_high, kSourceOne.high());
  EXPECT_EQ(metrics.source_id_low, kSourceOne.low());
  EXPECT_EQ(metrics.synchronization_age_ns, 10U);
  EXPECT_TRUE(metrics.hardware_timestamp_available);
  EXPECT_EQ(metrics.state, ClockQualityState::syncing);
  EXPECT_EQ(metrics.observation_count, 1U);
}

} // namespace
} // namespace aegis::time
