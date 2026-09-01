#ifndef AEGIS_MARKET_DATA_FEED_ARBITRATOR_HPP
#define AEGIS_MARKET_DATA_FEED_ARBITRATOR_HPP

#include "aegis/market_data/feed/sequence.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace aegis::market_data::feed {

enum class ArbitrationStatus : std::uint8_t {
  ready = 0,
  buffered_gap,
  duplicate,
  out_of_order,
  invalid_sequence,
  conflicting_copy,
  window_exceeded,
};

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct ArbitrationResult {
  ArbitrationStatus status{ArbitrationStatus::invalid_sequence};
  std::uint64_t expected_sequence{};
  std::uint64_t received_sequence{};
};

struct ArbitratedEvent {
  synthetic::SyntheticEvent event;
  FeedLeg source_leg{FeedLeg::a};
  bool recovered{false};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

class FeedAFeedBArbitrator {
public:
  FeedAFeedBArbitrator(std::uint64_t initial_sequence, std::uint64_t maximum_sequence,
                       std::size_t window_capacity) noexcept;

  [[nodiscard]] ArbitrationResult ingest(const DecodedPacket& packet) noexcept;
  [[nodiscard]] bool pop_ready(ArbitratedEvent& output) noexcept;
  void reset(std::uint64_t next_expected_sequence) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint64_t expected_sequence() const noexcept;
  [[nodiscard]] std::uint64_t maximum_sequence() const noexcept;
  [[nodiscard]] std::size_t occupancy() const noexcept;

private:
  struct Slot {
    synthetic::SyntheticEvent event;
    FeedLeg source_leg{FeedLeg::a};
    bool occupied{false};
    bool recovered{false};
  };

  struct RecentFingerprint {
    std::uint64_t sequence{};
    std::uint64_t event_hash{};
    bool occupied{false};
  };

  [[nodiscard]] Slot* find_slot(std::uint64_t sequence) noexcept;
  [[nodiscard]] Slot* find_empty_slot() noexcept;
  [[nodiscard]] const RecentFingerprint*
  find_recent(std::uint64_t sequence) const noexcept;
  void remember(std::uint64_t sequence, std::uint64_t event_hash) noexcept;

  SequenceTracker tracker_;
  std::array<Slot, kMaximumArbitrationWindow> slots_{};
  std::array<RecentFingerprint, kMaximumArbitrationWindow> recent_{};
  std::size_t window_capacity_{};
  std::size_t occupancy_{};
  std::size_t recent_cursor_{};
};

} // namespace aegis::market_data::feed

#endif // AEGIS_MARKET_DATA_FEED_ARBITRATOR_HPP
