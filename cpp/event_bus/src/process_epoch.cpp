#include "aegis/event_bus/process_epoch.hpp"

#include <atomic>
#include <bit>
#include <cstdint>

namespace aegis::event_bus {
namespace {

[[nodiscard]] bool stale(const std::uint64_t heartbeat, const std::uint64_t now,
                         const std::uint64_t timeout) noexcept {
  return heartbeat == 0U || now < heartbeat || now - heartbeat > timeout;
}

} // namespace

std::uint64_t ProcessEpochMechanism::guard(const std::uint64_t process_id,
                                           const std::uint64_t epoch) noexcept {
  auto value = process_id ^ std::rotl(epoch, 23) ^ 0xA3E6'19D4'70B2'5C8FULL;
  value ^= value >> 30U;
  value *= 0xBF58'476D'1CE4'E5B9ULL;
  value ^= value >> 27U;
  value *= 0x94D0'49BB'1331'11EBULL;
  value ^= value >> 31U;
  return value == 0U ? 1U : value;
}

EpochClaimStatus
ProcessEpochMechanism::claim(ProcessEpochState& state, const std::uint64_t process_id,
                             const std::uint64_t candidate_epoch,
                             const std::uint64_t now_monotonic_time_ns,
                             const std::uint64_t heartbeat_timeout_ns) noexcept {
  if (process_id == 0U || candidate_epoch == 0U || now_monotonic_time_ns == 0U ||
      heartbeat_timeout_ns == 0U) {
    return EpochClaimStatus::invalid;
  }
  auto current_epoch = state.epoch.load(std::memory_order_acquire);
  if (current_epoch == 0U) {
    if (!state.epoch.compare_exchange_strong(current_epoch, candidate_epoch,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
      return EpochClaimStatus::split_brain;
    }
    state.process_id.store(process_id, std::memory_order_relaxed);
    state.identity_guard.store(guard(process_id, candidate_epoch),
                               std::memory_order_relaxed);
    state.heartbeat_monotonic_time_ns.store(now_monotonic_time_ns,
                                            std::memory_order_release);
    return EpochClaimStatus::acquired;
  }

  const auto heartbeat =
      state.heartbeat_monotonic_time_ns.load(std::memory_order_acquire);
  const auto current_process = state.process_id.load(std::memory_order_relaxed);
  const auto current_guard = state.identity_guard.load(std::memory_order_relaxed);
  if (current_process == 0U || current_guard != guard(current_process, current_epoch)) {
    return EpochClaimStatus::corrupt;
  }
  if (current_epoch == candidate_epoch) {
    if (current_process != process_id) {
      return EpochClaimStatus::split_brain;
    }
    const auto status = ProcessEpochMechanism::heartbeat(
        state, process_id, candidate_epoch, now_monotonic_time_ns);
    return status == EpochHeartbeatStatus::recorded ? EpochClaimStatus::renewed
                                                    : EpochClaimStatus::corrupt;
  }
  if (!stale(heartbeat, now_monotonic_time_ns, heartbeat_timeout_ns)) {
    return EpochClaimStatus::split_brain;
  }
  if (candidate_epoch <= current_epoch) {
    return EpochClaimStatus::stale_epoch;
  }
  if (!state.epoch.compare_exchange_strong(current_epoch, candidate_epoch,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
    return EpochClaimStatus::split_brain;
  }
  state.process_id.store(process_id, std::memory_order_relaxed);
  state.identity_guard.store(guard(process_id, candidate_epoch),
                             std::memory_order_relaxed);
  state.heartbeat_monotonic_time_ns.store(now_monotonic_time_ns,
                                          std::memory_order_release);
  return EpochClaimStatus::stale_takeover;
}

EpochHeartbeatStatus ProcessEpochMechanism::heartbeat(
    ProcessEpochState& state, const std::uint64_t process_id, const std::uint64_t epoch,
    const std::uint64_t now_monotonic_time_ns) noexcept {
  if (process_id == 0U || epoch == 0U || now_monotonic_time_ns == 0U) {
    return EpochHeartbeatStatus::invalid;
  }
  const auto observed_epoch = state.epoch.load(std::memory_order_acquire);
  const auto observed_process = state.process_id.load(std::memory_order_relaxed);
  if (observed_epoch != epoch || observed_process != process_id) {
    return EpochHeartbeatStatus::lost_authority;
  }
  if (state.identity_guard.load(std::memory_order_relaxed) !=
      guard(process_id, epoch)) {
    return EpochHeartbeatStatus::corrupt;
  }
  const auto previous =
      state.heartbeat_monotonic_time_ns.load(std::memory_order_relaxed);
  if (now_monotonic_time_ns < previous) {
    return EpochHeartbeatStatus::time_regression;
  }
  auto expected_heartbeat = previous;
  // CAS prevents a fenced process from overwriting the heartbeat published by
  // a newer epoch after a stale-owner takeover.
  if (!state.heartbeat_monotonic_time_ns.compare_exchange_strong(
          expected_heartbeat, now_monotonic_time_ns, std::memory_order_release,
          std::memory_order_relaxed)) {
    return EpochHeartbeatStatus::lost_authority;
  }
  return state.epoch.load(std::memory_order_acquire) == epoch &&
                 state.process_id.load(std::memory_order_relaxed) == process_id
             ? EpochHeartbeatStatus::recorded
             : EpochHeartbeatStatus::lost_authority;
}

EpochHeartbeatStatus
ProcessEpochMechanism::mark_stopped(ProcessEpochState& state,
                                    const std::uint64_t process_id,
                                    const std::uint64_t epoch) noexcept {
  if (state.epoch.load(std::memory_order_acquire) != epoch ||
      state.process_id.load(std::memory_order_relaxed) != process_id) {
    return EpochHeartbeatStatus::lost_authority;
  }
  if (state.identity_guard.load(std::memory_order_relaxed) !=
      guard(process_id, epoch)) {
    return EpochHeartbeatStatus::corrupt;
  }
  auto heartbeat = state.heartbeat_monotonic_time_ns.load(std::memory_order_relaxed);
  if (!state.heartbeat_monotonic_time_ns.compare_exchange_strong(
          heartbeat, 0U, std::memory_order_release, std::memory_order_relaxed)) {
    return EpochHeartbeatStatus::lost_authority;
  }
  return state.epoch.load(std::memory_order_acquire) == epoch &&
                 state.process_id.load(std::memory_order_relaxed) == process_id
             ? EpochHeartbeatStatus::recorded
             : EpochHeartbeatStatus::lost_authority;
}

ProcessEpochSnapshot
ProcessEpochMechanism::inspect(const ProcessEpochState& state,
                               const std::uint64_t now_monotonic_time_ns,
                               const std::uint64_t heartbeat_timeout_ns) noexcept {
  const auto heartbeat =
      state.heartbeat_monotonic_time_ns.load(std::memory_order_acquire);
  const auto epoch = state.epoch.load(std::memory_order_relaxed);
  const auto process_id = state.process_id.load(std::memory_order_relaxed);
  const auto identity_guard = state.identity_guard.load(std::memory_order_relaxed);
  if (epoch == 0U && process_id == 0U && heartbeat == 0U && identity_guard == 0U) {
    return {.health = EpochHealth::empty};
  }
  if (epoch == 0U || process_id == 0U || identity_guard != guard(process_id, epoch) ||
      now_monotonic_time_ns == 0U || heartbeat_timeout_ns == 0U) {
    return {.process_id = process_id,
            .epoch = epoch,
            .heartbeat_monotonic_time_ns = heartbeat,
            .health = EpochHealth::corrupt};
  }
  return {.process_id = process_id,
          .epoch = epoch,
          .heartbeat_monotonic_time_ns = heartbeat,
          .health = stale(heartbeat, now_monotonic_time_ns, heartbeat_timeout_ns)
                        ? EpochHealth::stale
                        : EpochHealth::live};
}

void ProcessEpochMechanism::reset_quiescent(ProcessEpochState& state) noexcept {
  state.heartbeat_monotonic_time_ns.store(0U, std::memory_order_relaxed);
  state.identity_guard.store(0U, std::memory_order_relaxed);
  state.process_id.store(0U, std::memory_order_relaxed);
  state.epoch.store(0U, std::memory_order_release);
}

} // namespace aegis::event_bus
