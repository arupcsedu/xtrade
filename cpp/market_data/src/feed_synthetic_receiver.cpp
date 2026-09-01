#include "aegis/market_data/feed/synthetic_receiver.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aegis::market_data::feed {

RawPacket make_synthetic_raw_packet(
    const synthetic::SyntheticEvent& event, const aegis::common::SessionId session_id,
    const FeedLeg feed_leg,
    const std::uint64_t received_process_monotonic_time_ns) noexcept {
  RawPacket packet{
      .session_id = session_id,
      .feed_leg = feed_leg,
      .source_sequence_hint = event.channel_sequence,
      .received_process_monotonic_time_ns = received_process_monotonic_time_ns,
      .byte_count = static_cast<std::uint16_t>(synthetic::kPacketBytes),
      .bytes = {},
  };
  const auto encoded = synthetic::encode_packet(event);
  std::ranges::copy(encoded, packet.bytes.begin());
  return packet;
}

// Queue and history capacities are deliberately separate bounded resources.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
SyntheticReceiver::SyntheticReceiver(const FeedIdentity identity,
                                     const std::size_t queue_capacity,
                                     const std::size_t history_capacity) noexcept
    : identity_(identity), input_queue_(queue_capacity),
      recovery_queue_(queue_capacity),
      history_capacity_(history_capacity <= kMaximumReceiverHistory ? history_capacity
                                                                    : 0U) {}
// NOLINTEND(bugprone-easily-swappable-parameters)

bool SyntheticReceiver::enqueue(const RawPacket& packet,
                                const bool retain_for_recovery) noexcept {
  if (stopped_) {
    return false;
  }
  if (!input_queue_.push(packet)) {
    ++dropped_packets_;
    overrun_pending_ = true;
    return false;
  }
  if (retain_for_recovery) {
    static_cast<void>(prime_history(packet));
  }
  return true;
}

bool SyntheticReceiver::prime_history(const RawPacket& packet) noexcept {
  if (history_capacity_ == 0U || packet.source_sequence_hint == 0U) {
    return false;
  }
  history_[history_cursor_] = {.packet = packet, .occupied = true};
  history_cursor_ = (history_cursor_ + 1U) % history_capacity_;
  return true;
}

bool SyntheticReceiver::install_snapshot(const RecoverySnapshot& snapshot) noexcept {
  if (!same_identity(snapshot.identity)) {
    return false;
  }
  snapshot_ = snapshot;
  snapshot_installed_ = true;
  return true;
}

ReceiveStatus SyntheticReceiver::receive(RawPacket& output) noexcept {
  if (stopped_) {
    return ReceiveStatus::stopped;
  }
  if (overrun_pending_) {
    overrun_pending_ = false;
    return ReceiveStatus::overrun;
  }
  if (recovery_queue_.pop(output)) {
    return ReceiveStatus::packet;
  }
  return input_queue_.pop(output) ? ReceiveStatus::packet : ReceiveStatus::empty;
}

void SyntheticReceiver::stop() noexcept {
  stopped_ = true;
  input_queue_.clear();
  recovery_queue_.clear();
  snapshot_pending_ = false;
}

RecoveryRequestStatus
SyntheticReceiver::request_retransmission(const FeedIdentity& identity,
                                          const SequenceRange& range) noexcept {
  ++retransmission_requests_;
  if (stopped_) {
    return RecoveryRequestStatus::stopped;
  }
  if (!same_identity(identity) || !range.valid() || history_capacity_ == 0U) {
    return RecoveryRequestStatus::unavailable;
  }
  const auto distance =
      forward_sequence_distance(range.first, range.last, range.maximum_sequence);
  const auto count = distance + 1U;
  if (count > recovery_queue_.available()) {
    return RecoveryRequestStatus::full;
  }

  auto sequence = range.first;
  for (std::uint64_t index = 0U; index < count; ++index) {
    if (find_history(sequence) == nullptr) {
      return RecoveryRequestStatus::unavailable;
    }
    sequence = next_sequence(sequence, range.maximum_sequence);
  }
  sequence = range.first;
  for (std::uint64_t index = 0U; index < count; ++index) {
    const auto* const history = find_history(sequence);
    if (history == nullptr) {
      return RecoveryRequestStatus::unavailable;
    }
    auto recovered = history->packet;
    recovered.feed_leg = FeedLeg::recovery;
    if (!recovery_queue_.push(recovered)) {
      return RecoveryRequestStatus::full;
    }
    sequence = next_sequence(sequence, range.maximum_sequence);
  }
  return RecoveryRequestStatus::accepted;
}

