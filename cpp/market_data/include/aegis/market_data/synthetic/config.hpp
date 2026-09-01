#ifndef AEGIS_MARKET_DATA_SYNTHETIC_CONFIG_HPP
#define AEGIS_MARKET_DATA_SYNTHETIC_CONFIG_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace aegis::market_data::synthetic {

inline constexpr std::size_t kMaximumVenues = 8U;
inline constexpr std::size_t kMaximumInstruments = 64U;
inline constexpr std::size_t kMaximumBookDepth = 32U;
inline constexpr std::size_t kMaximumActiveOrdersPerInstrument = 256U;
inline constexpr std::uint32_t kPartsPerMillion = 1'000'000U;

enum class Scenario : std::uint8_t {
  normal = 0,
  high_message_rate_burst = 1,
  crossed_book_fault = 2,
  duplicate_packet = 3,
  missing_sequence = 4,
  out_of_order_packet = 5,
  stale_feed = 6,
  trading_halt = 7,
  reopening_auction = 8,
  earnings_shock = 9,
  macro_shock = 10,
  index_rebalance = 11,
  hidden_liquidity_replenishment = 12,
};

enum class FeedMode : std::uint8_t {
  order_level = 1,
  price_level = 2,
};

enum class OrderSizeDistribution : std::uint8_t {
  fixed = 1,
  uniform = 2,
  two_point = 3,
};

enum class ConfigError : std::uint8_t {
  none = 0,
  invalid_seed,
  invalid_event_count,
  invalid_venue_count,
  invalid_instrument_count,
  invalid_instrument,
  duplicate_instrument,
  invalid_tick_size,
  invalid_session,
  invalid_message_rate,
  invalid_volatility,
  invalid_spread,
  invalid_distribution,
  invalid_order_size,
  invalid_intensity,
  invalid_auction_period,
  invalid_stale_threshold,
  invalid_capacity,
  invalid_scenario,
  count_overflow,
};

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct InstrumentConfig {
  std::uint32_t venue_number{};
  std::uint32_t channel_number{};
  std::uint32_t instrument_number{};
  std::int64_t tick_value_currency_nanos{};
  std::int64_t initial_mid_price_ticks{};
  FeedMode feed_mode{FeedMode::order_level};
};

struct GeneratorConfig {
  std::uint64_t seed{20'260'829U};
  std::uint64_t event_count{10'000U};
  std::uint32_t venue_count{2U};
  std::uint32_t instrument_count{4U};
  std::array<InstrumentConfig, kMaximumInstruments> instruments{};
  std::int64_t session_start_exchange_time_ns{1'800'000'000'000'000'000LL};
  std::int64_t session_end_exchange_time_ns{1'800'021'600'000'000'000LL};
  std::uint64_t process_monotonic_start_ns{1'000'000U};
  std::uint64_t normal_message_rate_per_second{1'000'000U};
  std::uint64_t burst_message_rate_per_second{20'000'000U};
  std::uint32_t volatility_ppm{2'000U};
  std::uint32_t tight_spread_ticks{1U};
  std::uint32_t normal_spread_ticks{2U};
  std::uint32_t wide_spread_ticks{5U};
  std::uint64_t spread_regime_period_events{1'000U};
  OrderSizeDistribution order_size_distribution{OrderSizeDistribution::uniform};
  std::uint64_t minimum_order_quantity{1U};
  std::uint64_t maximum_order_quantity{1'000U};
  std::uint32_t cancellation_intensity_ppm{250'000U};
  std::uint32_t market_order_intensity_ppm{100'000U};
  std::uint64_t auction_start_event{};
  std::uint64_t auction_end_event{};
  std::uint64_t stale_threshold_ns{1'000'000'000U};
  std::uint32_t maximum_active_orders_per_instrument{128U};
  Scenario scenario{Scenario::normal};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] GeneratorConfig make_default_config() noexcept;
[[nodiscard]] ConfigError validate_config(const GeneratorConfig& config) noexcept;
[[nodiscard]] std::uint64_t config_hash(const GeneratorConfig& config) noexcept;
[[nodiscard]] std::optional<std::uint64_t>
expected_physical_packet_count(const GeneratorConfig& config) noexcept;
[[nodiscard]] std::uint64_t
scenario_injection_ordinal(const GeneratorConfig& config) noexcept;
[[nodiscard]] std::string_view scenario_name(Scenario scenario) noexcept;
[[nodiscard]] std::optional<Scenario> parse_scenario(std::string_view value) noexcept;
[[nodiscard]] std::string_view config_error_name(ConfigError error) noexcept;

} // namespace aegis::market_data::synthetic

#endif // AEGIS_MARKET_DATA_SYNTHETIC_CONFIG_HPP
