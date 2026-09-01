#include "aegis/market_data/feed/feed_handler.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace aegis::market_data::feed {
namespace {

[[nodiscard]] bool same_identity(const FeedIdentity& left,
                                 const FeedIdentity& right) noexcept {
  return left.session_id == right.session_id && left.venue_id == right.venue_id &&
         left.channel_id == right.channel_id &&
         left.venue_number == right.venue_number &&
         left.channel_number == right.channel_number;
}

} // namespace

FeedHandler::FeedHandler(FeedHandlerConfig config, PacketReceiver& receiver,
                         const BinaryDecoder& decoder,
                         RecoveryProvider& recovery_provider,
                         RawPacketJournal& raw_journal,
                         NormalizedEventPublisher& normalized_publisher,
                         FeedHealthPublisher& health_publisher) noexcept
    : config_(config), receiver_(receiver), decoder_(decoder),
      raw_journal_(raw_journal), normalized_publisher_(normalized_publisher),
      health_publisher_(health_publisher),
      leg_a_tracker_(config.initial_sequence, config.maximum_sequence),
      leg_b_tracker_(config.initial_sequence, config.maximum_sequence),
      arbitrator_(config.initial_sequence, config.maximum_sequence,
                  config.arbitration_window),
      recovery_(recovery_provider, config.identity, config.recovery_timeout_ns),
      data_quality_{
          .identity = config.identity,
          .feed_state = FeedState::starting,
          .quality = DataQualityCode::unknown,
          .reason = FeedHealthReason::starting,
          .observed_process_monotonic_time_ns = 0U,
          .last_good_exchange_event_time_ns = 0,
          .last_good_nic_receive_time_ns = 0,
          .missing_sequence_count = 0U,
          .malformed_record_count = 0U,
          .stale_after_ns = config.stale_after_ns,
          .transition_count = 0U,
      } {}

bool FeedHandler::start(const std::uint64_t now_ns) noexcept {
  if (started_ || !observe_time(now_ns)) {
    return false;
  }
  started_ = true;
  if (!config_.valid() || !arbitrator_.valid() || !leg_a_tracker_.valid() ||
      !leg_b_tracker_.valid()) {
    fail(FeedHealthReason::invalid_sequence, now_ns);
    return false;
  }
  if (!publish_health(FeedHealthReason::starting, now_ns)) {
    return false;
  }
  return transition(FeedState::recovering, FeedHealthReason::awaiting_first_sequence,
                    now_ns);
}

PollResult FeedHandler::poll_once(const std::uint64_t now_ns) noexcept {
  if (state_ == FeedState::stopped) {
    return PollResult::stopped;
  }
  if (!started_ || state_ == FeedState::invalid || !observe_time(now_ns)) {
    return PollResult::invalid;
  }
  if (state_ == FeedState::stale && !request_snapshot(now_ns)) {
    return PollResult::invalid;
  }
  if (recovery_.progress() == RecoveryProgress::snapshot_requested) {
    return poll_snapshot(now_ns);
  }

  RawPacket packet{};
  const auto receive_status = receiver_.receive(packet);
  if (receive_status == ReceiveStatus::empty) {
    return PollResult::idle;
  }
  if (receive_status == ReceiveStatus::stopped) {
    fail(FeedHealthReason::recovery_unavailable, now_ns);
    return PollResult::invalid;
  }
  if (receive_status == ReceiveStatus::overrun) {
    ++metrics_.receiver_overruns;
    fail(FeedHealthReason::receiver_overrun, now_ns);
    return PollResult::invalid;
  }
  ++metrics_.packets_received;
  if (packet.received_process_monotonic_time_ns > now_ns) {
    fail(FeedHealthReason::monotonic_regression, now_ns);
    return PollResult::invalid;
  }
  if (raw_journal_.enqueue(packet) != PublishStatus::accepted) {
    ++metrics_.raw_journal_drops;
    fail(FeedHealthReason::raw_journal_backpressure, now_ns);
    return PollResult::invalid;
  }
  ++metrics_.packets_journaled;

  DecodedPacket decoded{};
  const auto decode_status = decoder_.decode(packet, decoded);
  if (decode_status != DecodeStatus::decoded) {
    if (decode_status == DecodeStatus::session_mismatch ||
        decode_status == DecodeStatus::venue_mismatch ||
        decode_status == DecodeStatus::channel_mismatch) {
      ++metrics_.identity_mismatches;
      fail(FeedHealthReason::identity_mismatch, now_ns);
    } else {
      ++metrics_.malformed_packets;
      data_quality_.malformed_record_count = metrics_.malformed_packets;
      fail(FeedHealthReason::malformed_packet, now_ns);
    }
    return PollResult::invalid;
  }
  ++metrics_.packets_decoded;
  has_valid_activity_ = true;
  last_valid_activity_ns_ = now_ns;

  if (!observe_leg(decoded, now_ns) || !handle_arbitration(decoded, now_ns)) {
    return PollResult::invalid;
  }
  return PollResult::processed;
}

