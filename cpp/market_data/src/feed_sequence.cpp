#include "aegis/market_data/feed/sequence.hpp"

namespace aegis::market_data::feed {

SequenceDisposition
SequenceTracker::classify(const std::uint64_t sequence) const noexcept {
  if (!valid_ || sequence == 0U || sequence > maximum_) {
    return SequenceDisposition::invalid;
  }
  if (sequence == expected_) {
    return SequenceDisposition::next;
  }
  if (has_last_ && sequence == last_) {
    return SequenceDisposition::duplicate;
  }
  const auto forward = forward_sequence_distance(expected_, sequence, maximum_);
  const auto backward = forward_sequence_distance(sequence, expected_, maximum_);
  return forward < backward ? SequenceDisposition::gap
                            : SequenceDisposition::out_of_order;
}

bool SequenceTracker::commit(const std::uint64_t sequence) noexcept {
  if (classify(sequence) != SequenceDisposition::next) {
    return false;
  }
  last_ = sequence;
  has_last_ = true;
  expected_ = next_sequence(sequence, maximum_);
  return true;
}

void SequenceTracker::resynchronize_after(const std::uint64_t sequence) noexcept {
  if (!valid_ || sequence == 0U || sequence > maximum_) {
    return;
  }
  last_ = sequence;
  has_last_ = true;
  expected_ = next_sequence(sequence, maximum_);
}

bool GapDetector::activate(const std::uint64_t expected, const std::uint64_t received,
                           const std::uint64_t maximum_sequence) noexcept {
  if (active_ || expected == 0U || received == 0U || maximum_sequence == 0U ||
      expected > maximum_sequence || received > maximum_sequence ||
      expected == received) {
    return false;
  }
  const auto distance = forward_sequence_distance(expected, received, maximum_sequence);
  if (distance == 0U) {
    return false;
  }
  range_ = {
      .first = expected,
      .last = previous_sequence(received, maximum_sequence),
      .maximum_sequence = maximum_sequence,
  };
  next_missing_ = expected;
  missing_count_ = distance;
  active_ = true;
  return true;
}

bool GapDetector::mark_applied(const std::uint64_t sequence) noexcept {
  if (!active_ || sequence != next_missing_) {
    return false;
  }
  if (sequence == range_.last) {
    clear();
    return true;
  }
  next_missing_ = next_sequence(next_missing_, range_.maximum_sequence);
  return true;
}

void GapDetector::clear() noexcept {
  range_ = {};
  next_missing_ = 0U;
  missing_count_ = 0U;
  active_ = false;
}

} // namespace aegis::market_data::feed
