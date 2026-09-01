#ifndef AEGIS_RISK_TESTS_PORTFOLIO_TEST_SUPPORT_HPP
#define AEGIS_RISK_TESTS_PORTFOLIO_TEST_SUPPORT_HPP

#include "aegis/risk/portfolio_service.hpp"

#include <cstdint>

namespace aegis::risk::portfolio::test {

inline constexpr auto kSession = common::SessionId{1U, 1U};
inline constexpr auto kNextSession = common::SessionId{1U, 2U};
inline constexpr auto kConfiguration = common::ConfigurationVersion{2U, 1U};
inline constexpr auto kAccount = common::AccountId{3U, 1U};
inline constexpr auto kStrategy = common::StrategyId{4U, 1U};
inline constexpr auto kVenue = common::VenueId{5U, 1U};
inline constexpr auto kInstrument = common::InstrumentId{6U, 1U};
inline constexpr auto kOrder = common::OrderId{7U, 1U};
inline constexpr std::uint64_t kStart = 1'000'000U;

[[nodiscard]] inline PortfolioConfiguration
configuration(const bool require_drop_copy = false) noexcept {
  PortfolioConfiguration value;
  value.configuration_version = kConfiguration;
  value.session_id = kSession;
  value.accounts[0U] = kAccount;
  value.strategies[0U] = kStrategy;
  value.instruments[0U] = {.instrument_id = kInstrument,
                           .tick_value_currency_nanos = 100U,
                           .sector_index = 0U,
                           .beta_ppm = 1'200'000,
                           .liquidity_weight_ppm = 1'500'000U,
                           .event_weight_ppm = 2'000'000U,
                           .options_gamma_stress_currency_nanos = -7'000};
  value.account_count = 1U;
  value.strategy_count = 1U;
  value.instrument_count = 1U;
  value.sector_count = 1U;
  value.require_drop_copy_confirmation = require_drop_copy;
  value.stable_hash = stable_configuration_hash(value);
  return value;
}

[[nodiscard]] inline PortfolioEvent finalize(PortfolioEvent value) noexcept {
  value.stable_hash = stable_event_hash(value);
  return value;
}

// Test fixture builders intentionally use compact positional scalar arguments.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] inline PortfolioEvent mark(const std::uint64_t ordinal,
                                         const std::int64_t price = 100) noexcept {
  PortfolioEvent value;
  value.event_id = {10U, ordinal};
  value.session_id = kSession;
  value.configuration_version = kConfiguration;
  value.instrument_id = kInstrument;
  value.kind = PortfolioEventKind::mark;
  value.price_ticks = price;
  value.process_monotonic_time_ns = kStart + ordinal;
  return finalize(value);
}

[[nodiscard]] inline PortfolioEvent
order(const std::uint64_t ordinal,
      const OrderLifecycleState state = OrderLifecycleState::open,
      const std::uint64_t cumulative = 0U,
      const std::uint64_t remaining = 100U) noexcept {
  PortfolioEvent value;
  value.event_id = {11U, ordinal};
  value.session_id = kSession;
  value.configuration_version = kConfiguration;
  value.order_id = kOrder;
  value.account_id = kAccount;
  value.strategy_id = kStrategy;
  value.venue_id = kVenue;
  value.instrument_id = kInstrument;
  value.kind = PortfolioEventKind::order;
  value.side = Side::buy;
  value.order_state = state;
  value.price_ticks = 100;
  value.quantity_units = 100U;
  value.cumulative_fill_quantity_units = cumulative;
  value.remaining_quantity_units = remaining;
  value.process_monotonic_time_ns = kStart + ordinal;
  return finalize(value);
}

[[nodiscard]] inline PortfolioEvent
fill(const std::uint64_t event_ordinal, const std::uint64_t logical_ordinal,
     const Side side = Side::buy, const std::int64_t price = 100,
     const std::uint64_t quantity = 10U, const std::int64_t fee = 0,
     const FillSource source = FillSource::primary) noexcept {
  PortfolioEvent value;
  value.event_id = {12U, event_ordinal};
  value.logical_fill_id = {13U, logical_ordinal};
  value.session_id = kSession;
  value.configuration_version = kConfiguration;
  value.order_id = kOrder;
  value.account_id = kAccount;
  value.strategy_id = kStrategy;
  value.venue_id = kVenue;
  value.instrument_id = kInstrument;
  value.kind = PortfolioEventKind::fill;
  value.side = side;
  value.fill_source = source;
  value.price_ticks = price;
  value.quantity_units = quantity;
  value.fee_currency_nanos = fee;
  value.process_monotonic_time_ns = kStart + event_ordinal;
  return finalize(value);
}

[[nodiscard]] inline PortfolioEvent correction(const std::uint64_t event_ordinal,
                                               const std::uint64_t logical_ordinal,
                                               const std::int64_t price,
                                               const std::uint64_t quantity) noexcept {
  auto value = fill(event_ordinal, logical_ordinal, Side::buy, price, quantity);
  value.kind = PortfolioEventKind::fill_correction;
  value.target_fill_id = value.logical_fill_id;
  return finalize(value);
}

[[nodiscard]] inline PortfolioEvent bust(const std::uint64_t event_ordinal,
                                         const std::uint64_t logical_ordinal) noexcept {
  PortfolioEvent value;
  value.event_id = {14U, event_ordinal};
  value.target_fill_id = {13U, logical_ordinal};
  value.session_id = kSession;
  value.configuration_version = kConfiguration;
  value.kind = PortfolioEventKind::fill_bust;
  value.process_monotonic_time_ns = kStart + event_ordinal;
  return finalize(value);
}
// NOLINTEND(bugprone-easily-swappable-parameters)

} // namespace aegis::risk::portfolio::test

#endif // AEGIS_RISK_TESTS_PORTFOLIO_TEST_SUPPORT_HPP
