#ifndef AEGIS_HIGH_AVAILABILITY_RECOVERY_STREAM_HPP
#define AEGIS_HIGH_AVAILABILITY_RECOVERY_STREAM_HPP

#include "aegis/common/identifiers.hpp"
#include "aegis/event_bus/spsc_ring.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace aegis::high_availability {

inline constexpr std::size_t kRecoveryStreamCapacity = 1'024U;
inline constexpr std::size_t kRecoveryCommandCapacity = 8'192U;

enum class RecoveryRecordKind : std::uint8_t {
  journal_commit = 1,
  order_emission = 2,
  gateway_acknowledgement = 3,
  gateway_event = 4,
  fill = 5,
  oms_checkpoint = 6,
  risk_snapshot = 7,
  reconciliation = 8,
};

enum class RecoveryAppendStatus : std::uint8_t {
  accepted = 1,
  duplicate = 2,
  identity_conflict = 3,
  invalid = 4,
  fenced = 5,
  full = 6,
  unavailable = 7,
};

enum class RecoveryApplyStatus : std::uint8_t {
  applied = 1,
  empty = 2,
  corrupt = 3,
  unavailable = 4,
};

enum class RecoveryRotateStatus : std::uint8_t {
  rotated = 1,
  unchanged = 2,
  stale = 3,
  not_caught_up = 4,
  invalid = 5,
  unavailable = 6,
};

// Fixed-layout recovery records are repository-owned and intentionally contain
// no exchange-native bytes. The transport may copy them to a hot standby.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct RecoveryAuthority {
  common::SessionId session_id;
  std::uint64_t exchange_session_epoch{};
  std::uint64_t fencing_token{};
};

struct RecoveryPayload {
  RecoveryRecordKind kind{RecoveryRecordKind::journal_commit};
  common::GlobalEventId command_id;
  std::uint64_t command_hash{};
  std::uint64_t source_journal_sequence{};
  std::uint64_t source_journal_hash{};
  std::uint64_t process_monotonic_time_ns{};
  RecoveryAuthority authority;
  std::uint64_t stable_hash{};
};

struct RecoveryRecord {
  std::uint64_t stream_sequence{};
  std::uint64_t previous_record_hash{};
  RecoveryPayload payload;
  std::uint64_t record_hash{};
};

struct RecoveryAppendResult {
  RecoveryAppendStatus status{RecoveryAppendStatus::invalid};
  std::uint64_t stream_sequence{};
  std::uint64_t record_hash{};
};

struct RecoveryStreamSnapshot {
  RecoveryAuthority authority;
  std::uint64_t published_sequence{};
  std::uint64_t applied_sequence{};
  std::uint64_t published_hash{};
  std::uint64_t applied_hash{};
  std::uint64_t latest_source_journal_sequence{};
  std::uint64_t latest_source_journal_hash{};
  std::uint64_t lag_records{};
  std::uint64_t append_failures{};
  std::uint64_t duplicate_emissions{};
  std::uint64_t identity_conflicts{};
  bool healthy{false};
  bool caught_up{false};
  std::uint64_t stable_hash{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] bool valid_recovery_record_kind(RecoveryRecordKind value) noexcept;
[[nodiscard]] std::uint64_t
stable_recovery_payload_hash(RecoveryPayload value) noexcept;
[[nodiscard]] std::uint64_t stable_recovery_record_hash(RecoveryRecord value) noexcept;
[[nodiscard]] std::uint64_t
stable_recovery_snapshot_hash(RecoveryStreamSnapshot value) noexcept;

class BoundedRecoveryStream final {
public:
  explicit BoundedRecoveryStream(RecoveryAuthority authority) noexcept;

  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] RecoveryRotateStatus
  rotate_authority(RecoveryAuthority authority) noexcept;
  [[nodiscard]] RecoveryAppendResult append(RecoveryPayload payload) noexcept;
  [[nodiscard]] RecoveryApplyStatus apply_next() noexcept;
  [[nodiscard]] bool applied_command(common::GlobalEventId command_id,
                                     std::uint64_t command_hash) const noexcept;
  [[nodiscard]] RecoveryStreamSnapshot snapshot() const noexcept;
  void invalidate() noexcept;

private:
  struct CommandSlot {
    common::GlobalEventId command_id;
    std::uint64_t command_hash{};
    bool acknowledged{false};
    bool occupied{false};
  };

  using Ledger = std::array<CommandSlot, kRecoveryCommandCapacity>;

  [[nodiscard]] static std::size_t
  command_slot(common::GlobalEventId command_id) noexcept;
  [[nodiscard]] static std::size_t
  find_command(const Ledger& ledger, common::GlobalEventId command_id) noexcept;
  [[nodiscard]] static std::size_t
  find_free_command(const Ledger& ledger, common::GlobalEventId command_id) noexcept;
  [[nodiscard]] RecoveryAppendStatus
  validate_command_for_append(const RecoveryPayload& payload) const noexcept;
  [[nodiscard]] static bool apply_to_ledger(Ledger& ledger,
                                            const RecoveryPayload& payload) noexcept;
  void mark_unavailable() noexcept;

  RecoveryAuthority authority_;
  event_bus::SpscRing<RecoveryRecord, kRecoveryStreamCapacity> queue_;
  Ledger published_commands_{};
  Ledger applied_commands_{};
  std::atomic<std::uint64_t> published_sequence_{0U};
  std::atomic<std::uint64_t> applied_sequence_{0U};
  std::atomic<std::uint64_t> published_hash_{0U};
  std::atomic<std::uint64_t> applied_hash_{0U};
  std::atomic<std::uint64_t> latest_source_journal_sequence_{0U};
  std::atomic<std::uint64_t> latest_source_journal_hash_{0U};
  std::atomic<std::uint64_t> append_failures_{0U};
  std::atomic<std::uint64_t> duplicate_emissions_{0U};
  std::atomic<std::uint64_t> identity_conflicts_{0U};
  bool initialized_{false};
  std::atomic<bool> healthy_{false};
};

static_assert(std::is_trivially_copyable_v<RecoveryRecord>);
static_assert(std::is_trivially_copyable_v<RecoveryStreamSnapshot>);

} // namespace aegis::high_availability

#endif // AEGIS_HIGH_AVAILABILITY_RECOVERY_STREAM_HPP
