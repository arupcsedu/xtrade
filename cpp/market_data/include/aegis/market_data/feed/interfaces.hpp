#ifndef AEGIS_MARKET_DATA_FEED_INTERFACES_HPP
#define AEGIS_MARKET_DATA_FEED_INTERFACES_HPP

#include "aegis/market_data/feed/types.hpp"

#include <cstdint>

namespace aegis::market_data::feed {

class PacketReceiver {
public:
  virtual ~PacketReceiver() = default;
  [[nodiscard]] virtual ReceiveStatus receive(RawPacket& output) noexcept = 0;
  virtual void stop() noexcept = 0;
};

enum class KernelReceiveMode : std::uint8_t {
  nonblocking_datagram = 0,
  busy_poll,
  packet_ring,
};

// Provider-neutral operating-system receive abstraction. Concrete kernel and
// licensed venue behavior is intentionally absent from the synthetic phase.
class KernelReceiver : public PacketReceiver {
public:
  [[nodiscard]] virtual KernelReceiveMode receive_mode() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t receive_batch_capacity() const noexcept = 0;
};

class BinaryDecoder {
public:
  virtual ~BinaryDecoder() = default;
  [[nodiscard]] virtual DecodeStatus decode(const RawPacket& packet,
                                            DecodedPacket& output) const noexcept = 0;
};

class RawPacketJournal {
public:
  virtual ~RawPacketJournal() = default;
  [[nodiscard]] virtual PublishStatus enqueue(const RawPacket& packet) noexcept = 0;
};

class FeedHealthPublisher {
public:
  virtual ~FeedHealthPublisher() = default;
  [[nodiscard]] virtual PublishStatus
  publish(const DataQualityState& state) noexcept = 0;
};

class NormalizedEventPublisher {
public:
  virtual ~NormalizedEventPublisher() = default;
  [[nodiscard]] virtual PublishStatus
  publish(const NormalizedEvent& event) noexcept = 0;
  [[nodiscard]] virtual PublishStatus
  publish_snapshot(const RecoverySnapshot& snapshot) noexcept = 0;
};

class RecoveryProvider {
public:
  virtual ~RecoveryProvider() = default;
  [[nodiscard]] virtual RecoveryRequestStatus
  request_retransmission(const FeedIdentity& identity,
                         const SequenceRange& range) noexcept = 0;
  [[nodiscard]] virtual RecoveryRequestStatus
  request_snapshot(const FeedIdentity& identity) noexcept = 0;
  [[nodiscard]] virtual ReceiveStatus
  poll_snapshot(RecoverySnapshot& output) noexcept = 0;
};

} // namespace aegis::market_data::feed

#endif // AEGIS_MARKET_DATA_FEED_INTERFACES_HPP
