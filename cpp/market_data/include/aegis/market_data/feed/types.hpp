#ifndef AEGIS_MARKET_DATA_FEED_TYPES_HPP
#define AEGIS_MARKET_DATA_FEED_TYPES_HPP

#include "aegis/common/identifiers.hpp"
#include "aegis/market_data/synthetic/book.hpp"
#include "aegis/market_data/synthetic/event.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace aegis::market_data::feed {

inline constexpr std::size_t kMaximumRawPacketBytes = 2'048U;
inline constexpr std::size_t kMaximumArbitrationWindow = 64U;
inline constexpr std::size_t kMaximumReceiverQueue = 256U;
inline constexpr std::size_t kMaximumReceiverHistory = 256U;

enum class FeedLeg : std::uint8_t { a = 1, b = 2, recovery = 3 };

enum class FeedState : std::uint8_t {
  starting = 0,
  recovering,
  healthy,
  gap_detected,
  replaying_gap,
  stale,
  invalid,
  stopped,
};

enum class DataQualityCode : std::uint8_t {
  unknown = 0,
  valid = 1,
  degraded = 2,
  stale = 3,
  invalid = 4,
};

enum class FeedHealthReason : std::uint8_t {
  starting = 0,
  awaiting_first_sequence,
  continuous,
  redundant_leg_gap,
  sequence_gap,
  replay_in_progress,
  snapshot_recovery,
  stale_timeout,
  receiver_overrun,
  raw_journal_backpressure,
  malformed_packet,
  identity_mismatch,
  invalid_sequence,
  out_of_order_sequence,
  redundant_feed_conflict,
  arbitration_overload,
  normalized_publisher_backpressure,
  health_publisher_backpressure,
  recovery_unavailable,
  snapshot_invalid,
  monotonic_regression,
  stopped,
};

enum class ReceiveStatus : std::uint8_t { packet = 0, empty, overrun, stopped };

enum class PublishStatus : std::uint8_t { accepted = 0, full, stopped };

enum class RecoveryRequestStatus : std::uint8_t {
  accepted = 0,
  unavailable,
  full,
  stopped,
};

enum class DecodeStatus : std::uint8_t {
  decoded = 0,
  malformed,
  session_mismatch,
  venue_mismatch,
  channel_mismatch,
  invalid_feed_leg,
};

// Fixed-layout values cross the packet loop. Public members support aggregate
// initialization without builders or allocation.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct FeedIdentity {
  aegis::common::SessionId session_id;
  aegis::common::VenueId venue_id;
  aegis::common::ChannelId channel_id;
  std::uint32_t venue_number{};
  std::uint32_t channel_number{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return session_id.valid() && venue_id.valid() && channel_id.valid() &&
           venue_number != 0U && channel_number != 0U;
  }
};

struct FeedHandlerConfig {
  FeedIdentity identity;
  std::uint64_t initial_sequence{1U};
  std::uint64_t maximum_sequence{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t stale_after_ns{1'000'000'000U};
  std::uint64_t recovery_timeout_ns{100'000'000U};
  std::size_t arbitration_window{32U};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return identity.valid() && initial_sequence != 0U && maximum_sequence != 0U &&
           initial_sequence <= maximum_sequence && stale_after_ns != 0U &&
           recovery_timeout_ns != 0U && arbitration_window != 0U &&
           arbitration_window <= kMaximumArbitrationWindow;
  }
};

struct RawPacket {
  aegis::common::SessionId session_id;
  FeedLeg feed_leg{FeedLeg::a};
  std::uint64_t source_sequence_hint{};
  std::uint64_t received_process_monotonic_time_ns{};
  std::uint16_t byte_count{};
  std::array<std::uint8_t, kMaximumRawPacketBytes> bytes{};
};

struct DecodedPacket {
  synthetic::SyntheticEvent event;
  FeedLeg feed_leg{FeedLeg::a};
  bool recovered{false};
};

struct SequenceRange {
  std::uint64_t first{};
  std::uint64_t last{};
  std::uint64_t maximum_sequence{std::numeric_limits<std::uint64_t>::max()};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return first != 0U && last != 0U && maximum_sequence != 0U &&
           first <= maximum_sequence && last <= maximum_sequence;
  }
};

struct DataQualityState {
  FeedIdentity identity;
  FeedState feed_state{FeedState::starting};
  DataQualityCode quality{DataQualityCode::unknown};
  FeedHealthReason reason{FeedHealthReason::starting};
  std::uint64_t observed_process_monotonic_time_ns{};
  std::int64_t last_good_exchange_event_time_ns{};
  std::int64_t last_good_nic_receive_time_ns{};
  std::uint64_t missing_sequence_count{};
  std::uint64_t malformed_record_count{};
  std::uint64_t stale_after_ns{};
  std::uint64_t transition_count{};
};

struct FeedMetrics {
  std::uint64_t packets_received{};
  std::uint64_t packets_journaled{};
  std::uint64_t packets_decoded{};
  std::uint64_t normalized_events{};
  std::uint64_t duplicate_packets{};
  std::uint64_t out_of_order_packets{};
  std::uint64_t canonical_gaps{};
  std::uint64_t redundant_leg_gaps{};
  std::uint64_t redundant_leg_out_of_order{};
  std::uint64_t missing_sequences{};
  std::uint64_t malformed_packets{};
  std::uint64_t identity_mismatches{};
  std::uint64_t retransmission_requests{};
  std::uint64_t retransmission_failures{};
  std::uint64_t snapshots_requested{};
  std::uint64_t snapshots_applied{};
  std::uint64_t receiver_overruns{};
  std::uint64_t raw_journal_drops{};
  std::uint64_t normalized_publisher_drops{};
  std::uint64_t health_publisher_drops{};
  std::uint64_t arbitration_overloads{};
  std::uint64_t state_transitions{};
  std::uint64_t maximum_arbitration_occupancy{};
};

struct RecoverySnapshot {
  FeedIdentity identity;
  std::uint64_t last_sequence{};
  std::uint64_t observed_process_monotonic_time_ns{};
  synthetic::BookSet books;
  std::uint64_t book_hash{};
};

struct NormalizedEvent {
  synthetic::SyntheticEvent event;
  FeedLeg source_leg{FeedLeg::a};
  bool recovered{false};
  DataQualityState data_quality;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] constexpr bool valid_feed_leg(const FeedLeg leg) noexcept {
  return leg == FeedLeg::a || leg == FeedLeg::b || leg == FeedLeg::recovery;
}

[[nodiscard]] constexpr std::uint64_t
next_sequence(const std::uint64_t sequence,
              const std::uint64_t maximum_sequence) noexcept {
  return sequence == maximum_sequence ? 1U : sequence + 1U;
}

[[nodiscard]] constexpr std::uint64_t
previous_sequence(const std::uint64_t sequence,
                  const std::uint64_t maximum_sequence) noexcept {
  return sequence == 1U ? maximum_sequence : sequence - 1U;
}

[[nodiscard]] constexpr std::uint64_t
forward_sequence_distance(const std::uint64_t from, const std::uint64_t to,
                          const std::uint64_t maximum_sequence) noexcept {
  return to >= from ? to - from : (maximum_sequence - from) + to;
}

} // namespace aegis::market_data::feed

#endif // AEGIS_MARKET_DATA_FEED_TYPES_HPP
