#ifndef AEGIS_MARKET_DATA_FEED_SEQUENCE_HPP
#define AEGIS_MARKET_DATA_FEED_SEQUENCE_HPP

#include "aegis/market_data/feed/types.hpp"

#include <cstdint>

namespace aegis::market_data::feed {

enum class SequenceDisposition : std::uint8_t {
  next = 0,
  duplicate,
  gap,
  out_of_order,
  invalid,
};

class SequenceTracker {
public:
  constexpr SequenceTracker(const std::uint64_t initial_sequence,
                            const std::uint64_t maximum_sequence) noexcept
      : expected_(initial_sequence), maximum_(maximum_sequence),
        valid_(initial_sequence != 0U && maximum_sequence != 0U &&
               initial_sequence <= maximum_sequence) {}

  [[nodiscard]] SequenceDisposition classify(std::uint64_t sequence) const noexcept;
  [[nodiscard]] bool commit(std::uint64_t sequence) noexcept;
  void resynchronize_after(std::uint64_t sequence) noexcept;

  [[nodiscard]] constexpr bool valid() const noexcept { return valid_; }
  [[nodiscard]] constexpr std::uint64_t expected() const noexcept { return expected_; }
  [[nodiscard]] constexpr std::uint64_t maximum() const noexcept { return maximum_; }

private:
  std::uint64_t expected_{};
  std::uint64_t maximum_{};
  std::uint64_t last_{};
  bool has_last_{false};
  bool valid_{false};
};

class GapDetector {
public:
  [[nodiscard]] bool activate(std::uint64_t expected, std::uint64_t received,
                              std::uint64_t maximum_sequence) noexcept;
  [[nodiscard]] bool mark_applied(std::uint64_t sequence) noexcept;
  void clear() noexcept;

  [[nodiscard]] constexpr bool active() const noexcept { return active_; }
  [[nodiscard]] constexpr const SequenceRange& range() const noexcept { return range_; }
  [[nodiscard]] constexpr std::uint64_t missing_count() const noexcept {
    return missing_count_;
  }

private:
  SequenceRange range_{};
  std::uint64_t next_missing_{};
  std::uint64_t missing_count_{};
  bool active_{false};
};

} // namespace aegis::market_data::feed

#endif // AEGIS_MARKET_DATA_FEED_SEQUENCE_HPP
