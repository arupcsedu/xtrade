#include "aegis/risk/portfolio_journal.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace aegis::risk::portfolio {
namespace {

constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;

void mix(std::uint64_t& hash, const std::uint64_t value) noexcept {
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    hash ^= (value >> shift) & 0xFFU;
    hash *= kFnvPrime;
  }
}

} // namespace

std::uint64_t stable_journal_record_hash(PortfolioJournalRecord value) noexcept {
  value.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix(hash, value.sequence);
  mix(hash, value.event.stable_hash);
  mix(hash, static_cast<std::uint8_t>(value.result.status));
  mix(hash, static_cast<std::uint8_t>(value.result.invariant));
  mix(hash, value.result.journal_sequence);
  mix(hash, value.result.snapshot_sequence);
  mix(hash, value.result.snapshot_hash);
  mix(hash, value.previous_record_hash);
  const auto result = hash == 0U ? 1U : hash;
  return result;
}

PortfolioJournal::PortfolioJournal() noexcept {
  for (auto& slot : slots_) {
    slot.published_sequence.store(0U, std::memory_order_relaxed);
  }
}

PortfolioJournal::Reservation PortfolioJournal::try_reserve() noexcept {
  const auto reserved = reserved_sequence_.load(std::memory_order_relaxed);
  if (reserved >= slots_.size()) {
    return {};
  }
  reserved_sequence_.store(reserved + 1U, std::memory_order_relaxed);
  return {.sequence = reserved + 1U, .valid = true};
}

void PortfolioJournal::commit(const Reservation reservation,
                              const PortfolioEvent& event,
                              const ApplyResult& result) noexcept {
  if (!reservation.valid || reservation.sequence == 0U ||
      reservation.sequence > slots_.size()) {
    return;
  }
  const auto index = static_cast<std::size_t>(reservation.sequence - 1U);
  auto& record = slots_[index].record;
  record.sequence = reservation.sequence;
  record.event = event;
  record.result = result;
  record.previous_record_hash = last_record_hash_.load(std::memory_order_relaxed);
  record.stable_hash = stable_journal_record_hash(record);
  slots_[index].published_sequence.store(reservation.sequence,
                                         std::memory_order_release);
  last_record_hash_.store(record.stable_hash, std::memory_order_release);
  published_sequence_.store(reservation.sequence, std::memory_order_release);
}

bool PortfolioJournal::read(const std::uint64_t sequence,
                            PortfolioJournalRecord& output) const noexcept {
  if (sequence == 0U || sequence > slots_.size() ||
      sequence > published_sequence_.load(std::memory_order_acquire)) {
    return false;
  }
  const auto index = static_cast<std::size_t>(sequence - 1U);
  if (slots_[index].published_sequence.load(std::memory_order_acquire) != sequence) {
    return false;
  }
  output = slots_[index].record;
  return stable_journal_record_hash(output) == output.stable_hash;
}

std::uint64_t PortfolioJournal::size() const noexcept {
  return published_sequence_.load(std::memory_order_acquire);
}

std::uint64_t PortfolioJournal::last_record_hash() const noexcept {
  return last_record_hash_.load(std::memory_order_acquire);
}

} // namespace aegis::risk::portfolio
