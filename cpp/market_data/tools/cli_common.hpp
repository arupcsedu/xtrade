#ifndef AEGIS_MARKET_DATA_SYNTHETIC_CLI_COMMON_HPP
#define AEGIS_MARKET_DATA_SYNTHETIC_CLI_COMMON_HPP

#include "aegis/market_data/synthetic/config.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace aegis::market_data::synthetic::cli {

enum class RequestedFeedMode : std::uint8_t { mixed = 0, order_level, price_level };

template <typename Integer>
[[nodiscard]] std::optional<Integer>
parse_integer(const std::string_view value) noexcept {
  Integer parsed{};
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  return parsed;
}

[[nodiscard]] inline FeedMode selected_feed_mode(const RequestedFeedMode requested_mode,
                                                 const std::size_t index) noexcept {
  switch (requested_mode) {
  case RequestedFeedMode::order_level:
    return FeedMode::order_level;
  case RequestedFeedMode::price_level:
    return FeedMode::price_level;
  case RequestedFeedMode::mixed:
    return (index % 2U) == 0U ? FeedMode::order_level : FeedMode::price_level;
  }
  return FeedMode::order_level;
}

[[nodiscard]] inline bool
configure_instruments(GeneratorConfig& config, const RequestedFeedMode requested_mode,
                      const std::int64_t tick_value) noexcept {
  if (config.venue_count == 0U || config.venue_count > kMaximumVenues ||
      config.instrument_count == 0U || config.instrument_count > kMaximumInstruments ||
      tick_value <= 0) {
    return false;
  }
  for (auto& instrument : config.instruments) {
    instrument = {};
  }
  for (std::size_t index = 0; index < config.instrument_count; ++index) {
    const auto venue = static_cast<std::uint32_t>((index % config.venue_count) + 1U);
    const auto mode = selected_feed_mode(requested_mode, index);
    config.instruments[index] = {
        .venue_number = venue,
        .channel_number = venue,
        .instrument_number = static_cast<std::uint32_t>(index + 1U),
        .tick_value_currency_nanos = tick_value,
        .initial_mid_price_ticks = 10'000 + static_cast<std::int64_t>(index * 100U),
        .feed_mode = mode,
    };
  }
  return true;
}

} // namespace aegis::market_data::synthetic::cli

#endif // AEGIS_MARKET_DATA_SYNTHETIC_CLI_COMMON_HPP
