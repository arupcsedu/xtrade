#include "aegis/market_data/synthetic/config.hpp"

#include "aegis/market_data/synthetic/hash.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace aegis::market_data::synthetic {
namespace {

[[nodiscard]] constexpr bool valid_scenario(const Scenario scenario) noexcept {
  return scenario >= Scenario::normal &&
         scenario <= Scenario::hidden_liquidity_replenishment;
}

[[nodiscard]] constexpr bool valid_feed_mode(const FeedMode mode) noexcept {
  return mode == FeedMode::order_level || mode == FeedMode::price_level;
}

[[nodiscard]] constexpr bool
valid_distribution(const OrderSizeDistribution distribution) noexcept {
  return distribution == OrderSizeDistribution::fixed ||
         distribution == OrderSizeDistribution::uniform ||
         distribution == OrderSizeDistribution::two_point;
}

[[nodiscard]] ConfigError validate_dimensions(const GeneratorConfig& config) noexcept {
  if (config.event_count == 0U) {
    return ConfigError::invalid_event_count;
  }
  if (config.venue_count == 0U || config.venue_count > kMaximumVenues) {
    return ConfigError::invalid_venue_count;
  }
  if (config.instrument_count == 0U || config.instrument_count > kMaximumInstruments) {
    return ConfigError::invalid_instrument_count;
  }
  const auto scenario_minimum =
      (2U * static_cast<std::uint64_t>(config.instrument_count)) + 6U;
  if (config.scenario != Scenario::normal && config.event_count < scenario_minimum) {
    return ConfigError::invalid_event_count;
  }
  return ConfigError::none;
}

[[nodiscard]] ConfigError validate_instruments(const GeneratorConfig& config) noexcept {
  const auto minimum_mid = static_cast<std::int64_t>(config.wide_spread_ticks) + 2;
  for (std::size_t index = 0; index < config.instrument_count; ++index) {
    const auto& instrument = config.instruments[index];
    if (instrument.venue_number == 0U || instrument.venue_number > config.venue_count ||
        instrument.channel_number == 0U || instrument.instrument_number == 0U ||
        !valid_feed_mode(instrument.feed_mode)) {
      return ConfigError::invalid_instrument;
    }
    if (instrument.tick_value_currency_nanos <= 0) {
      return ConfigError::invalid_tick_size;
    }
    if (instrument.initial_mid_price_ticks <= minimum_mid) {
      return ConfigError::invalid_instrument;
    }
    for (std::size_t prior = 0; prior < index; ++prior) {
      if (config.instruments[prior].venue_number == instrument.venue_number &&
          config.instruments[prior].instrument_number == instrument.instrument_number) {
        return ConfigError::duplicate_instrument;
      }
    }
  }
  return ConfigError::none;
}

[[nodiscard]] ConfigError
validate_session_and_rates(const GeneratorConfig& config) noexcept {
  if (config.session_start_exchange_time_ns <= 0 ||
      config.session_end_exchange_time_ns <= config.session_start_exchange_time_ns ||
      config.process_monotonic_start_ns == 0U) {
    return ConfigError::invalid_session;
  }
  if (config.normal_message_rate_per_second == 0U ||
      config.normal_message_rate_per_second > 1'000'000'000U ||
      config.burst_message_rate_per_second == 0U ||
      config.burst_message_rate_per_second > 1'000'000'000U ||
      config.burst_message_rate_per_second < config.normal_message_rate_per_second) {
    return ConfigError::invalid_message_rate;
  }
  return ConfigError::none;
}

[[nodiscard]] ConfigError
validate_market_parameters(const GeneratorConfig& config) noexcept {
  if (config.volatility_ppm > kPartsPerMillion) {
    return ConfigError::invalid_volatility;
  }
  if (config.tight_spread_ticks == 0U ||
      config.tight_spread_ticks > config.normal_spread_ticks ||
      config.normal_spread_ticks > config.wide_spread_ticks ||
      config.spread_regime_period_events == 0U ||
      config.wide_spread_ticks >
          static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
    return ConfigError::invalid_spread;
  }
  if (!valid_distribution(config.order_size_distribution)) {
    return ConfigError::invalid_distribution;
  }
  if (config.minimum_order_quantity == 0U ||
      config.maximum_order_quantity < config.minimum_order_quantity) {
    return ConfigError::invalid_order_size;
  }
  if (config.cancellation_intensity_ppm > kPartsPerMillion ||
      config.market_order_intensity_ppm > kPartsPerMillion ||
      config.cancellation_intensity_ppm >
          kPartsPerMillion - config.market_order_intensity_ppm) {
    return ConfigError::invalid_intensity;
  }
  return ConfigError::none;
}

[[nodiscard]] ConfigError
validate_auction_and_capacity(const GeneratorConfig& config) noexcept {
  const bool no_auction =
      config.auction_start_event == 0U && config.auction_end_event == 0U;
  const bool valid_auction = config.auction_start_event > 0U &&
                             config.auction_start_event < config.auction_end_event &&
                             config.auction_end_event <= config.event_count;
  if (!no_auction && !valid_auction) {
    return ConfigError::invalid_auction_period;
  }
  if (config.stale_threshold_ns == 0U ||
      config.stale_threshold_ns >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return ConfigError::invalid_stale_threshold;
  }
  if (config.maximum_active_orders_per_instrument == 0U ||
      config.maximum_active_orders_per_instrument > kMaximumActiveOrdersPerInstrument) {
    return ConfigError::invalid_capacity;
  }
  if (config.maximum_order_quantity > std::numeric_limits<std::uint64_t>::max() /
                                          config.maximum_active_orders_per_instrument) {
    return ConfigError::invalid_order_size;
  }
  return valid_scenario(config.scenario) ? ConfigError::none
                                         : ConfigError::invalid_scenario;
}

[[nodiscard]] ConfigError
validate_numeric_ranges(const GeneratorConfig& config) noexcept {
  const auto interval_ns = std::max<std::uint64_t>(
      1U, 1'000'000'000U / config.normal_message_rate_per_second);
  const auto exchange_room = static_cast<std::uint64_t>(
      std::numeric_limits<std::int64_t>::max() - config.session_start_exchange_time_ns);
  if (config.event_count - 1U > exchange_room / interval_ns) {
    return ConfigError::count_overflow;
  }
  const auto base_duration = (config.event_count - 1U) * interval_ns;
  const auto stale_extension =
      config.scenario == Scenario::stale_feed ? config.stale_threshold_ns : 0U;
  if (base_duration > std::numeric_limits<std::uint64_t>::max() - stale_extension) {
    return ConfigError::count_overflow;
  }
  const auto total_duration = base_duration + stale_extension;
  if (total_duration > std::numeric_limits<std::uint64_t>::max() -
                           config.process_monotonic_start_ns ||
      total_duration > exchange_room) {
    return ConfigError::count_overflow;
  }
  for (std::size_t index = 0; index < config.instrument_count; ++index) {
    const auto mid = config.instruments[index].initial_mid_price_ticks;
    if (mid > std::numeric_limits<std::int64_t>::max() - 20) {
      return ConfigError::count_overflow;
    }
    const auto upward_room =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - mid - 20);
    if (config.event_count > upward_room) {
      return ConfigError::count_overflow;
    }
    if (config.scenario == Scenario::macro_shock && mid <= 12) {
      return ConfigError::invalid_instrument;
    }
  }
  return expected_physical_packet_count(config).has_value()
             ? ConfigError::none
             : ConfigError::count_overflow;
}

} // namespace

