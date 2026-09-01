#ifndef AEGIS_MARKET_DATA_FEED_FEED_HANDLER_HPP
#define AEGIS_MARKET_DATA_FEED_FEED_HANDLER_HPP

#include "aegis/market_data/feed/arbitrator.hpp"
#include "aegis/market_data/feed/interfaces.hpp"
#include "aegis/market_data/feed/recovery.hpp"

#include <cstdint>

namespace aegis::market_data::feed {

enum class PollResult : std::uint8_t { processed = 0, idle, invalid, stopped };

class FeedHandler {
public:
  FeedHandler(FeedHandlerConfig config, PacketReceiver& receiver,
              const BinaryDecoder& decoder, RecoveryProvider& recovery_provider,
              RawPacketJournal& raw_journal,
              NormalizedEventPublisher& normalized_publisher,
              FeedHealthPublisher& health_publisher) noexcept;

  [[nodiscard]] bool start(std::uint64_t now_ns) noexcept;
  [[nodiscard]] PollResult poll_once(std::uint64_t now_ns) noexcept;
  void on_timer(std::uint64_t now_ns) noexcept;
  void stop(std::uint64_t now_ns) noexcept;

  [[nodiscard]] FeedState state() const noexcept;
  [[nodiscard]] const DataQualityState& data_quality() const noexcept;
  [[nodiscard]] const FeedMetrics& metrics() const noexcept;

private:
  [[nodiscard]] bool observe_time(std::uint64_t now_ns) noexcept;
  [[nodiscard]] bool transition(FeedState state, FeedHealthReason reason,
                                std::uint64_t now_ns) noexcept;
  [[nodiscard]] bool publish_health(FeedHealthReason reason,
                                    std::uint64_t now_ns) noexcept;
  void fail(FeedHealthReason reason, std::uint64_t now_ns) noexcept;
  [[nodiscard]] DataQualityCode current_quality() const noexcept;
  [[nodiscard]] bool observe_leg(const DecodedPacket& packet,
                                 std::uint64_t now_ns) noexcept;
  [[nodiscard]] bool handle_arbitration(const DecodedPacket& packet,
                                        std::uint64_t now_ns) noexcept;
  [[nodiscard]] bool drain_ready(std::uint64_t now_ns) noexcept;
  [[nodiscard]] bool apply_snapshot(const RecoverySnapshot& snapshot,
                                    std::uint64_t now_ns) noexcept;
  [[nodiscard]] PollResult poll_snapshot(std::uint64_t now_ns) noexcept;
  [[nodiscard]] bool request_snapshot(std::uint64_t now_ns) noexcept;

  FeedHandlerConfig config_;
  PacketReceiver& receiver_;
  const BinaryDecoder& decoder_;
  RawPacketJournal& raw_journal_;
  NormalizedEventPublisher& normalized_publisher_;
  FeedHealthPublisher& health_publisher_;
  SequenceTracker leg_a_tracker_;
  SequenceTracker leg_b_tracker_;
  FeedAFeedBArbitrator arbitrator_;
  GapDetector gap_detector_;
  RecoveryCoordinator recovery_;
  DataQualityState data_quality_{};
  FeedMetrics metrics_{};
  FeedState state_{FeedState::starting};
  std::uint64_t last_observed_time_ns_{};
  std::uint64_t last_valid_activity_ns_{};
  bool has_observed_time_{false};
  bool has_valid_activity_{false};
  bool started_{false};
  bool leg_a_degraded_{false};
  bool leg_b_degraded_{false};
};

} // namespace aegis::market_data::feed

#endif // AEGIS_MARKET_DATA_FEED_FEED_HANDLER_HPP
