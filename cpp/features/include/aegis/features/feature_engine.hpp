#ifndef AEGIS_FEATURES_FEATURE_ENGINE_HPP
#define AEGIS_FEATURES_FEATURE_ENGINE_HPP

#include "aegis/features/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace aegis::features {

namespace detail {
struct FeatureStorage;
}

class FeatureEngine final {
public:
  explicit FeatureEngine(FeatureEngineConfig config);
  ~FeatureEngine();

  FeatureEngine(const FeatureEngine&) = delete;
  FeatureEngine& operator=(const FeatureEngine&) = delete;
  FeatureEngine(FeatureEngine&&) = delete;
  FeatureEngine& operator=(FeatureEngine&&) = delete;

  [[nodiscard]] FeatureResult on_book(const BookFeatureEvent& event) noexcept;
  [[nodiscard]] FeatureResult on_trade(const TradeFeatureEvent& event) noexcept;
  [[nodiscard]] FeatureResult
  mark_gap(std::uint64_t process_monotonic_time_ns) noexcept;
  [[nodiscard]] FeatureResult
  begin_recovery(std::uint64_t process_monotonic_time_ns) noexcept;
  [[nodiscard]] FeatureResult
  reset_session(aegis::common::SessionId session_id,
                std::int64_t session_open_exchange_time_ns,
                std::int64_t session_close_exchange_time_ns,
                std::uint64_t process_monotonic_time_ns) noexcept;

  [[nodiscard]] FeatureError make_snapshot(std::uint64_t process_monotonic_time_ns,
                                           FeatureSnapshot& output) const noexcept;
  [[nodiscard]] const FeatureEngineConfig& config() const noexcept;
  [[nodiscard]] EngineState state() const noexcept;
  [[nodiscard]] FeatureError last_error() const noexcept;
  [[nodiscard]] std::uint64_t accepted_event_count() const noexcept;
  [[nodiscard]] std::size_t memory_bytes_per_symbol() const noexcept;
  [[nodiscard]] bool owns_current_thread() const noexcept;

  // Used by deterministic replay harnesses. This performs no allocation and
  // restores construction-time configuration and RECOVERING state.
  [[nodiscard]] FeatureResult reset_for_replay() noexcept;

private:
  [[nodiscard]] FeatureResult fail(FeatureError error) noexcept;
  [[nodiscard]] FeatureResult result() const noexcept;
  [[nodiscard]] FeatureError validate_common(
      aegis::common::GlobalEventId event_id, aegis::common::SessionId session_id,
      aegis::common::InstrumentId instrument_id, aegis::common::VenueId venue_id,
      std::uint32_t venue_number, std::uint64_t global_ordinal,
      std::int64_t exchange_event_time_ns, std::uint64_t process_monotonic_time_ns,
      market_data::synthetic::DataQuality quality,
      std::size_t& venue_index) const noexcept;
  void clear_state(EngineState state, std::uint64_t process_monotonic_time_ns) noexcept;
  void refresh_state() noexcept;

  FeatureEngineConfig initial_config_;
  FeatureEngineConfig config_;
  std::thread::id owner_thread_;
  std::unique_ptr<detail::FeatureStorage> storage_;
  EngineState state_{EngineState::recovering};
  FeatureError last_error_{FeatureError::none};
  std::uint64_t accepted_event_count_{};
  std::uint64_t last_ordinal_{};
  std::uint64_t last_process_monotonic_time_ns_{};
  std::int64_t last_exchange_event_time_ns_{};
  aegis::common::GlobalEventId first_event_id_;
  aegis::common::GlobalEventId last_event_id_;
  std::uint64_t first_ordinal_{};
};

} // namespace aegis::features

#endif // AEGIS_FEATURES_FEATURE_ENGINE_HPP
