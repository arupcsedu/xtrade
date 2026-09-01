#ifndef AEGIS_RISK_PORTFOLIO_JOURNAL_HPP
#define AEGIS_RISK_PORTFOLIO_JOURNAL_HPP

#include "aegis/risk/portfolio_types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace aegis::risk::portfolio {

inline constexpr std::size_t kPortfolioJournalCapacity = 8'192U;

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct PortfolioJournalRecord {
  std::uint64_t sequence{};
  PortfolioEvent event;
  ApplyResult result;
  std::uint64_t previous_record_hash{};
  std::uint64_t stable_hash{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] std::uint64_t
stable_journal_record_hash(PortfolioJournalRecord value) noexcept;

class PortfolioJournal final {
public:
  struct Reservation {
    std::uint64_t sequence{};
    bool valid{false};
  };

  PortfolioJournal() noexcept;

  // One portfolio service is the producer. Readers may inspect only committed
  // immutable records and never advance or overwrite the append-only range.
  [[nodiscard]] Reservation try_reserve() noexcept;
  void commit(Reservation reservation, const PortfolioEvent& event,
              const ApplyResult& result) noexcept;
  [[nodiscard]] bool read(std::uint64_t sequence,
                          PortfolioJournalRecord& output) const noexcept;
  [[nodiscard]] std::uint64_t size() const noexcept;
  [[nodiscard]] std::uint64_t last_record_hash() const noexcept;

private:
  struct Slot {
    std::atomic<std::uint64_t> published_sequence{0U};
    PortfolioJournalRecord record;
  };

  std::array<Slot, kPortfolioJournalCapacity> slots_{};
  alignas(64) std::atomic<std::uint64_t> reserved_sequence_{0U};
  alignas(64) std::atomic<std::uint64_t> published_sequence_{0U};
  alignas(64) std::atomic<std::uint64_t> last_record_hash_{0U};
};

static_assert((kPortfolioJournalCapacity & (kPortfolioJournalCapacity - 1U)) == 0U);

} // namespace aegis::risk::portfolio

#endif // AEGIS_RISK_PORTFOLIO_JOURNAL_HPP
