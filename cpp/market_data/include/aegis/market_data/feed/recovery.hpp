#ifndef AEGIS_MARKET_DATA_FEED_RECOVERY_HPP
#define AEGIS_MARKET_DATA_FEED_RECOVERY_HPP

#include "aegis/market_data/feed/interfaces.hpp"

#include <cstdint>

namespace aegis::market_data::feed {

enum class RecoveryProgress : std::uint8_t {
  idle = 0,
  replaying,
  snapshot_requested,
  failed,
};

class RecoveryCoordinator {
public:
  RecoveryCoordinator(RecoveryProvider& provider, FeedIdentity identity,
                      std::uint64_t timeout_ns) noexcept;

  [[nodiscard]] RecoveryProgress begin_gap(const SequenceRange& range,
                                           std::uint64_t now_ns) noexcept;
  [[nodiscard]] RecoveryProgress request_snapshot() noexcept;
  [[nodiscard]] RecoveryProgress on_timer(std::uint64_t now_ns) noexcept;
  [[nodiscard]] ReceiveStatus poll_snapshot(RecoverySnapshot& output) noexcept;
  void complete() noexcept;

  [[nodiscard]] RecoveryProgress progress() const noexcept;

private:
  RecoveryProvider& provider_;
  FeedIdentity identity_;
  std::uint64_t timeout_ns_{};
  std::uint64_t deadline_ns_{};
  RecoveryProgress progress_{RecoveryProgress::idle};
};

} // namespace aegis::market_data::feed

#endif // AEGIS_MARKET_DATA_FEED_RECOVERY_HPP
