#include "aegis/market_data/feed/arbitrator.hpp"

#include <algorithm>

namespace aegis::market_data::feed {

// The public constructor mirrors the sequence contract's named fields.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
FeedAFeedBArbitrator::FeedAFeedBArbitrator(const std::uint64_t initial_sequence,
                                           const std::uint64_t maximum_sequence,
                                           const std::size_t window_capacity) noexcept
    : tracker_(initial_sequence, maximum_sequence),
      window_capacity_(window_capacity <= kMaximumArbitrationWindow ? window_capacity
                                                                    : 0U) {}
// NOLINTEND(bugprone-easily-swappable-parameters)

ArbitrationResult FeedAFeedBArbitrator::ingest(const DecodedPacket& packet) noexcept {
  const auto sequence = packet.event.channel_sequence;
  const auto expected = tracker_.expected();
  if (!valid_feed_leg(packet.feed_leg)) {
    return {.status = ArbitrationStatus::invalid_sequence,
            .expected_sequence = expected,
            .received_sequence = sequence};
  }
  if (const auto* prior = find_recent(sequence); prior != nullptr) {
    const auto status = prior->event_hash == packet.event.event_hash
                            ? ArbitrationStatus::duplicate
                            : ArbitrationStatus::conflicting_copy;
    return {
        .status = status, .expected_sequence = expected, .received_sequence = sequence};
  }
  if (auto* existing = find_slot(sequence); existing != nullptr) {
    if (existing->event.event_hash != packet.event.event_hash) {
      return {.status = ArbitrationStatus::conflicting_copy,
              .expected_sequence = expected,
              .received_sequence = sequence};
    }
    existing->recovered = existing->recovered || packet.recovered;
    return {.status = ArbitrationStatus::duplicate,
            .expected_sequence = expected,
            .received_sequence = sequence};
  }

  const auto disposition = tracker_.classify(sequence);
  if (disposition == SequenceDisposition::invalid) {
    return {.status = ArbitrationStatus::invalid_sequence,
            .expected_sequence = expected,
            .received_sequence = sequence};
  }
  if (disposition == SequenceDisposition::duplicate) {
    return {.status = ArbitrationStatus::duplicate,
            .expected_sequence = expected,
            .received_sequence = sequence};
  }
  if (disposition == SequenceDisposition::out_of_order) {
    return {.status = ArbitrationStatus::out_of_order,
            .expected_sequence = expected,
            .received_sequence = sequence};
  }
  const auto distance =
      forward_sequence_distance(expected, sequence, tracker_.maximum());
  if (distance >= window_capacity_) {
    return {.status = ArbitrationStatus::window_exceeded,
            .expected_sequence = expected,
            .received_sequence = sequence};
  }
  auto* slot = find_empty_slot();
  if (slot == nullptr) {
    return {.status = ArbitrationStatus::window_exceeded,
            .expected_sequence = expected,
            .received_sequence = sequence};
  }
  *slot = {
      .event = packet.event,
      .source_leg = packet.feed_leg,
      .occupied = true,
      .recovered = packet.recovered,
  };
  ++occupancy_;
  return {.status = disposition == SequenceDisposition::next
                        ? ArbitrationStatus::ready
                        : ArbitrationStatus::buffered_gap,
          .expected_sequence = expected,
          .received_sequence = sequence};
}

bool FeedAFeedBArbitrator::pop_ready(ArbitratedEvent& output) noexcept {
  auto* slot = find_slot(tracker_.expected());
  if (slot == nullptr) {
    return false;
  }
  output = {
      .event = slot->event,
      .source_leg = slot->source_leg,
      .recovered = slot->recovered,
  };
  remember(slot->event.channel_sequence, slot->event.event_hash);
  if (!tracker_.commit(slot->event.channel_sequence)) {
    return false;
  }
  *slot = {};
  --occupancy_;
  return true;
}

void FeedAFeedBArbitrator::reset(const std::uint64_t next_expected_sequence) noexcept {
  tracker_ = SequenceTracker{next_expected_sequence, tracker_.maximum()};
  std::ranges::fill(slots_, Slot{});
  std::ranges::fill(recent_, RecentFingerprint{});
  occupancy_ = 0U;
  recent_cursor_ = 0U;
}

bool FeedAFeedBArbitrator::valid() const noexcept {
  return tracker_.valid() && window_capacity_ != 0U;
}

std::uint64_t FeedAFeedBArbitrator::expected_sequence() const noexcept {
  return tracker_.expected();
}

std::uint64_t FeedAFeedBArbitrator::maximum_sequence() const noexcept {
  return tracker_.maximum();
}

std::size_t FeedAFeedBArbitrator::occupancy() const noexcept { return occupancy_; }

FeedAFeedBArbitrator::Slot*
FeedAFeedBArbitrator::find_slot(const std::uint64_t sequence) noexcept {
  auto* const end = slots_.begin() + static_cast<std::ptrdiff_t>(window_capacity_);
  auto* const found = std::find_if(slots_.begin(), end, [sequence](const Slot& slot) {
    return slot.occupied && slot.event.channel_sequence == sequence;
  });
  return found == end ? nullptr : &*found;
}

FeedAFeedBArbitrator::Slot* FeedAFeedBArbitrator::find_empty_slot() noexcept {
  auto* const end = slots_.begin() + static_cast<std::ptrdiff_t>(window_capacity_);
  auto* const found = std::find_if(slots_.begin(), end,
                                   [](const Slot& slot) { return !slot.occupied; });
  return found == end ? nullptr : &*found;
}

const FeedAFeedBArbitrator::RecentFingerprint*
FeedAFeedBArbitrator::find_recent(const std::uint64_t sequence) const noexcept {
  const auto* const end =
      recent_.begin() + static_cast<std::ptrdiff_t>(window_capacity_);
  const auto* const found =
      std::find_if(recent_.begin(), end, [sequence](const RecentFingerprint& recent) {
        return recent.occupied && recent.sequence == sequence;
      });
  return found == end ? nullptr : &*found;
}

void FeedAFeedBArbitrator::remember(const std::uint64_t sequence,
                                    const std::uint64_t event_hash) noexcept {
  recent_[recent_cursor_] = {
      .sequence = sequence,
      .event_hash = event_hash,
      .occupied = true,
  };
  recent_cursor_ = (recent_cursor_ + 1U) % window_capacity_;
}

} // namespace aegis::market_data::feed