RecoveryRequestStatus
SyntheticReceiver::request_snapshot(const FeedIdentity& identity) noexcept {
  ++snapshot_requests_;
  if (stopped_) {
    return RecoveryRequestStatus::stopped;
  }
  if (!same_identity(identity) || !snapshot_installed_) {
    return RecoveryRequestStatus::unavailable;
  }
  snapshot_pending_ = true;
  return RecoveryRequestStatus::accepted;
}

ReceiveStatus SyntheticReceiver::poll_snapshot(RecoverySnapshot& output) noexcept {
  if (stopped_) {
    return ReceiveStatus::stopped;
  }
  if (!snapshot_pending_) {
    return ReceiveStatus::empty;
  }
  output = snapshot_;
  snapshot_pending_ = false;
  return ReceiveStatus::packet;
}

std::size_t SyntheticReceiver::queue_size() const noexcept {
  return input_queue_.size() + recovery_queue_.size();
}

std::uint64_t SyntheticReceiver::dropped_packets() const noexcept {
  return dropped_packets_;
}

std::uint64_t SyntheticReceiver::retransmission_requests() const noexcept {
  return retransmission_requests_;
}

std::uint64_t SyntheticReceiver::snapshot_requests() const noexcept {
  return snapshot_requests_;
}

const SyntheticReceiver::HistoryEntry*
SyntheticReceiver::find_history(const std::uint64_t sequence) const noexcept {
  for (std::size_t offset = 0U; offset < history_capacity_; ++offset) {
    const auto index =
        (history_cursor_ + history_capacity_ - 1U - offset) % history_capacity_;
    const auto& entry = history_[index];
    if (entry.occupied && entry.packet.source_sequence_hint == sequence &&
        entry.packet.session_id == identity_.session_id) {
      return &entry;
    }
  }
  return nullptr;
}

bool SyntheticReceiver::same_identity(const FeedIdentity& identity) const noexcept {
  return identity.session_id == identity_.session_id &&
         identity.venue_id == identity_.venue_id &&
         identity.channel_id == identity_.channel_id &&
         identity.venue_number == identity_.venue_number &&
         identity.channel_number == identity_.channel_number;
}

DecodeStatus SyntheticDecoder::decode(const RawPacket& packet,
                                      DecodedPacket& output) const noexcept {
  if (!valid_feed_leg(packet.feed_leg)) {
    return DecodeStatus::invalid_feed_leg;
  }
  if (packet.session_id != identity_.session_id) {
    return DecodeStatus::session_mismatch;
  }
  if (packet.byte_count == 0U || packet.byte_count > packet.bytes.size()) {
    return DecodeStatus::malformed;
  }
  synthetic::SyntheticEvent event{};
  const auto error = synthetic::decode_packet(
      std::span<const std::uint8_t>{packet.bytes}.first(packet.byte_count), event);
  if (error != synthetic::DecodeError::none) {
    return DecodeStatus::malformed;
  }
  if (event.venue_number != identity_.venue_number) {
    return DecodeStatus::venue_mismatch;
  }
  if (event.channel_number != identity_.channel_number) {
    return DecodeStatus::channel_mismatch;
  }
  output = {
      .event = event,
      .feed_leg = packet.feed_leg,
      .recovered = packet.feed_leg == FeedLeg::recovery,
  };
  return DecodeStatus::decoded;
}

} // namespace aegis::market_data::feed
