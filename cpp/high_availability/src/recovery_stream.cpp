#include "aegis/high_availability/recovery_stream.hpp"

#include "aegis/event_bus/queue_types.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>

namespace aegis::high_availability {
namespace {

constexpr std::uint64_t kHashOffset = 1'469'598'103'934'665'603ULL;
constexpr std::uint64_t kHashPrime = 1'099'511'628'211ULL;

void mix(std::uint64_t& hash, const std::uint64_t value) noexcept {
  hash ^= value;
  hash *= kHashPrime;
}

template <typename Identifier>
void mix_identifier(std::uint64_t& hash, const Identifier& value) noexcept {
  mix(hash, value.high());
  mix(hash, value.low());
}

[[nodiscard]] bool valid_authority(const RecoveryAuthority& value) noexcept {
  return value.session_id.valid() && value.exchange_session_epoch != 0U;
}

[[nodiscard]] bool command_record(const RecoveryRecordKind kind) noexcept {
  return kind == RecoveryRecordKind::order_emission ||
         kind == RecoveryRecordKind::gateway_acknowledgement;
}

} // namespace

bool valid_recovery_record_kind(const RecoveryRecordKind value) noexcept {
  switch (value) {
  case RecoveryRecordKind::journal_commit:
  case RecoveryRecordKind::order_emission:
  case RecoveryRecordKind::gateway_acknowledgement:
  case RecoveryRecordKind::gateway_event:
  case RecoveryRecordKind::fill:
  case RecoveryRecordKind::oms_checkpoint:
  case RecoveryRecordKind::risk_snapshot:
  case RecoveryRecordKind::reconciliation:
    return true;
  }
  return false;
}

std::uint64_t stable_recovery_payload_hash(RecoveryPayload value) noexcept {
  value.stable_hash = 0U;
  auto hash = kHashOffset;
  mix(hash, static_cast<std::uint64_t>(value.kind));
  mix_identifier(hash, value.command_id);
  mix(hash, value.command_hash);
  mix(hash, value.source_journal_sequence);
  mix(hash, value.source_journal_hash);
  mix(hash, value.process_monotonic_time_ns);
  mix_identifier(hash, value.authority.session_id);
  mix(hash, value.authority.exchange_session_epoch);
  mix(hash, value.authority.fencing_token);
  return hash == 0U ? 1U : hash;
}

std::uint64_t stable_recovery_record_hash(RecoveryRecord value) noexcept {
  value.record_hash = 0U;
  auto hash = kHashOffset;
  mix(hash, value.stream_sequence);
  mix(hash, value.previous_record_hash);
  mix(hash, value.payload.stable_hash);
  return hash == 0U ? 1U : hash;
}

std::uint64_t stable_recovery_snapshot_hash(RecoveryStreamSnapshot value) noexcept {
  value.stable_hash = 0U;
  auto hash = kHashOffset;
  mix_identifier(hash, value.authority.session_id);
  mix(hash, value.authority.exchange_session_epoch);
  mix(hash, value.authority.fencing_token);
  mix(hash, value.published_sequence);
  mix(hash, value.applied_sequence);
  mix(hash, value.published_hash);
  mix(hash, value.applied_hash);
  mix(hash, value.latest_source_journal_sequence);
  mix(hash, value.latest_source_journal_hash);
  mix(hash, value.lag_records);
  mix(hash, value.append_failures);
  mix(hash, value.duplicate_emissions);
  mix(hash, value.identity_conflicts);
  mix(hash, value.healthy ? 1U : 0U);
  mix(hash, value.caught_up ? 1U : 0U);
  return hash == 0U ? 1U : hash;
}

BoundedRecoveryStream::BoundedRecoveryStream(const RecoveryAuthority authority) noexcept
    : authority_(authority), initialized_(valid_authority(authority)),
      healthy_(initialized_) {}

bool BoundedRecoveryStream::initialized() const noexcept {
  return initialized_ && healthy_.load(std::memory_order_acquire);
}

RecoveryRotateStatus
BoundedRecoveryStream::rotate_authority(const RecoveryAuthority authority) noexcept {
  if (!initialized_ || !healthy_.load(std::memory_order_acquire)) {
    return RecoveryRotateStatus::unavailable;
  }
  if (!valid_authority(authority) || authority.session_id != authority_.session_id ||
      authority.exchange_session_epoch != authority_.exchange_session_epoch ||
      authority.fencing_token == 0U) {
    return RecoveryRotateStatus::invalid;
  }
  if (authority.fencing_token < authority_.fencing_token) {
    return RecoveryRotateStatus::stale;
  }
  if (authority.fencing_token == authority_.fencing_token) {
    return RecoveryRotateStatus::unchanged;
  }
  if (published_sequence_.load(std::memory_order_acquire) !=
          applied_sequence_.load(std::memory_order_acquire) ||
      published_hash_.load(std::memory_order_relaxed) !=
          applied_hash_.load(std::memory_order_relaxed)) {
    return RecoveryRotateStatus::not_caught_up;
  }
  authority_ = authority;
  return RecoveryRotateStatus::rotated;
}

std::size_t
BoundedRecoveryStream::command_slot(const common::GlobalEventId command_id) noexcept {
  const auto mixed = command_id.high() ^ std::rotl(command_id.low(), 29);
  return static_cast<std::size_t>(mixed & (kRecoveryCommandCapacity - 1U));
}

std::size_t
BoundedRecoveryStream::find_command(const Ledger& ledger,
                                    const common::GlobalEventId command_id) noexcept {
  const auto start = command_slot(command_id);
  for (std::size_t offset = 0U; offset < kRecoveryCommandCapacity; ++offset) {
    const auto index = (start + offset) & (kRecoveryCommandCapacity - 1U);
    if (!ledger[index].occupied) {
      return kRecoveryCommandCapacity;
    }
    if (ledger[index].command_id == command_id) {
      return index;
    }
  }
  return kRecoveryCommandCapacity;
}

std::size_t BoundedRecoveryStream::find_free_command(
    const Ledger& ledger, const common::GlobalEventId command_id) noexcept {
  const auto start = command_slot(command_id);
  for (std::size_t offset = 0U; offset < kRecoveryCommandCapacity; ++offset) {
    const auto index = (start + offset) & (kRecoveryCommandCapacity - 1U);
    if (!ledger[index].occupied || ledger[index].command_id == command_id) {
      return index;
    }
  }
  return kRecoveryCommandCapacity;
}

RecoveryAppendStatus BoundedRecoveryStream::validate_command_for_append(
    const RecoveryPayload& payload) const noexcept {
  if (!command_record(payload.kind)) {
    return RecoveryAppendStatus::accepted;
  }
  if (!payload.command_id.valid() || payload.command_hash == 0U) {
    return RecoveryAppendStatus::invalid;
  }
  const auto index = find_command(published_commands_, payload.command_id);
  if (payload.kind == RecoveryRecordKind::order_emission) {
    if (index == kRecoveryCommandCapacity) {
      return find_free_command(published_commands_, payload.command_id) ==
                     kRecoveryCommandCapacity
                 ? RecoveryAppendStatus::unavailable
                 : RecoveryAppendStatus::accepted;
    }
    return published_commands_[index].command_hash == payload.command_hash
               ? RecoveryAppendStatus::duplicate
               : RecoveryAppendStatus::identity_conflict;
  }
  if (index == kRecoveryCommandCapacity ||
      published_commands_[index].command_hash != payload.command_hash) {
    return RecoveryAppendStatus::identity_conflict;
  }
  return published_commands_[index].acknowledged ? RecoveryAppendStatus::duplicate
                                                 : RecoveryAppendStatus::accepted;
}

bool BoundedRecoveryStream::apply_to_ledger(Ledger& ledger,
                                            const RecoveryPayload& payload) noexcept {
  if (!command_record(payload.kind)) {
    return true;
  }
  auto index = find_command(ledger, payload.command_id);
  if (payload.kind == RecoveryRecordKind::order_emission) {
    if (index != kRecoveryCommandCapacity) {
      return ledger[index].command_hash == payload.command_hash;
    }
    index = find_free_command(ledger, payload.command_id);
    if (index == kRecoveryCommandCapacity) {
      return false;
    }
    ledger[index] = {.command_id = payload.command_id,
                     .command_hash = payload.command_hash,
                     .acknowledged = false,
                     .occupied = true};
    return true;
  }
  if (index == kRecoveryCommandCapacity ||
      ledger[index].command_hash != payload.command_hash) {
    return false;
  }
  ledger[index].acknowledged = true;
  return true;
}

RecoveryAppendResult BoundedRecoveryStream::append(RecoveryPayload payload) noexcept {
  if (!initialized_ || !healthy_.load(std::memory_order_acquire)) {
    return {.status = RecoveryAppendStatus::unavailable};
  }
  if (!valid_recovery_record_kind(payload.kind) ||
      payload.process_monotonic_time_ns == 0U ||
      payload.source_journal_sequence == 0U || payload.source_journal_hash == 0U ||
      payload.authority.session_id != authority_.session_id ||
      payload.authority.exchange_session_epoch != authority_.exchange_session_epoch ||
      payload.authority.fencing_token != authority_.fencing_token ||
      payload.stable_hash == 0U ||
      payload.stable_hash != stable_recovery_payload_hash(payload)) {
    append_failures_.fetch_add(1U, std::memory_order_relaxed);
    return {.status = payload.authority.fencing_token != authority_.fencing_token
                          ? RecoveryAppendStatus::fenced
                          : RecoveryAppendStatus::invalid};
  }
  const auto command_status = validate_command_for_append(payload);
  if (command_status == RecoveryAppendStatus::duplicate) {
    duplicate_emissions_.fetch_add(
        payload.kind == RecoveryRecordKind::order_emission ? 1U : 0U,
        std::memory_order_relaxed);
    return {.status = command_status};
  }
  if (command_status == RecoveryAppendStatus::identity_conflict) {
    identity_conflicts_.fetch_add(1U, std::memory_order_relaxed);
    mark_unavailable();
    return {.status = command_status};
  }
  if (command_status != RecoveryAppendStatus::accepted) {
    append_failures_.fetch_add(1U, std::memory_order_relaxed);
    mark_unavailable();
    return {.status = command_status};
  }

  RecoveryRecord record{
      .stream_sequence = published_sequence_.load(std::memory_order_relaxed) + 1U,
      .previous_record_hash = published_hash_.load(std::memory_order_relaxed),
      .payload = payload};
  record.record_hash = stable_recovery_record_hash(record);
  const auto status = queue_.enqueue(record, event_bus::OverloadPolicy::fail_closed);
  if (status != event_bus::EnqueueStatus::accepted) {
    append_failures_.fetch_add(1U, std::memory_order_relaxed);
    mark_unavailable();
    return {.status = RecoveryAppendStatus::full};
  }
  if (!apply_to_ledger(published_commands_, payload)) {
    append_failures_.fetch_add(1U, std::memory_order_relaxed);
    mark_unavailable();
    return {.status = RecoveryAppendStatus::unavailable};
  }
  latest_source_journal_sequence_.store(payload.source_journal_sequence,
                                        std::memory_order_relaxed);
  latest_source_journal_hash_.store(payload.source_journal_hash,
                                    std::memory_order_relaxed);
  published_hash_.store(record.record_hash, std::memory_order_relaxed);
  published_sequence_.store(record.stream_sequence, std::memory_order_release);
  return {.status = RecoveryAppendStatus::accepted,
          .stream_sequence = record.stream_sequence,
          .record_hash = record.record_hash};
}

RecoveryApplyStatus BoundedRecoveryStream::apply_next() noexcept {
  if (!initialized_ || !healthy_.load(std::memory_order_acquire)) {
    return RecoveryApplyStatus::unavailable;
  }
  RecoveryRecord record{};
  const auto status = queue_.dequeue(record);
  if (status == event_bus::DequeueStatus::empty) {
    return RecoveryApplyStatus::empty;
  }
  if (status != event_bus::DequeueStatus::item ||
      record.stream_sequence !=
          applied_sequence_.load(std::memory_order_relaxed) + 1U ||
      record.previous_record_hash != applied_hash_.load(std::memory_order_relaxed) ||
      record.record_hash == 0U ||
      record.record_hash != stable_recovery_record_hash(record) ||
      record.payload.stable_hash == 0U ||
      record.payload.stable_hash != stable_recovery_payload_hash(record.payload) ||
      !apply_to_ledger(applied_commands_, record.payload)) {
    mark_unavailable();
    return RecoveryApplyStatus::corrupt;
  }
  applied_hash_.store(record.record_hash, std::memory_order_relaxed);
  applied_sequence_.store(record.stream_sequence, std::memory_order_release);
  return RecoveryApplyStatus::applied;
}

bool BoundedRecoveryStream::applied_command(
    const common::GlobalEventId command_id,
    const std::uint64_t command_hash) const noexcept {
  (void)applied_sequence_.load(std::memory_order_acquire);
  const auto index = find_command(applied_commands_, command_id);
  return index != kRecoveryCommandCapacity &&
         applied_commands_[index].command_hash == command_hash;
}

RecoveryStreamSnapshot BoundedRecoveryStream::snapshot() const noexcept {
  const auto published_sequence = published_sequence_.load(std::memory_order_acquire);
  const auto applied_sequence = applied_sequence_.load(std::memory_order_acquire);
  const auto published_hash = published_hash_.load(std::memory_order_relaxed);
  const auto applied_hash = applied_hash_.load(std::memory_order_relaxed);
  const bool healthy = initialized_ && healthy_.load(std::memory_order_acquire) &&
                       applied_sequence <= published_sequence;
  RecoveryStreamSnapshot result{
      .authority = authority_,
      .published_sequence = published_sequence,
      .applied_sequence = applied_sequence,
      .published_hash = published_hash,
      .applied_hash = applied_hash,
      .latest_source_journal_sequence =
          latest_source_journal_sequence_.load(std::memory_order_relaxed),
      .latest_source_journal_hash =
          latest_source_journal_hash_.load(std::memory_order_relaxed),
      .lag_records = published_sequence >= applied_sequence
                         ? published_sequence - applied_sequence
                         : 0U,
      .append_failures = append_failures_.load(std::memory_order_relaxed),
      .duplicate_emissions = duplicate_emissions_.load(std::memory_order_relaxed),
      .identity_conflicts = identity_conflicts_.load(std::memory_order_relaxed),
      .healthy = healthy,
      .caught_up = healthy && published_sequence == applied_sequence &&
                   published_hash == applied_hash};
  result.stable_hash = stable_recovery_snapshot_hash(result);
  return result;
}

void BoundedRecoveryStream::invalidate() noexcept { mark_unavailable(); }

void BoundedRecoveryStream::mark_unavailable() noexcept {
  healthy_.store(false, std::memory_order_release);
  queue_.invalidate();
}

} // namespace aegis::high_availability