GeneratorConfig make_default_config() noexcept {
  GeneratorConfig config{};
  for (std::size_t index = 0; index < config.instrument_count; ++index) {
    const auto venue = static_cast<std::uint32_t>((index % config.venue_count) + 1U);
    config.instruments[index] = {
        .venue_number = venue,
        .channel_number = venue,
        .instrument_number = static_cast<std::uint32_t>(index + 1U),
        .tick_value_currency_nanos = 10'000'000,
        .initial_mid_price_ticks = 10'000 + static_cast<std::int64_t>(index * 100U),
        .feed_mode = (index % 2U) == 0U ? FeedMode::order_level : FeedMode::price_level,
    };
  }
  return config;
}

ConfigError validate_config(const GeneratorConfig& config) noexcept {
  auto error = validate_dimensions(config);
  if (error != ConfigError::none) {
    return error;
  }
  error = validate_instruments(config);
  if (error != ConfigError::none) {
    return error;
  }
  error = validate_session_and_rates(config);
  if (error != ConfigError::none) {
    return error;
  }
  error = validate_market_parameters(config);
  if (error != ConfigError::none) {
    return error;
  }
  error = validate_auction_and_capacity(config);
  if (error != ConfigError::none) {
    return error;
  }
  return validate_numeric_ranges(config);
}

std::uint64_t config_hash(const GeneratorConfig& config) noexcept {
  StableHash64 hash;
  hash.add_u64(config.seed);
  hash.add_u64(config.event_count);
  hash.add_u32(config.venue_count);
  hash.add_u32(config.instrument_count);
  for (std::size_t index = 0; index < config.instrument_count; ++index) {
    const auto& instrument = config.instruments[index];
    hash.add_u32(instrument.venue_number);
    hash.add_u32(instrument.channel_number);
    hash.add_u32(instrument.instrument_number);
    hash.add_i64(instrument.tick_value_currency_nanos);
    hash.add_i64(instrument.initial_mid_price_ticks);
    hash.add_byte(static_cast<std::uint8_t>(instrument.feed_mode));
  }
  hash.add_i64(config.session_start_exchange_time_ns);
  hash.add_i64(config.session_end_exchange_time_ns);
  hash.add_u64(config.process_monotonic_start_ns);
  hash.add_u64(config.normal_message_rate_per_second);
  hash.add_u64(config.burst_message_rate_per_second);
  hash.add_u32(config.volatility_ppm);
  hash.add_u32(config.tight_spread_ticks);
  hash.add_u32(config.normal_spread_ticks);
  hash.add_u32(config.wide_spread_ticks);
  hash.add_u64(config.spread_regime_period_events);
  hash.add_byte(static_cast<std::uint8_t>(config.order_size_distribution));
  hash.add_u64(config.minimum_order_quantity);
  hash.add_u64(config.maximum_order_quantity);
  hash.add_u32(config.cancellation_intensity_ppm);
  hash.add_u32(config.market_order_intensity_ppm);
  hash.add_u64(config.auction_start_event);
  hash.add_u64(config.auction_end_event);
  hash.add_u64(config.stale_threshold_ns);
  hash.add_u32(config.maximum_active_orders_per_instrument);
  hash.add_u16(static_cast<std::uint16_t>(config.scenario));
  return hash.value();
}

