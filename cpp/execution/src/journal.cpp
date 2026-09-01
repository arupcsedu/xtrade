#include "aegis/execution/journal.hpp"

namespace aegis::execution {

GatewayAuditJournal::GatewayAuditJournal() noexcept {
  for (auto& slot : slots_) {
    slot.published_sequence.store(0U, std::memory_order_relaxed);
  }
}

GatewayAuditJournal::Reservation GatewayAuditJournal::try_reserve() noexcept {
  const auto current = reserved_sequence_.load(std::memory_order_relaxed);
  if (current >= slots_.size()) {
    return {};
  }
  reserved_sequence_.store(current + 1U, std::memory_order_relaxed);
  return {.sequence = current + 1U, .valid = true};
}

bool GatewayAuditJournal::commit(const Reservation reservation,
                                 GatewayAuditRecord record) noexcept {
  if (!reservation.valid || reservation.sequence == 0U ||
      reservation.sequence > slots_.size() || record.sequence != 0U) {
    return false;
  }
  const auto expected = published_sequence_.load(std::memory_order_relaxed) + 1U;
  if (reservation.sequence != expected) {
    return false;
  }
  record.sequence = reservation.sequence;
  record.previous_record_hash = last_record_hash_.load(std::memory_order_relaxed);
  record.stable_hash = stable_gateway_audit_record_hash(record);
  auto& slot = slots_[static_cast<std::size_t>(reservation.sequence - 1U)];
  slot.record = record;
  slot.published_sequence.store(reservation.sequence, std::memory_order_release);
  last_record_hash_.store(record.stable_hash, std::memory_order_release);
  published_sequence_.store(reservation.sequence, std::memory_order_release);
  return true;
}

bool GatewayAuditJournal::read(const std::uint64_t sequence,
                               GatewayAuditRecord& output) const noexcept {
  if (sequence == 0U || sequence > slots_.size() ||
      sequence > published_sequence_.load(std::memory_order_acquire)) {
    return false;
  }
  const auto& slot = slots_[static_cast<std::size_t>(sequence - 1U)];
  if (slot.published_sequence.load(std::memory_order_acquire) != sequence) {
    return false;
  }
  output = slot.record;
  return output.sequence == sequence && output.stable_hash != 0U &&
         output.stable_hash == stable_gateway_audit_record_hash(output);
}

std::uint64_t GatewayAuditJournal::size() const noexcept {
  return published_sequence_.load(std::memory_order_acquire);
}

std::uint64_t GatewayAuditJournal::last_record_hash() const noexcept {
  return last_record_hash_.load(std::memory_order_acquire);
}

bool GatewayAuditJournal::verify_chain() const noexcept {
  std::uint64_t previous{};
  GatewayAuditRecord record{};
  for (std::uint64_t sequence = 1U; sequence <= size(); ++sequence) {
    if (!read(sequence, record) || record.previous_record_hash != previous) {
      return false;
    }
    previous = record.stable_hash;
  }
  return previous == last_record_hash();
}

} // namespace aegis::execution
