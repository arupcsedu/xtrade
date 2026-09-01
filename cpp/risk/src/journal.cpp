#include "aegis/risk/journal.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace aegis::risk {

RiskDecisionJournal::RiskDecisionJournal() noexcept {
  for (auto& slot : slots_) {
    slot.published_sequence.store(0U, std::memory_order_relaxed);
  }
}

RiskDecisionJournal::Reservation RiskDecisionJournal::try_reserve() noexcept {
  const auto reserved = reserved_sequence_.load(std::memory_order_relaxed);
  const auto consumed = consumed_sequence_.load(std::memory_order_acquire);
  if (reserved - consumed >= slots_.size()) {
    return {};
  }
  reserved_sequence_.store(reserved + 1U, std::memory_order_relaxed);
  return {.sequence = reserved + 1U, .valid = true};
}

void RiskDecisionJournal::commit(const Reservation reservation,
                                 const RiskDecision& decision) noexcept {
  if (!reservation.valid || reservation.sequence == 0U) {
    return;
  }
  const auto index = static_cast<std::size_t>((reservation.sequence - 1U) &
                                              (kRiskDecisionJournalCapacity - 1U));
  slots_[index].decision = decision;
  slots_[index].published_sequence.store(reservation.sequence,
                                         std::memory_order_release);
  published_sequence_.store(reservation.sequence, std::memory_order_release);
}

bool RiskDecisionJournal::try_pop(RiskDecision& decision) noexcept {
  const auto consumed = consumed_sequence_.load(std::memory_order_relaxed);
  const auto expected = consumed + 1U;
  if (published_sequence_.load(std::memory_order_acquire) < expected) {
    return false;
  }
  const auto index =
      static_cast<std::size_t>((expected - 1U) & (kRiskDecisionJournalCapacity - 1U));
  if (slots_[index].published_sequence.load(std::memory_order_acquire) != expected) {
    return false;
  }
  decision = slots_[index].decision;
  consumed_sequence_.store(expected, std::memory_order_release);
  return true;
}

std::uint64_t RiskDecisionJournal::size() const noexcept {
  return published_sequence_.load(std::memory_order_acquire) -
         consumed_sequence_.load(std::memory_order_acquire);
}

} // namespace aegis::risk