void FeedHandler::on_timer(const std::uint64_t now_ns) noexcept {
  if (!started_ || state_ == FeedState::invalid || state_ == FeedState::stopped ||
      !observe_time(now_ns)) {
    return;
  }
  const auto before = recovery_.progress();
  const auto after = recovery_.on_timer(now_ns);
  if (before == RecoveryProgress::replaying &&
      after == RecoveryProgress::snapshot_requested) {
    ++metrics_.retransmission_failures;
    ++metrics_.snapshots_requested;
    static_cast<void>(
        transition(FeedState::recovering, FeedHealthReason::snapshot_recovery, now_ns));
    return;
  }
  if (after == RecoveryProgress::failed) {
    ++metrics_.retransmission_failures;
    fail(FeedHealthReason::recovery_unavailable, now_ns);
    return;
  }
  if (state_ == FeedState::healthy && has_valid_activity_ &&
      now_ns - last_valid_activity_ns_ >= config_.stale_after_ns) {
    static_cast<void>(
        transition(FeedState::stale, FeedHealthReason::stale_timeout, now_ns));
  }
}

void FeedHandler::stop(const std::uint64_t now_ns) noexcept {
  if (state_ == FeedState::stopped) {
    return;
  }
  if (has_observed_time_ && now_ns < last_observed_time_ns_) {
    ++metrics_.state_transitions;
    state_ = FeedState::invalid;
    data_quality_.feed_state = state_;
    data_quality_.quality = DataQualityCode::invalid;
    data_quality_.reason = FeedHealthReason::monotonic_regression;
    data_quality_.transition_count = metrics_.state_transitions;
  } else {
    last_observed_time_ns_ = now_ns;
    has_observed_time_ = true;
  }
  receiver_.stop();
  ++metrics_.state_transitions;
  state_ = FeedState::stopped;
  data_quality_.feed_state = state_;
  data_quality_.quality = DataQualityCode::invalid;
  data_quality_.reason = FeedHealthReason::stopped;
  data_quality_.observed_process_monotonic_time_ns = now_ns;
  data_quality_.transition_count = metrics_.state_transitions;
  if (health_publisher_.publish(data_quality_) != PublishStatus::accepted) {
    ++metrics_.health_publisher_drops;
  }
}

FeedState FeedHandler::state() const noexcept { return state_; }

const DataQualityState& FeedHandler::data_quality() const noexcept {
  return data_quality_;
}

const FeedMetrics& FeedHandler::metrics() const noexcept { return metrics_; }

bool FeedHandler::observe_time(const std::uint64_t now_ns) noexcept {
  if (has_observed_time_ && now_ns < last_observed_time_ns_) {
    fail(FeedHealthReason::monotonic_regression, last_observed_time_ns_);
    return false;
  }
  has_observed_time_ = true;
  last_observed_time_ns_ = now_ns;
  return true;
}

bool FeedHandler::transition(const FeedState state, const FeedHealthReason reason,
                             const std::uint64_t now_ns) noexcept {
  if (state_ != state) {
    state_ = state;
    ++metrics_.state_transitions;
  }
  return publish_health(reason, now_ns);
}

bool FeedHandler::publish_health(const FeedHealthReason reason,
                                 const std::uint64_t now_ns) noexcept {
  data_quality_.feed_state = state_;
  data_quality_.quality = current_quality();
  data_quality_.reason = reason;
  data_quality_.observed_process_monotonic_time_ns = now_ns;
  data_quality_.missing_sequence_count = metrics_.missing_sequences;
  data_quality_.malformed_record_count = metrics_.malformed_packets;
  data_quality_.transition_count = metrics_.state_transitions;
  if (health_publisher_.publish(data_quality_) == PublishStatus::accepted) {
    return true;
  }
  ++metrics_.health_publisher_drops;
  if (state_ != FeedState::invalid) {
    state_ = FeedState::invalid;
    ++metrics_.state_transitions;
  }
  data_quality_.feed_state = FeedState::invalid;
  data_quality_.quality = DataQualityCode::invalid;
  data_quality_.reason = FeedHealthReason::health_publisher_backpressure;
  data_quality_.transition_count = metrics_.state_transitions;
  return false;
}

