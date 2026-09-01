#ifndef AEGIS_EVENT_BUS_PROCESS_EPOCH_HPP
#define AEGIS_EVENT_BUS_PROCESS_EPOCH_HPP

#include "aegis/event_bus/cache_line.hpp"

#include <atomic>
#include <cstdint>

namespace aegis::event_bus {

enum class EpochClaimStatus : std::uint8_t {
  acquired = 1,
  renewed = 2,
  stale_takeover = 3,
  split_brain = 4,
  stale_epoch = 5,
  corrupt = 6,
  invalid = 7,
};

enum class EpochHeartbeatStatus : std::uint8_t {
  recorded = 1,
  lost_authority = 2,
  time_regression = 3,
  corrupt = 4,
  invalid = 5,
};

enum class EpochHealth : std::uint8_t {
  empty = 1,
  live = 2,
  stale = 3,
  corrupt = 4,
};

// This state is suitable for placement in process-shared memory. All members
// are lock-free 64-bit atomics; heartbeat is the release/acquire publication
// marker for process_id, epoch, and guard.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct alignas(kCacheLineBytes) ProcessEpochState {
  std::atomic<std::uint64_t> process_id{0U};
  std::atomic<std::uint64_t> epoch{0U};
  std::atomic<std::uint64_t> heartbeat_monotonic_time_ns{0U};
  std::atomic<std::uint64_t> identity_guard{0U};
};

struct ProcessEpochSnapshot {
  std::uint64_t process_id{};
  std::uint64_t epoch{};
  std::uint64_t heartbeat_monotonic_time_ns{};
  EpochHealth health{EpochHealth::empty};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

class ProcessEpochMechanism final {
public:
  [[nodiscard]] static EpochClaimStatus
  claim(ProcessEpochState& state, std::uint64_t process_id,
        std::uint64_t candidate_epoch, std::uint64_t now_monotonic_time_ns,
        std::uint64_t heartbeat_timeout_ns) noexcept;

  [[nodiscard]] static EpochHeartbeatStatus
  heartbeat(ProcessEpochState& state, std::uint64_t process_id, std::uint64_t epoch,
            std::uint64_t now_monotonic_time_ns) noexcept;

  [[nodiscard]] static EpochHeartbeatStatus mark_stopped(ProcessEpochState& state,
                                                         std::uint64_t process_id,
                                                         std::uint64_t epoch) noexcept;

  [[nodiscard]] static ProcessEpochSnapshot
  inspect(const ProcessEpochState& state, std::uint64_t now_monotonic_time_ns,
          std::uint64_t heartbeat_timeout_ns) noexcept;

  static void reset_quiescent(ProcessEpochState& state) noexcept;

private:
  [[nodiscard]] static std::uint64_t guard(std::uint64_t process_id,
                                           std::uint64_t epoch) noexcept;
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(alignof(ProcessEpochState) == kCacheLineBytes);
static_assert(sizeof(ProcessEpochState) == kCacheLineBytes);

} // namespace aegis::event_bus

#endif // AEGIS_EVENT_BUS_PROCESS_EPOCH_HPP
