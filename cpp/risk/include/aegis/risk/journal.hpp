#ifndef AEGIS_RISK_JOURNAL_HPP
#define AEGIS_RISK_JOURNAL_HPP

#include "aegis/risk/types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace aegis::risk {

inline constexpr std::size_t kRiskDecisionJournalCapacity = 4'096U;

class RiskDecisionJournal final {
public:
  struct Reservation {
    std::uint64_t sequence{};
    bool valid{false};
  };

  RiskDecisionJournal() noexcept;

  // The risk engine is the sole producer. A consumer may drain concurrently.
  [[nodiscard]] Reservation try_reserve() noexcept;
  void commit(Reservation reservation, const RiskDecision& decision) noexcept;
  [[nodiscard]] bool try_pop(RiskDecision& decision) noexcept;
  [[nodiscard]] std::uint64_t size() const noexcept;

private:
  struct Slot {
    std::atomic<std::uint64_t> published_sequence{0U};
    RiskDecision decision;
  };

  std::array<Slot, kRiskDecisionJournalCapacity> slots_{};
  alignas(64) std::atomic<std::uint64_t> reserved_sequence_{0U};
  alignas(64) std::atomic<std::uint64_t> published_sequence_{0U};
  alignas(64) std::atomic<std::uint64_t> consumed_sequence_{0U};
};

static_assert((kRiskDecisionJournalCapacity & (kRiskDecisionJournalCapacity - 1U)) ==
              0U);

} // namespace aegis::risk

#endif // AEGIS_RISK_JOURNAL_HPP