void FeedHandler::fail(const FeedHealthReason reason,
                       const std::uint64_t now_ns) noexcept {
  if (state_ == FeedState::stopped) {
    return;
  }
  static_cast<void>(transition(FeedState::invalid, reason, now_ns));
}

DataQualityCode FeedHandler::current_quality() const noexcept {
  switch (state_) {
  case FeedState::starting:
    return DataQualityCode::unknown;
  case FeedState::healthy:
    return leg_a_degraded_ || leg_b_degraded_ ? DataQualityCode::degraded
                                              : DataQualityCode::valid;
  case FeedState::recovering:
  case FeedState::gap_detected:
  case FeedState::replaying_gap:
    return DataQualityCode::degraded;
  case FeedState::stale:
    return DataQualityCode::stale;
  case FeedState::invalid:
  case FeedState::stopped:
    return DataQualityCode::invalid;
  }
  return DataQualityCode::invalid;
}

bool FeedHandler::observe_leg(const DecodedPacket& packet,
                              const std::uint64_t now_ns) noexcept {
  if (packet.feed_leg == FeedLeg::recovery) {
    return true;
  }
  auto& tracker = packet.feed_leg == FeedLeg::a ? leg_a_tracker_ : leg_b_tracker_;
  auto& degraded = packet.feed_leg == FeedLeg::a ? leg_a_degraded_ : leg_b_degraded_;
  const auto sequence = packet.event.channel_sequence;
  const auto disposition = tracker.classify(sequence);
  if (disposition == SequenceDisposition::invalid) {
    fail(FeedHealthReason::invalid_sequence, now_ns);
    return false;
  }
  if (disposition == SequenceDisposition::next) {
    static_cast<void>(tracker.commit(sequence));
    if (degraded) {
      degraded = false;
      if (!publish_health(FeedHealthReason::continuous, now_ns)) {
        return false;
      }
    }
    return true;
  }
  if (disposition == SequenceDisposition::gap) {
    ++metrics_.redundant_leg_gaps;
    tracker.resynchronize_after(sequence);
    degraded = true;
    return publish_health(FeedHealthReason::redundant_leg_gap, now_ns);
  }
  if (disposition == SequenceDisposition::duplicate) {
    return true;
  }
  ++metrics_.redundant_leg_out_of_order;
  return true;
}

bool FeedHandler::handle_arbitration(const DecodedPacket& packet,
                                     const std::uint64_t now_ns) noexcept {
  const auto result = arbitrator_.ingest(packet);
  metrics_.maximum_arbitration_occupancy =
      std::max(metrics_.maximum_arbitration_occupancy,
               static_cast<std::uint64_t>(arbitrator_.occupancy()));
  switch (result.status) {
  case ArbitrationStatus::ready:
    if (state_ == FeedState::recovering && !gap_detector_.active()) {
      if (!transition(FeedState::healthy, FeedHealthReason::continuous, now_ns)) {
        return false;
      }
    }
    return drain_ready(now_ns);
  case ArbitrationStatus::buffered_gap: {
    if (gap_detector_.active()) {
      return true;
    }
    if (!gap_detector_.activate(result.expected_sequence, result.received_sequence,
                                arbitrator_.maximum_sequence())) {
      fail(FeedHealthReason::invalid_sequence, now_ns);
      return false;
    }
    ++metrics_.canonical_gaps;
    metrics_.missing_sequences += gap_detector_.missing_count();
    if (!transition(FeedState::gap_detected, FeedHealthReason::sequence_gap, now_ns)) {
      return false;
    }
    ++metrics_.retransmission_requests;
    const auto progress = recovery_.begin_gap(gap_detector_.range(), now_ns);
    if (progress == RecoveryProgress::replaying) {
      return transition(FeedState::replaying_gap, FeedHealthReason::replay_in_progress,
                        now_ns);
    }
    ++metrics_.retransmission_failures;
    if (progress == RecoveryProgress::snapshot_requested) {
      ++metrics_.snapshots_requested;
      return transition(FeedState::recovering, FeedHealthReason::snapshot_recovery,
                        now_ns);
    }
    fail(FeedHealthReason::recovery_unavailable, now_ns);
    return false;
  }
  case ArbitrationStatus::duplicate:
    ++metrics_.duplicate_packets;
    return true;
  case ArbitrationStatus::out_of_order:
    ++metrics_.out_of_order_packets;
    fail(FeedHealthReason::out_of_order_sequence, now_ns);
    return false;
  case ArbitrationStatus::invalid_sequence:
    fail(FeedHealthReason::invalid_sequence, now_ns);
    return false;
  case ArbitrationStatus::conflicting_copy:
    fail(FeedHealthReason::redundant_feed_conflict, now_ns);
    return false;
  case ArbitrationStatus::window_exceeded:
    ++metrics_.arbitration_overloads;
    fail(FeedHealthReason::arbitration_overload, now_ns);
    return false;
  }
  fail(FeedHealthReason::invalid_sequence, now_ns);
  return false;
}

