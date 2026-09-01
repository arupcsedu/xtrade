#ifndef AEGIS_EXECUTION_JOURNAL_HPP
#define AEGIS_EXECUTION_JOURNAL_HPP

#include "aegis/execution/types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace aegis::execution {

class GatewayAuditJournal final {
public:
  struct Reservation {
    std::uint64_t sequence{};
    bool valid{false};
  };

  GatewayAuditJournal() noexcept;
  GatewayAuditJournal(const GatewayAuditJournal&) = delete;
  GatewayAuditJournal& operator=(const GatewayAuditJournal&) = delete;

  [[nodiscard]] Reservation try_reserve() noexcept;
  [[nodiscard]] bool commit(Reservation reservation,
                            GatewayAuditRecord record) noexcept;
  [[nodiscard]] bool read(std::uint64_t sequence,
                          GatewayAuditRecord& output) const noexcept;
  [[nodiscard]] std::uint64_t size() const noexcept;
  [[nodiscard]] std::uint64_t last_record_hash() const noexcept;
  [[nodiscard]] bool verify_chain() const noexcept;

private:
  struct Slot {
    GatewayAuditRecord record;
    std::atomic<std::uint64_t> published_sequence{0U};
  };

  std::array<Slot, kMaximumGatewayJournalRecords> slots_{};
  std::atomic<std::uint64_t> reserved_sequence_{0U};
  std::atomic<std::uint64_t> published_sequence_{0U};
  std::atomic<std::uint64_t> last_record_hash_{0U};
};

} // namespace aegis::execution

#endif // AEGIS_EXECUTION_JOURNAL_HPP
