#ifndef AEGIS_FEATURES_TYPES_HPP
#define AEGIS_FEATURES_TYPES_HPP

#include "aegis/common/identifiers.hpp"
#include "aegis/market_data/synthetic/event.hpp"
#include "aegis/order_book/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace aegis::features {

inline constexpr std::size_t kMaximumFeatureVenues = 8U;
inline constexpr std::size_t kMaximumFeatureDepth = 16U;
inline constexpr std::size_t kMaximumWindowSamples = 2'048U;
inline constexpr std::size_t kMaximumParityEvents = 256U;
inline constexpr std::int64_t kPartsPerMillion = 1'000'000;

enum class FeatureName : std::uint8_t {
  midpoint_half_ticks = 0,
  spread_ticks,
  relative_spread_ppm,
  microprice_ticks_ppm,
  top_level_imbalance_ppm,
  multi_level_weighted_imbalance_ppm,
  order_flow_imbalance_units,
  signed_trade_imbalance_ppm,
  add_rate_millihertz,
  cancel_rate_millihertz,
  execute_rate_millihertz,
  queue_depletion_rate_units_per_second,
  rolling_return_ppm,
  realized_volatility_ppm,
  volume_units,
  vwap_ticks_ppm,
  trade_intensity_millihertz,
  quote_intensity_millihertz,
  cross_venue_divergence_half_ticks,
  bid_leader_venue_number,
  ask_leader_venue_number,
  venue_leadership_share_ppm,
  replenishment_rate_millihertz,
  data_quality_code,
  data_age_ns,
  session_progress_ppm,
  count,
};

inline constexpr std::size_t kFeatureCount =
    static_cast<std::size_t>(FeatureName::count);

enum class FeatureValidity : std::uint8_t {
  missing = 1,
  warming = 2,
  valid = 3,
  degraded = 4,
  stale = 5,
  invalid = 6,
};

enum class EngineState : std::uint8_t {
  recovering = 1,
  warming = 2,
  ready = 3,
  degraded = 4,
  stale = 5,
  invalid = 6,
};

enum class FeatureError : std::uint8_t {
  none = 0,
  wrong_thread,
  invalid_configuration,
  invalid_identity,
  invalid_session,
  invalid_venue,
  invalid_event_id,
  invalid_ordinal,
  invalid_timestamp,
  invalid_data_quality,
  invalid_book,
  invalid_trade,
  crossed_market,
  numeric_overflow,
  window_capacity_exhausted,
  no_snapshot,
  publisher_backpressure,
  parity_capacity_exhausted,
  parity_mismatch,
};

// All values are integers with units encoded in FeatureName. A missing or
// invalid feature carries value zero; consumers must inspect validity first.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct FeatureValue {
  std::int64_t value{};
  std::uint64_t as_of_process_monotonic_time_ns{};
  FeatureValidity validity{FeatureValidity::missing};
};

struct VenueConfig {
  aegis::common::VenueId venue_id;
  std::uint32_t venue_number{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return venue_id.valid() && venue_number != 0U;
  }
};

struct FeatureEngineConfig {
  aegis::common::InstrumentId instrument_id;
  aegis::common::SessionId session_id;
  aegis::common::ConfigurationVersion feature_definition_version;
  std::array<VenueConfig, kMaximumFeatureVenues> venues{};
  std::uint32_t venue_count{};
  std::uint32_t depth_levels{5U};
  std::uint32_t window_capacity{1'024U};
  std::uint32_t minimum_book_events{2U};
  std::uint32_t minimum_trade_events{1U};
  std::uint64_t rolling_window_ns{1'000'000'000U};
  std::uint64_t stale_after_ns{500'000'000U};
  std::uint64_t replenishment_horizon_ns{5'000'000U};
  std::uint64_t minimum_replenishment_quantity_units{1U};
  std::uint64_t maximum_quantity_units{
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())};
  std::int64_t maximum_absolute_price_ticks{1'000'000'000LL};
  std::int64_t session_open_exchange_time_ns{};
  std::int64_t session_close_exchange_time_ns{};

  [[nodiscard]] bool valid() const noexcept;
};

struct BookFeatureEvent {
  aegis::common::GlobalEventId global_event_id;
  aegis::common::SessionId session_id;
  aegis::common::InstrumentId instrument_id;
  aegis::common::VenueId venue_id;
  std::uint32_t venue_number{};
  std::uint64_t global_ordinal{};
  std::int64_t exchange_event_time_ns{};
  std::uint64_t process_monotonic_time_ns{};
  order_book::BookOperation operation{order_book::BookOperation::observe_quote};
  market_data::synthetic::Side side{market_data::synthetic::Side::none};
  std::int64_t affected_price_ticks{};
  std::uint64_t affected_quantity_units{};
  order_book::DepthSnapshot depth;
  market_data::synthetic::DataQuality data_quality{
      market_data::synthetic::DataQuality::valid};
  bool top_changed{false};
  bool hidden_replenishment{false};
};

struct TradeFeatureEvent {
  aegis::common::GlobalEventId global_event_id;
  aegis::common::SessionId session_id;
  aegis::common::InstrumentId instrument_id;
  aegis::common::VenueId venue_id;
  std::uint32_t venue_number{};
  std::uint64_t global_ordinal{};
  std::int64_t exchange_event_time_ns{};
  std::uint64_t process_monotonic_time_ns{};
  market_data::synthetic::Side aggressor_side{market_data::synthetic::Side::none};
  std::int64_t price_ticks{};
  std::uint64_t quantity_units{};
  market_data::synthetic::DataQuality data_quality{
      market_data::synthetic::DataQuality::valid};
};

struct FeatureSnapshot {
  aegis::common::FeatureSnapshotId snapshot_id;
  aegis::common::InstrumentId instrument_id;
  aegis::common::SessionId session_id;
  aegis::common::ConfigurationVersion feature_definition_version;
  aegis::common::GlobalEventId source_first_global_event_id;
  aegis::common::GlobalEventId source_last_global_event_id;
  std::array<FeatureValue, kFeatureCount> values{};
  std::uint64_t source_first_ordinal{};
  std::uint64_t source_last_ordinal{};
  std::int64_t as_of_exchange_event_time_ns{};
  std::uint64_t created_process_monotonic_time_ns{};
  std::uint64_t stable_hash{};
  EngineState state{EngineState::recovering};

  [[nodiscard]] const FeatureValue& get(FeatureName name) const noexcept {
    return values[static_cast<std::size_t>(name)];
  }
};

struct FeatureResult {
  FeatureError error{FeatureError::none};
  EngineState state{EngineState::recovering};
  std::uint64_t accepted_event_count{};

  [[nodiscard]] constexpr bool accepted() const noexcept {
    return error == FeatureError::none;
  }
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] std::uint64_t
stable_snapshot_hash(const FeatureSnapshot& snapshot) noexcept;

static_assert(std::is_trivially_copyable_v<BookFeatureEvent>);
static_assert(std::is_trivially_copyable_v<TradeFeatureEvent>);
static_assert(std::is_trivially_copyable_v<FeatureSnapshot>);

} // namespace aegis::features

#endif // AEGIS_FEATURES_TYPES_HPP
