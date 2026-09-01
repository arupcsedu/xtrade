#ifndef AEGIS_MARKET_DATA_FEED_SYNTHETIC_RECEIVER_HPP
#define AEGIS_MARKET_DATA_FEED_SYNTHETIC_RECEIVER_HPP

#include "aegis/market_data/feed/bounded_queue.hpp"
#include "aegis/market_data/feed/interfaces.hpp"
#include "aegis/market_data/synthetic/mock_protocol.hpp"

#include <array>
#include <cstddef>

namespace aegis::market_data::feed {

[[nodiscard]] RawPacket
make_synthetic_raw_packet(const synthetic::SyntheticEvent& event,
                          aegis::common::SessionId session_id, FeedLeg feed_leg,
                          std::uint64_t received_process_monotonic_time_ns) noexcept;

class SyntheticReceiver final : public PacketReceiver, public RecoveryProvider {
public:
  SyntheticReceiver(FeedIdentity identity, std::size_t queue_capacity,
                    std::size_t history_capacity) noexcept;

  [[nodiscard]] bool enqueue(const RawPacket& packet,
                             bool retain_for_recovery = true) noexcept;
  [[nodiscard]] bool prime_history(const RawPacket& packet) noexcept;
  [[nodiscard]] bool install_snapshot(const RecoverySnapshot& snapshot) noexcept;

  [[nodiscard]] ReceiveStatus receive(RawPacket& output) noexcept override;
  void stop() noexcept override;

  [[nodiscard]] RecoveryRequestStatus
  request_retransmission(const FeedIdentity& identity,
                         const SequenceRange& range) noexcept override;
  [[nodiscard]] RecoveryRequestStatus
  request_snapshot(const FeedIdentity& identity) noexcept override;
  [[nodiscard]] ReceiveStatus poll_snapshot(RecoverySnapshot& output) noexcept override;

  [[nodiscard]] std::size_t queue_size() const noexcept;
  [[nodiscard]] std::uint64_t dropped_packets() const noexcept;
  [[nodiscard]] std::uint64_t retransmission_requests() const noexcept;
  [[nodiscard]] std::uint64_t snapshot_requests() const noexcept;

private:
  struct HistoryEntry {
    RawPacket packet;
    bool occupied{false};
  };

  [[nodiscard]] const HistoryEntry* find_history(std::uint64_t sequence) const noexcept;
  [[nodiscard]] bool same_identity(const FeedIdentity& identity) const noexcept;

  FeedIdentity identity_;
  BoundedQueue<RawPacket, kMaximumReceiverQueue> input_queue_;
  BoundedQueue<RawPacket, kMaximumReceiverQueue> recovery_queue_;
  std::array<HistoryEntry, kMaximumReceiverHistory> history_{};
  std::size_t history_capacity_{};
  std::size_t history_cursor_{};
  RecoverySnapshot snapshot_{};
  bool snapshot_installed_{false};
  bool snapshot_pending_{false};
  bool overrun_pending_{false};
  bool stopped_{false};
  std::uint64_t dropped_packets_{};
  std::uint64_t retransmission_requests_{};
  std::uint64_t snapshot_requests_{};
};

class SyntheticDecoder final : public BinaryDecoder {
public:
  explicit constexpr SyntheticDecoder(const FeedIdentity identity) noexcept
      : identity_(identity) {}

  [[nodiscard]] DecodeStatus decode(const RawPacket& packet,
                                    DecodedPacket& output) const noexcept override;

private:
  FeedIdentity identity_;
};

} // namespace aegis::market_data::feed

#endif // AEGIS_MARKET_DATA_FEED_SYNTHETIC_RECEIVER_HPP
