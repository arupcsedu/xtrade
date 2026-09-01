#include "aegis/models/microstructure_dataset.hpp"

#include "aegis/market_data/synthetic/event.hpp"
#include "aegis/market_data/synthetic/generator.hpp"
#include "aegis/market_data/synthetic/hash.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <ostream>
#include <utility>

namespace aegis::models {
namespace {

namespace synthetic = market_data::synthetic;

struct InstrumentDatasetState {
  std::int64_t best_bid_ticks{};
  std::int64_t best_ask_ticks{};
  std::int64_t previous_price_ticks{};
  std::uint64_t bid_quantity_units{};
  std::uint64_t ask_quantity_units{};
  std::uint64_t add_count{};
  std::uint64_t cancel_count{};
  std::uint64_t execute_count{};
  std::uint64_t trade_count{};
  std::uint64_t depleted_quantity_units{};
};

[[nodiscard]] std::int64_t signed_quantity(const synthetic::SyntheticEvent& event) {
  const auto bounded = std::min<std::uint64_t>(
      event.quantity_units,
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
  const auto quantity = static_cast<std::int64_t>(bounded);
  return event.side == synthetic::Side::ask ? -quantity : quantity;
}

void update_state(const synthetic::SyntheticEvent& event,
                  InstrumentDatasetState& state) noexcept {
  auto& side_quantity = event.side == synthetic::Side::ask ? state.ask_quantity_units
                                                           : state.bid_quantity_units;
  auto& side_price =
      event.side == synthetic::Side::ask ? state.best_ask_ticks : state.best_bid_ticks;
  switch (event.type) {
  case synthetic::NativeMessageType::add_order:
    ++state.add_count;
    side_quantity =
        std::min<std::uint64_t>(10'000'000U, side_quantity + event.quantity_units);
    side_price = event.price_ticks;
    break;
  case synthetic::NativeMessageType::cancel_order:
    ++state.cancel_count;
    state.depleted_quantity_units = std::min<std::uint64_t>(
        10'000'000U, state.depleted_quantity_units + event.quantity_units);
    side_quantity -= std::min(side_quantity, event.quantity_units);
    break;
  case synthetic::NativeMessageType::modify_order:
    ++state.cancel_count;
    state.depleted_quantity_units = std::min<std::uint64_t>(
        10'000'000U, state.depleted_quantity_units + event.quantity_units);
    side_quantity = event.quantity_units;
    side_price = event.price_ticks;
    break;
  case synthetic::NativeMessageType::price_level:
    if (event.action == synthetic::BookAction::delete_level) {
      ++state.cancel_count;
      state.depleted_quantity_units = std::min<std::uint64_t>(
          10'000'000U, state.depleted_quantity_units + side_quantity);
      side_quantity = 0U;
    } else {
      ++state.add_count;
      side_quantity = event.level_quantity_units;
      side_price = event.price_ticks;
    }
    break;
  case synthetic::NativeMessageType::trade:
    ++state.execute_count;
    ++state.trade_count;
    state.depleted_quantity_units = std::min<std::uint64_t>(
        10'000'000U, state.depleted_quantity_units + event.quantity_units);
    break;
  case synthetic::NativeMessageType::quote:
    ++state.add_count;
    break;
  case synthetic::NativeMessageType::auction_imbalance:
  case synthetic::NativeMessageType::trading_status:
    break;
  }
}

[[nodiscard]] std::int64_t clamp_i64(const std::uint64_t value) noexcept {
  return static_cast<std::int64_t>(std::min<std::uint64_t>(value, 10'000'000U));
}

void add_hash_value(synthetic::StableHash64& hash, const std::int64_t value) noexcept {
  hash.add_i64(value);
}

} // namespace

// This offline exporter keeps every derived field visible in one audit-friendly
// row construction. It is not a hot-path function.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool write_synthetic_microstructure_dataset(
    const synthetic::GeneratorConfig& config, std::ostream& output,
    MicrostructureDatasetSummary& summary) noexcept {
  try {
    auto generator_value = synthetic::SyntheticExchangeGenerator::create(config);
    if (!generator_value.has_value()) {
      return false;
    }
    auto generator = std::move(*generator_value);
    std::array<InstrumentDatasetState, synthetic::kMaximumInstruments> states{};
    synthetic::StableHash64 hash;
    hash.add_u64(config.seed);
    hash.add_u64(config.event_count);
    output << "source_event_hash,top_level_imbalance_ppm,spread_ticks,"
              "order_flow_imbalance_units,signed_trade_imbalance_ppm,"
              "add_rate_millihertz,cancel_rate_millihertz,execute_rate_millihertz,"
              "queue_depletion_rate_units_per_second,rolling_return_ppm,"
              "realized_volatility_ppm,trade_intensity_millihertz,"
              "quote_intensity_millihertz,data_quality_code,session_progress_ppm,"
              "quantity_ahead_units,order_quantity_units,order_age_ns,"
              "price_distance_ticks,venue_number,fee_rate_ppm,"
              "label_mid_price_up,label_spread_widening,label_queue_depletion,"
              "label_passive_fill,label_adverse_selection,target_cost_ppm,"
              "target_impact_ppm\n";

    synthetic::SyntheticEvent event{};
    std::uint64_t rows{};
    while (true) {
      const auto generated = generator.next(event);
      if (generated.error == synthetic::GenerationError::complete) {
        break;
      }
      if (!generated.ok() || event.instrument_number == 0U ||
          event.instrument_number > states.size()) {
        return false;
      }
      auto& state = states[event.instrument_number - 1U];
      const auto prior_price = state.previous_price_ticks;
      update_state(event, state);
      if (event.price_ticks != 0) {
        state.previous_price_ticks = event.price_ticks;
      }
      const auto bid = clamp_i64(state.bid_quantity_units);
      const auto ask = clamp_i64(state.ask_quantity_units);
      const auto total = bid + ask;
      const auto imbalance = total == 0 ? 0 : ((bid - ask) * 1'000'000LL) / total;
      const auto spread =
          state.best_bid_ticks > 0 && state.best_ask_ticks > 0
              ? std::max<std::int64_t>(0, state.best_ask_ticks - state.best_bid_ticks)
              : 0;
      const auto signed_flow = signed_quantity(event);
      const auto signed_trade =
          event.type == synthetic::NativeMessageType::trade
              ? std::clamp(signed_flow * 1'000LL, -1'000'000LL, 1'000'000LL)
              : 0;
      const auto add_rate = clamp_i64(state.add_count * 1'000U);
      const auto cancel_rate = clamp_i64(state.cancel_count * 1'000U);
      const auto execute_rate = clamp_i64(state.execute_count * 1'000U);
      const auto depletion_rate = clamp_i64(state.depleted_quantity_units);
      const auto initial_price =
          config.instruments[event.instrument_number - 1U].initial_mid_price_ticks;
      const auto current_price =
          event.price_ticks == 0 ? initial_price : event.price_ticks;
      const auto rolling_return =
          ((current_price - initial_price) * 1'000'000LL) / initial_price;
      const auto realized_volatility =
          prior_price == 0
              ? 0
              : std::min<std::int64_t>(
                    1'000'000LL, std::llabs(current_price - prior_price) * 10'000LL);
      const auto trade_intensity = clamp_i64(state.trade_count * 1'000U);
      const auto quote_intensity = add_rate;
      const auto data_quality = static_cast<std::int64_t>(event.data_quality);
      const auto session_progress = static_cast<std::int64_t>(
          (event.global_ordinal * 1'000'000U) / config.event_count);
      const auto quantity_ahead =
          clamp_i64(event.level_quantity_units == 0U ? event.quantity_units
                                                     : event.level_quantity_units);
      const auto order_quantity = clamp_i64(event.quantity_units);
      const auto order_age =
          static_cast<std::int64_t>((event.global_ordinal % 1'000U) * 1'000U);
      const auto midpoint = state.best_bid_ticks > 0 && state.best_ask_ticks > 0
                                ? (state.best_bid_ticks + state.best_ask_ticks) / 2
                                : initial_price;
      const auto distance = std::llabs(current_price - midpoint);
      constexpr std::int64_t kFeeRatePpm = 30;

      const auto mid_up = current_price >= prior_price && prior_price != 0 ? 1 : 0;
      const auto spread_widening =
          std::cmp_greater(spread, config.normal_spread_ticks) ? 1 : 0;
      const auto queue_depletion =
          event.type == synthetic::NativeMessageType::cancel_order ||
                  event.type == synthetic::NativeMessageType::modify_order ||
                  event.type == synthetic::NativeMessageType::trade
              ? 1
              : 0;
      const auto passive_fill =
          event.type == synthetic::NativeMessageType::trade ? 1 : 0;
      const auto adverse_selection = passive_fill != 0 && signed_trade < 0 ? 1 : 0;
      const auto target_cost = std::min<std::int64_t>(
          1'000'000LL, kFeeRatePpm + (spread * 100LL) + (distance * 25LL) +
                           (order_quantity / 10LL) + (realized_volatility / 100LL));
      const auto target_impact = std::min<std::int64_t>(
          1'000'000LL,
          (order_quantity / 5LL) + (trade_intensity / 100LL) + (distance * 20LL));

      const std::array<std::int64_t, 27> values{
          static_cast<std::int64_t>(event.event_hash),
          imbalance,
          spread,
          signed_flow,
          signed_trade,
          add_rate,
          cancel_rate,
          execute_rate,
          depletion_rate,
          rolling_return,
          realized_volatility,
          trade_intensity,
          quote_intensity,
          data_quality,
          session_progress,
          quantity_ahead,
          order_quantity,
          order_age,
          distance,
          static_cast<std::int64_t>(event.venue_number),
          kFeeRatePpm,
          mid_up,
          spread_widening,
          queue_depletion,
          passive_fill,
          adverse_selection,
          target_cost,
      };
      for (const auto value : values) {
        add_hash_value(hash, value);
        output << value << ',';
      }
      add_hash_value(hash, target_impact);
      output << target_impact << '\n';
      ++rows;
    }
    output.flush();
    if (!output.good()) {
      return false;
    }
    summary = {.seed = config.seed,
               .source_event_count = generator.events_generated(),
               .row_count = rows,
               .stable_hash = hash.value()};
    return rows == config.event_count;
  } catch (...) {
    return false;
  }
}

} // namespace aegis::models
