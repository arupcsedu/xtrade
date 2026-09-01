#ifndef AEGIS_OMS_JOURNAL_HPP
#define AEGIS_OMS_JOURNAL_HPP

#include "aegis/oms/types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iosfwd>

namespace aegis::oms {

inline constexpr std::size_t kOmsJournalCapacity = 8'192U;

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct OmsJournalRecord {
  std::uint64_t sequence{};
  OmsInput input;
  ApplyResult result;
  std::uint64_t previous_record_hash{};
  std::uint64_t stable_hash{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] std::uint64_t stable_journal_record_hash(OmsJournalRecord value) noexcept;
[[nodiscard]] std::uint64_t oms_journal_abi_tag() noexcept;

class OmsJournal final {
public:
  struct Reservation {
    std::uint64_t sequence{};
    bool valid{false};
  };

  OmsJournal() noexcept;

  [[nodiscard]] Reservation try_reserve() noexcept;
  void commit(Reservation reservation, const OmsInput& input,
              const ApplyResult& result) noexcept;
  [[nodiscard]] bool read(std::uint64_t sequence,
                          OmsJournalRecord& output) const noexcept;
  [[nodiscard]] std::uint64_t size() const noexcept;
  [[nodiscard]] std::uint64_t last_record_hash() const noexcept;
  [[nodiscard]] bool verify_chain() const noexcept;

  // Persistence is deliberately off the hot path. The reference file format
  // is same-build ABI fenced, versioned, hash chained, and corruption checked.
  [[nodiscard]] PersistenceStatus write_binary(std::ostream& output,
                                               std::uint64_t configuration_hash) const;
  [[nodiscard]] PersistenceStatus
  load_binary(std::istream& input, std::uint64_t expected_configuration_hash);

private:
  struct Slot {
    std::atomic<std::uint64_t> published_sequence{0U};
    OmsJournalRecord record;
  };

  std::array<Slot, kOmsJournalCapacity> slots_{};
  alignas(64) std::atomic<std::uint64_t> reserved_sequence_{0U};
  alignas(64) std::atomic<std::uint64_t> published_sequence_{0U};
  alignas(64) std::atomic<std::uint64_t> last_record_hash_{0U};
};

static_assert((kOmsJournalCapacity & (kOmsJournalCapacity - 1U)) == 0U);

} // namespace aegis::oms

#endif // AEGIS_OMS_JOURNAL_HPP