bool FeedHandler::drain_ready(const std::uint64_t now_ns) noexcept {
  ArbitratedEvent ready{};
  while (arbitrator_.pop_ready(ready)) {
    data_quality_.feed_state = state_;
    data_quality_.quality = current_quality();
    data_quality_.reason = gap_detector_.active() ? FeedHealthReason::replay_in_progress
                                                  : FeedHealthReason::continuous;
    data_quality_.observed_process_monotonic_time_ns = now_ns;
    data_quality_.missing_sequence_count = metrics_.missing_sequences;
    data_quality_.malformed_record_count = metrics_.malformed_packets;
    if (data_quality_.quality == DataQualityCode::valid) {
      data_quality_.last_good_exchange_event_time_ns =
          ready.event.exchange_event_time_ns;
      data_quality_.last_good_nic_receive_time_ns = ready.event.nic_receive_time_ns;
    }
    const NormalizedEvent normalized{
        .event = ready.event,
        .source_leg = ready.source_leg,
        .recovered = ready.recovered,
        .data_quality = data_quality_,
    };
    if (normalized_publisher_.publish(normalized) != PublishStatus::accepted) {
      ++metrics_.normalized_publisher_drops;
      fail(FeedHealthReason::normalized_publisher_backpressure, now_ns);
      return false;
    }
    ++metrics_.normalized_events;
    if (gap_detector_.active() &&
        gap_detector_.mark_applied(ready.event.channel_sequence) &&
        !gap_detector_.active()) {
      recovery_.complete();
      if (!transition(FeedState::healthy, FeedHealthReason::continuous, now_ns)) {
        return false;
      }
    }
  }
  return true;
}

bool FeedHandler::apply_snapshot(const RecoverySnapshot& snapshot,
                                 const std::uint64_t now_ns) noexcept {
  if (!same_identity(snapshot.identity, config_.identity) ||
      snapshot.last_sequence == 0U ||
      snapshot.last_sequence > config_.maximum_sequence ||
      snapshot.books.size() == 0U || !snapshot.books.all_valid() ||
      snapshot.book_hash == 0U || snapshot.books.stable_hash() != snapshot.book_hash ||
      snapshot.observed_process_monotonic_time_ns > now_ns) {
    fail(FeedHealthReason::snapshot_invalid, now_ns);
    return false;
  }
  if (normalized_publisher_.publish_snapshot(snapshot) != PublishStatus::accepted) {
    ++metrics_.normalized_publisher_drops;
    fail(FeedHealthReason::normalized_publisher_backpressure, now_ns);
    return false;
  }
  const auto expected = next_sequence(snapshot.last_sequence, config_.maximum_sequence);
  arbitrator_.reset(expected);
  leg_a_tracker_ = SequenceTracker{expected, config_.maximum_sequence};
  leg_b_tracker_ = SequenceTracker{expected, config_.maximum_sequence};
  leg_a_degraded_ = false;
  leg_b_degraded_ = false;
  gap_detector_.clear();
  recovery_.complete();
  ++metrics_.snapshots_applied;
  has_valid_activity_ = true;
  last_valid_activity_ns_ = now_ns;
  return transition(FeedState::healthy, FeedHealthReason::continuous, now_ns);
}

PollResult FeedHandler::poll_snapshot(const std::uint64_t now_ns) noexcept {
  RecoverySnapshot snapshot{};
  const auto status = recovery_.poll_snapshot(snapshot);
  if (status == ReceiveStatus::empty) {
    return PollResult::idle;
  }
  if (status != ReceiveStatus::packet || !apply_snapshot(snapshot, now_ns)) {
    if (state_ != FeedState::invalid) {
      fail(FeedHealthReason::recovery_unavailable, now_ns);
    }
    return PollResult::invalid;
  }
  return PollResult::processed;
}

bool FeedHandler::request_snapshot(const std::uint64_t now_ns) noexcept {
  ++metrics_.snapshots_requested;
  const auto progress = recovery_.request_snapshot();
  if (progress != RecoveryProgress::snapshot_requested) {
    fail(FeedHealthReason::recovery_unavailable, now_ns);
    return false;
  }
  return transition(FeedState::recovering, FeedHealthReason::snapshot_recovery, now_ns);
}

} // namespace aegis::market_data::feed