std::optional<std::uint64_t>
expected_physical_packet_count(const GeneratorConfig& config) noexcept {
  if (config.scenario == Scenario::duplicate_packet) {
    if (config.event_count == std::numeric_limits<std::uint64_t>::max()) {
      return std::nullopt;
    }
    return config.event_count + 1U;
  }
  if (config.scenario == Scenario::missing_sequence) {
    if (config.event_count == 0U) {
      return std::nullopt;
    }
    return config.event_count - 1U;
  }
  return config.event_count;
}

std::uint64_t scenario_injection_ordinal(const GeneratorConfig& config) noexcept {
  const auto bootstrap_end =
      (2U * static_cast<std::uint64_t>(config.instrument_count)) + 1U;
  const auto upper = config.event_count > 4U ? config.event_count - 4U : 1U;
  const auto preferred = std::max(config.event_count / 2U, bootstrap_end);
  return std::min(preferred, upper);
}

std::string_view scenario_name(const Scenario scenario) noexcept {
  switch (scenario) {
  case Scenario::normal:
    return "normal";
  case Scenario::high_message_rate_burst:
    return "high-message-rate-burst";
  case Scenario::crossed_book_fault:
    return "crossed-book-fault";
  case Scenario::duplicate_packet:
    return "duplicate-packet";
  case Scenario::missing_sequence:
    return "missing-sequence";
  case Scenario::out_of_order_packet:
    return "out-of-order-packet";
  case Scenario::stale_feed:
    return "stale-feed";
  case Scenario::trading_halt:
    return "trading-halt";
  case Scenario::reopening_auction:
    return "reopening-auction";
  case Scenario::earnings_shock:
    return "earnings-shock";
  case Scenario::macro_shock:
    return "macro-shock";
  case Scenario::index_rebalance:
    return "index-rebalance";
  case Scenario::hidden_liquidity_replenishment:
    return "hidden-liquidity-replenishment";
  }
  return "invalid";
}

std::optional<Scenario> parse_scenario(const std::string_view value) noexcept {
  for (auto raw = static_cast<std::uint16_t>(Scenario::normal);
       raw <= static_cast<std::uint16_t>(Scenario::hidden_liquidity_replenishment);
       ++raw) {
    const auto scenario = static_cast<Scenario>(raw);
    if (scenario_name(scenario) == value) {
      return scenario;
    }
  }
  return std::nullopt;
}

std::string_view config_error_name(const ConfigError error) noexcept {
  switch (error) {
  case ConfigError::none:
    return "none";
  case ConfigError::invalid_seed:
    return "invalid-seed";
  case ConfigError::invalid_event_count:
    return "invalid-event-count";
  case ConfigError::invalid_venue_count:
    return "invalid-venue-count";
  case ConfigError::invalid_instrument_count:
    return "invalid-instrument-count";
  case ConfigError::invalid_instrument:
    return "invalid-instrument";
  case ConfigError::duplicate_instrument:
    return "duplicate-instrument";
  case ConfigError::invalid_tick_size:
    return "invalid-tick-size";
  case ConfigError::invalid_session:
    return "invalid-session";
  case ConfigError::invalid_message_rate:
    return "invalid-message-rate";
  case ConfigError::invalid_volatility:
    return "invalid-volatility";
  case ConfigError::invalid_spread:
    return "invalid-spread";
  case ConfigError::invalid_distribution:
    return "invalid-distribution";
  case ConfigError::invalid_order_size:
    return "invalid-order-size";
  case ConfigError::invalid_intensity:
    return "invalid-intensity";
  case ConfigError::invalid_auction_period:
    return "invalid-auction-period";
  case ConfigError::invalid_stale_threshold:
    return "invalid-stale-threshold";
  case ConfigError::invalid_capacity:
    return "invalid-capacity";
  case ConfigError::invalid_scenario:
    return "invalid-scenario";
  case ConfigError::count_overflow:
    return "count-overflow";
  }
  return "invalid-error";
}

} // namespace aegis::market_data::synthetic
