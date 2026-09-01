#include "aegis/market_data/feed/recovery.hpp"

#include <limits>

namespace aegis::market_data::feed {

RecoveryCoordinator::RecoveryCoordinator(RecoveryProvider& provider,
                                         const FeedIdentity identity,
                                         const std::uint64_t timeout_ns) noexcept
    : provider_(provider), identity_(identity), timeout_ns_(timeout_ns) {}

RecoveryProgress RecoveryCoordinator::begin_gap(const SequenceRange& range,
                                                const std::uint64_t now_ns) noexcept {
  if (progress_ != RecoveryProgress::idle || timeout_ns_ == 0U) {
    progress_ = RecoveryProgress::failed;
    return progress_;
  }
  const auto status = provider_.request_retransmission(identity_, range);
  if (status != RecoveryRequestStatus::accepted) {
    return request_snapshot();
  }
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  deadline_ns_ = now_ns > maximum - timeout_ns_ ? maximum : now_ns + timeout_ns_;
  progress_ = RecoveryProgress::replaying;
  return progress_;
}

RecoveryProgress RecoveryCoordinator::request_snapshot() noexcept {
  const auto status = provider_.request_snapshot(identity_);
  progress_ = status == RecoveryRequestStatus::accepted
                  ? RecoveryProgress::snapshot_requested
                  : RecoveryProgress::failed;
  return progress_;
}

RecoveryProgress RecoveryCoordinator::on_timer(const std::uint64_t now_ns) noexcept {
  if (progress_ == RecoveryProgress::replaying && now_ns >= deadline_ns_) {
    return request_snapshot();
  }
  return progress_;
}

ReceiveStatus RecoveryCoordinator::poll_snapshot(RecoverySnapshot& output) noexcept {
  if (progress_ != RecoveryProgress::snapshot_requested) {
    return ReceiveStatus::empty;
  }
  return provider_.poll_snapshot(output);
}

void RecoveryCoordinator::complete() noexcept {
  deadline_ns_ = 0U;
  progress_ = RecoveryProgress::idle;
}

RecoveryProgress RecoveryCoordinator::progress() const noexcept { return progress_; }

} // namespace aegis::market_data::feed
