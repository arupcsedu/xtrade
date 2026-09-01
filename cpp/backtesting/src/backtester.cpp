#include "aegis/backtesting/backtester.hpp"

#include "aegis/market_data/synthetic/book.hpp"
#include "aegis/market_data/synthetic/capture.hpp"
#include "aegis/market_data/synthetic/generator.hpp"
#include "aegis/market_data/synthetic/mock_protocol.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace aegis::backtesting {
namespace synthetic = market_data::synthetic;
namespace {

constexpr std::uint64_t kPartsPerMillion = 1'000'000U;
constexpr std::uint64_t kBacktestIdentifierNamespace = 0x4241'434b'5445'5354ULL;
constexpr double kProbabilityFloor = 1.0e-12;

[[nodiscard]] bool checked_add_u64(const std::uint64_t left, const std::uint64_t right,
                                   std::uint64_t& output) noexcept {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    return false;
  }
  output = left + right;
  return true;
}

[[nodiscard]] bool checked_add_i64(const std::int64_t left, const std::int64_t right,
                                   std::int64_t& output) noexcept {
  if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
    return false;
  }
  output = left + right;
  return true;
}

[[nodiscard]] bool checked_sub_i64(const std::int64_t left, const std::int64_t right,
                                   std::int64_t& output) noexcept {
  if (right == std::numeric_limits<std::int64_t>::min()) {
    if (left >= 0) {
      return false;
    }
    output = left - right;
    return true;
  }
  return checked_add_i64(left, -right, output);
}

[[nodiscard]] bool checked_mul_u64(const std::uint64_t left, const std::uint64_t right,
                                   std::uint64_t& output) noexcept {
  if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  output = left * right;
  return true;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] bool checked_signed_product(const std::int64_t value,
                                          const std::uint64_t quantity,
                                          std::int64_t& output) noexcept {
  const auto negative = value < 0;
  const auto magnitude = negative ? static_cast<std::uint64_t>(-(value + 1)) + 1U
                                  : static_cast<std::uint64_t>(value);
  std::uint64_t product{};
  if (!checked_mul_u64(magnitude, quantity, product)) {
    return false;
  }
  const auto negative_limit = std::uint64_t{1U} << 63U;
  if (negative) {
    if (product > negative_limit) {
      return false;
    }
    output = product == negative_limit ? std::numeric_limits<std::int64_t>::min()
                                       : -static_cast<std::int64_t>(product);
    return true;
  }
  if (product > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  output = static_cast<std::int64_t>(product);
  return true;
}

[[nodiscard]] bool checked_notional(const std::int64_t price_ticks,
                                    const std::int64_t tick_value,
                                    const std::uint64_t quantity,
                                    std::int64_t& output) noexcept {
  if (price_ticks <= 0 || tick_value <= 0) {
    return false;
  }
  std::uint64_t price_value{};
  if (!checked_mul_u64(static_cast<std::uint64_t>(price_ticks),
                       static_cast<std::uint64_t>(tick_value), price_value) ||
      !checked_mul_u64(price_value, quantity, price_value) ||
      price_value >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  output = static_cast<std::int64_t>(price_value);
  return true;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] std::uint64_t bounded_sample(const std::uint64_t seed,
                                           const std::uint64_t ordinal,
                                           const std::uint64_t upper) noexcept {
  synthetic::SplitMix64 random{seed ^ (ordinal * 0x9E37'79B9'7F4A'7C15ULL)};
  return random.bounded(upper);
}
// NOLINTEND(bugprone-easily-swappable-parameters)

[[nodiscard]] bool terminal(const OrderState state) noexcept {
  return state == OrderState::filled || state == OrderState::rejected ||
         state == OrderState::canceled || state == OrderState::expired;
}

[[nodiscard]] synthetic::Side market_side(const OrderSide side) noexcept {
  return side == OrderSide::buy ? synthetic::Side::bid : synthetic::Side::ask;
}

[[nodiscard]] int side_sign(const OrderSide side) noexcept {
  return side == OrderSide::buy ? 1 : -1;
}

[[nodiscard]] bool valid_forecast(const ForecastSignal& value,
                                  const std::uint64_t default_horizon) noexcept {
  if (!value.valid) {
    return true;
  }
  return value.probability_up_ppm <= kProbabilityScale &&
         value.return_p10_ppm <= value.return_p50_ppm &&
         value.return_p50_ppm <= value.return_p90_ppm &&
         (value.horizon_ns != 0U || default_horizon != 0U);
}

[[nodiscard]] EventPeriod period_for(const synthetic::SyntheticEvent& event,
                                     const synthetic::TradingStatus previous) noexcept {
  if ((event.message_flags & synthetic::kMessageFlagShock) != 0U) {
    return EventPeriod::shock;
  }
  if (event.type == synthetic::NativeMessageType::auction_imbalance ||
      event.status == synthetic::TradingStatus::auction) {
    return EventPeriod::auction;
  }
  if (event.status == synthetic::TradingStatus::halted ||
      previous == synthetic::TradingStatus::halted) {
    return event.status == synthetic::TradingStatus::halted ? EventPeriod::halted
                                                            : EventPeriod::reopening;
  }
  return EventPeriod::normal;
}

[[nodiscard]] std::size_t period_index(const EventPeriod value) noexcept {
  return static_cast<std::size_t>(value);
}

} // namespace

bool StrategyActionBuffer::push(const StrategyAction& action) noexcept {
  if (size_ >= actions_.size()) {
    return false;
  }
  actions_[size_++] = action;
  return true;
}

std::span<const StrategyAction> StrategyActionBuffer::actions() const noexcept {
  return {actions_.data(), size_};
}

void StrategyActionBuffer::clear() noexcept { size_ = 0U; }

class EventBacktester::Impl final {
public:
  struct StrategySlot {
    common::StrategyId strategy_id;
    IBacktestStrategy* strategy{};
  };

  struct MarketRuntime {
    InstrumentMapping mapping;
    synthetic::TradingStatus status{synthetic::TradingStatus::open};
    synthetic::TradingStatus previous_status{synthetic::TradingStatus::open};
    std::int64_t last_trade_ticks{};
    std::uint64_t consumed_bid_units{};
    std::uint64_t consumed_ask_units{};
    QueueModelQuality queue_quality{QueueModelQuality::order_level};
  };

  struct ExternalOrder {
    std::uint32_t venue_number{};
    std::uint32_t instrument_number{};
    std::uint64_t synthetic_order_id{};
    synthetic::Side side{synthetic::Side::none};
    std::int64_t price_ticks{};
    std::uint64_t remaining_quantity_units{};
    std::uint64_t priority_time_ns{};
    std::uint64_t priority_ordinal{};
    bool active{false};
    bool source_live{false};
    bool aggregate{false};
    bool inferred_hidden{false};
  };

  struct OrderMetadata {
    std::uint32_t venue_number{};
    std::uint32_t instrument_number{};
    std::uint64_t submission_ordinal{};
    std::uint64_t priority_ordinal{};
    std::uint64_t acknowledgement_due_ns{};
    std::uint64_t cancel_due_ns{};
    std::uint64_t shadow_queue_ahead_units{};
    std::uint64_t visible_aggressive_quantity_units{};
  };

  struct ForecastRuntime {
    common::StrategyId strategy_id;
    common::InstrumentId instrument_id;
    std::uint32_t probability_up_ppm{};
    std::int64_t p10_ppm{};
    std::int64_t p90_ppm{};
    std::int64_t initial_midpoint_x2{};
    std::uint64_t due_time_ns{};
    double realized_return_ppm{};
    bool resolved{false};
  };

  struct PositionRuntime {
    common::StrategyId strategy_id;
    common::InstrumentId instrument_id;
    std::int64_t quantity_units{};
    std::int64_t cash_currency_nanos{};
  };

  struct StrategyRuntime {
    common::StrategyId strategy_id;
    std::vector<std::int64_t> pnl_changes;
    std::array<EventPeriodMetrics, kEventPeriodCount> periods{};
    std::int64_t last_equity{};
    std::int64_t peak_equity{};
    std::int64_t maximum_drawdown{};
    std::uint64_t turnover{};
  };

  struct ChannelSequence {
    std::uint32_t venue_number{};
    std::uint32_t channel_number{};
    std::uint64_t last_sequence{};
  };

  explicit Impl(const BacktestConfig value) : config(value) {
    if (!valid_config(config)) {
      status = Status::invalid_configuration;
      report_value.status = status;
      return;
    }
    config_hash = configuration_hash(config);
    report_value.deterministic_seed = config.deterministic_seed;
    report_value.configuration_hash = config_hash;
    report_value.acknowledgement_latency = config.acknowledgement_latency;
    report_value.hidden_liquidity_probability_ppm =
        config.hidden_liquidity_probability_ppm;
    report_value.maximum_hidden_liquidity_units = config.maximum_hidden_liquidity_units;
    report_value.maker_fee_per_unit_currency_nanos =
        config.maker_fee_per_unit_currency_nanos;
    report_value.taker_fee_per_unit_currency_nanos =
        config.taker_fee_per_unit_currency_nanos;
    report_value.slippage_ticks = config.slippage_ticks;
    report_value.impact_ticks_per_million_units = config.impact_ticks_per_million_units;
    for (std::size_t index = 0U; index < config.instrument_count; ++index) {
      if (!books.register_instrument(config.instruments[index].venue_number,
                                     config.instruments[index].instrument_number)) {
        status = Status::invalid_configuration;
        report_value.status = status;
        return;
      }
      markets.push_back(MarketRuntime{.mapping = config.instruments[index]});
    }
    strategies.reserve(kMaximumBacktestStrategies);
    strategy_runtime.reserve(kMaximumBacktestStrategies);
    orders.reserve(static_cast<std::size_t>(config.maximum_orders));
    order_metadata.reserve(static_cast<std::size_t>(config.maximum_orders));
    external_orders.reserve(static_cast<std::size_t>(config.maximum_external_orders));
    fills.reserve(static_cast<std::size_t>(config.maximum_fills));
    forecasts.reserve(static_cast<std::size_t>(config.maximum_orders));
  }

  [[nodiscard]] std::size_t
  find_market(const std::uint32_t venue_number,
              const std::uint32_t instrument_number) const noexcept {
    for (std::size_t index = 0U; index < markets.size(); ++index) {
      if (markets[index].mapping.venue_number == venue_number &&
          markets[index].mapping.instrument_number == instrument_number) {
        return index;
      }
    }
    return markets.size();
  }

  [[nodiscard]] std::size_t
  find_market(const common::VenueId venue_id,
              const common::InstrumentId instrument_id) const noexcept {
    for (std::size_t index = 0U; index < markets.size(); ++index) {
      if (markets[index].mapping.venue_id == venue_id &&
          markets[index].mapping.instrument_id == instrument_id) {
        return index;
      }
    }
    return markets.size();
  }

  [[nodiscard]] std::size_t
  find_strategy_runtime(const common::StrategyId strategy_id) const noexcept {
    for (std::size_t index = 0U; index < strategy_runtime.size(); ++index) {
      if (strategy_runtime[index].strategy_id == strategy_id) {
        return index;
      }
    }
    return strategy_runtime.size();
  }

  [[nodiscard]] std::size_t
  find_position(const common::StrategyId strategy_id,
                const common::InstrumentId instrument_id) const noexcept {
    for (std::size_t index = 0U; index < positions.size(); ++index) {
      if (positions[index].strategy_id == strategy_id &&
          positions[index].instrument_id == instrument_id) {
        return index;
      }
    }
    return positions.size();
  }

  [[nodiscard]] std::size_t
  find_or_create_position(const common::StrategyId strategy_id,
                          const common::InstrumentId instrument_id) noexcept {
    const auto existing = find_position(strategy_id, instrument_id);
    if (existing != positions.size()) {
      return existing;
    }
    positions.push_back(
        PositionRuntime{.strategy_id = strategy_id, .instrument_id = instrument_id});
    return positions.size() - 1U;
  }

  [[nodiscard]] std::optional<std::int64_t>
  midpoint_x2(const std::size_t market_index) const noexcept {
    if (market_index >= markets.size()) {
      return std::nullopt;
    }
    const auto* book = books.find(markets[market_index].mapping.venue_number,
                                  markets[market_index].mapping.instrument_number);
    if (book == nullptr || !book->valid || book->crossed) {
      return std::nullopt;
    }
    const auto* bid = synthetic::best_bid(*book);
    const auto* ask = synthetic::best_ask(*book);
    std::int64_t result{};
    if (bid == nullptr || ask == nullptr ||
        !checked_add_i64(bid->price_ticks, ask->price_ticks, result)) {
      return std::nullopt;
    }
    return result;
  }

  [[nodiscard]] MarketView market_view(const std::size_t market_index,
                                       const synthetic::DataQuality quality,
                                       const std::uint64_t ordinal,
                                       const std::uint64_t now_ns) const noexcept {
    const auto& runtime = markets[market_index];
    const auto* book =
        books.find(runtime.mapping.venue_number, runtime.mapping.instrument_number);
    MarketView result{.venue_id = runtime.mapping.venue_id,
                      .instrument_id = runtime.mapping.instrument_id,
                      .venue_number = runtime.mapping.venue_number,
                      .instrument_number = runtime.mapping.instrument_number,
                      .status = runtime.status,
                      .data_quality = quality,
                      .source_ordinal = ordinal,
                      .process_monotonic_time_ns = now_ns,
                      .last_trade_ticks = runtime.last_trade_ticks,
                      .queue_model_quality = runtime.queue_quality};
    if (book == nullptr || !book->valid || book->crossed) {
      return result;
    }
    const auto* bid = synthetic::best_bid(*book);
    const auto* ask = synthetic::best_ask(*book);
    if (bid == nullptr || ask == nullptr) {
      return result;
    }
    result.best_bid_ticks = bid->price_ticks;
    result.best_ask_ticks = ask->price_ticks;
    result.best_bid_quantity_units = bid->quantity_units;
    result.best_ask_quantity_units = ask->quantity_units;
    result.valid_book = checked_add_i64(result.best_bid_ticks, result.best_ask_ticks,
                                        result.midpoint_x2);
    return result;
  }

  [[nodiscard]] std::uint64_t
  sample_latency(const std::uint64_t ordinal) const noexcept {
    const auto& distribution = config.acknowledgement_latency;
    if (distribution.kind == LatencyDistributionKind::fixed) {
      return distribution.minimum_ns;
    }
    if (distribution.kind == LatencyDistributionKind::two_point) {
      return bounded_sample(config.deterministic_seed ^ 0x1A7E'4EECU, ordinal,
                            kPartsPerMillion) < distribution.secondary_probability_ppm
                 ? distribution.secondary_ns
                 : distribution.minimum_ns;
    }
    const auto width = distribution.maximum_ns - distribution.minimum_ns;
    return distribution.minimum_ns +
           bounded_sample(config.deterministic_seed ^ 0xA11C'4C4BU, ordinal,
                          width + 1U);
  }

  [[nodiscard]] bool rejected(const std::uint64_t ordinal) const noexcept {
    return config.reject_probability_ppm != 0U &&
           bounded_sample(config.deterministic_seed ^ 0x0E1E'C7EDU, ordinal,
                          kPartsPerMillion) < config.reject_probability_ppm;
  }

  [[nodiscard]] std::uint64_t
  hidden_quantity(const std::uint64_t ordinal) const noexcept {
    if (config.hidden_liquidity_probability_ppm == 0U ||
        config.maximum_hidden_liquidity_units == 0U ||
        bounded_sample(config.deterministic_seed ^ 0x41DD'E001U, ordinal,
                       kPartsPerMillion) >= config.hidden_liquidity_probability_ppm) {
      return 0U;
    }
    return 1U + bounded_sample(config.deterministic_seed ^ 0x41DD'E002U, ordinal,
                               config.maximum_hidden_liquidity_units);
  }

  [[nodiscard]] std::size_t reusable_external_slot() const noexcept {
    for (std::size_t index = 0U; index < external_orders.size(); ++index) {
      if (!external_orders[index].active && !external_orders[index].source_live) {
        return index;
      }
    }
    return external_orders.size();
  }

  [[nodiscard]] Status add_external(ExternalOrder value) noexcept {
    const auto slot = reusable_external_slot();
    if (slot < external_orders.size()) {
      external_orders[slot] = value;
      return Status::ok;
    }
    if (external_orders.size() >= config.maximum_external_orders) {
      return Status::capacity_exhausted;
    }
    external_orders.push_back(value);
    return Status::ok;
  }

  [[nodiscard]] std::size_t
  find_external(const synthetic::SyntheticEvent& event) const noexcept {
    for (std::size_t index = 0U; index < external_orders.size(); ++index) {
      const auto& order = external_orders[index];
      if (order.source_live && !order.aggregate && !order.inferred_hidden &&
          order.venue_number == event.venue_number &&
          order.instrument_number == event.instrument_number &&
          order.synthetic_order_id == event.synthetic_order_id) {
        return index;
      }
    }
    return external_orders.size();
  }

  [[nodiscard]] std::size_t
  find_aggregate(const synthetic::SyntheticEvent& event) const noexcept {
    for (std::size_t index = 0U; index < external_orders.size(); ++index) {
      const auto& order = external_orders[index];
      if (order.active && order.aggregate && order.venue_number == event.venue_number &&
          order.instrument_number == event.instrument_number &&
          order.side == event.side && order.price_ticks == event.price_ticks) {
        return index;
      }
    }
    return external_orders.size();
  }

  [[nodiscard]] bool
  external_before_order(const ExternalOrder& external,
                        const std::size_t order_index) const noexcept {
    const auto& order = orders[order_index];
    const auto& metadata = order_metadata[order_index];
    return external.priority_time_ns < order.acknowledgement_time_ns ||
           (external.priority_time_ns == order.acknowledgement_time_ns &&
            external.priority_ordinal < metadata.priority_ordinal);
  }

  void record_cancellation_ahead(const ExternalOrder& external,
                                 const std::uint64_t canceled) noexcept {
    for (std::size_t index = 0U; index < orders.size(); ++index) {
      auto& order = orders[index];
      if (terminal(order.state) || order.venue_id != mapping_for(external).venue_id ||
          order.instrument_id != mapping_for(external).instrument_id ||
          market_side(order.side) != external.side ||
          order.limit_price_ticks != external.price_ticks ||
          !external_before_order(external, index)) {
        continue;
      }
      order.cancellations_ahead_units += canceled;
      if (order.shadow) {
        auto& queue = order_metadata[index].shadow_queue_ahead_units;
        queue = canceled >= queue ? 0U : queue - canceled;
      }
    }
  }

  [[nodiscard]] const InstrumentMapping&
  mapping_for(const ExternalOrder& external) const noexcept {
    const auto index = find_market(external.venue_number, external.instrument_number);
    return markets[index].mapping;
  }

  [[nodiscard]] Status
  update_external_queue(const synthetic::SyntheticEvent& event) noexcept {
    if (event.type == synthetic::NativeMessageType::add_order) {
      return add_external({.venue_number = event.venue_number,
                           .instrument_number = event.instrument_number,
                           .synthetic_order_id = event.synthetic_order_id,
                           .side = event.side,
                           .price_ticks = event.price_ticks,
                           .remaining_quantity_units = event.quantity_units,
                           .priority_time_ns = event.process_monotonic_time_ns,
                           .priority_ordinal = event.global_ordinal,
                           .active = true,
                           .source_live = true});
    }
    if (event.type == synthetic::NativeMessageType::cancel_order) {
      const auto index = find_external(event);
      if (index == external_orders.size()) {
        return Status::invalid_event;
      }
      auto& order = external_orders[index];
      const auto canceled = order.remaining_quantity_units;
      record_cancellation_ahead(order, canceled);
      order.remaining_quantity_units = 0U;
      order.active = false;
      order.source_live = false;
      return Status::ok;
    }
    if (event.type == synthetic::NativeMessageType::modify_order) {
      const auto index = find_external(event);
      if (index == external_orders.size()) {
        return Status::invalid_event;
      }
      auto& order = external_orders[index];
      if (event.quantity_units < order.remaining_quantity_units) {
        record_cancellation_ahead(order, order.remaining_quantity_units -
                                             event.quantity_units);
      } else if (event.quantity_units > order.remaining_quantity_units) {
        order.priority_time_ns = event.process_monotonic_time_ns;
        order.priority_ordinal = event.global_ordinal;
      }
      order.remaining_quantity_units = event.quantity_units;
      order.active = event.quantity_units != 0U;
      return Status::ok;
    }
    if (event.type != synthetic::NativeMessageType::price_level) {
      return Status::ok;
    }
    const auto market_index = find_market(event.venue_number, event.instrument_number);
    markets[market_index].queue_quality = QueueModelQuality::aggregated_price_level;
    report_value.worst_queue_model_quality = QueueModelQuality::aggregated_price_level;
    const auto existing = find_aggregate(event);
    const auto new_quantity = event.action == synthetic::BookAction::delete_level
                                  ? 0U
                                  : event.level_quantity_units;
    if (existing != external_orders.size()) {
      auto& aggregate = external_orders[existing];
      if (new_quantity < aggregate.remaining_quantity_units) {
        record_cancellation_ahead(aggregate,
                                  aggregate.remaining_quantity_units - new_quantity);
      } else if (new_quantity > aggregate.remaining_quantity_units) {
        aggregate.priority_time_ns = event.process_monotonic_time_ns;
        aggregate.priority_ordinal = event.global_ordinal;
      }
      aggregate.remaining_quantity_units = new_quantity;
      aggregate.active = new_quantity != 0U;
      aggregate.source_live = aggregate.active;
      return Status::ok;
    }
    if (new_quantity == 0U) {
      return Status::ok;
    }
    return add_external({.venue_number = event.venue_number,
                         .instrument_number = event.instrument_number,
                         .synthetic_order_id = event.global_ordinal,
                         .side = event.side,
                         .price_ticks = event.price_ticks,
                         .remaining_quantity_units = new_quantity,
                         .priority_time_ns = event.process_monotonic_time_ns,
                         .priority_ordinal = event.global_ordinal,
                         .active = true,
                         .source_live = true,
                         .aggregate = true});
  }

  [[nodiscard]] std::uint64_t
  queue_ahead(const std::size_t order_index) const noexcept {
    const auto& order = orders[order_index];
    const auto& metadata = order_metadata[order_index];
    std::uint64_t quantity{};
    for (const auto& external : external_orders) {
      if (!external.active || external.venue_number != metadata.venue_number ||
          external.instrument_number != metadata.instrument_number ||
          external.side != market_side(order.side) ||
          external.price_ticks != order.limit_price_ticks ||
          !external_before_order(external, order_index)) {
        continue;
      }
      if (!checked_add_u64(quantity, external.remaining_quantity_units, quantity)) {
        return std::numeric_limits<std::uint64_t>::max();
      }
    }
    return quantity;
  }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] Status add_hidden_ahead(const std::size_t order_index,
                                        const std::uint64_t quantity) noexcept {
    if (quantity == 0U) {
      return Status::ok;
    }
    const auto& order = orders[order_index];
    const auto& metadata = order_metadata[order_index];
    return add_external({.venue_number = metadata.venue_number,
                         .instrument_number = metadata.instrument_number,
                         .synthetic_order_id = order.order_id.low(),
                         .side = market_side(order.side),
                         .price_ticks = order.limit_price_ticks,
                         .remaining_quantity_units = quantity,
                         .priority_time_ns = order.acknowledgement_time_ns,
                         .priority_ordinal = metadata.priority_ordinal - 1U,
                         .active = true,
                         .source_live = false,
                         .inferred_hidden = true});
  }

  // NOLINTBEGIN(bugprone-easily-swappable-parameters)
  [[nodiscard]] Status record_forecast(const common::StrategyId strategy_id,
                                       const StrategyAction& action,
                                       const std::size_t market_index,
                                       const std::uint64_t now_ns) noexcept {
    if (!action.forecast.valid) {
      return Status::ok;
    }
    const auto midpoint = midpoint_x2(market_index);
    if (!midpoint.has_value() || *midpoint <= 0) {
      return Status::invalid_strategy_action;
    }
    const auto horizon = action.forecast.horizon_ns == 0U
                             ? config.default_forecast_horizon_ns
                             : action.forecast.horizon_ns;
    std::uint64_t due{};
    if (!checked_add_u64(now_ns, horizon, due)) {
      return Status::arithmetic_overflow;
    }
    forecasts.push_back({.strategy_id = strategy_id,
                         .instrument_id = action.instrument_id,
                         .probability_up_ppm = action.forecast.probability_up_ppm,
                         .p10_ppm = action.forecast.return_p10_ppm,
                         .p90_ppm = action.forecast.return_p90_ppm,
                         .initial_midpoint_x2 = *midpoint,
                         .due_time_ns = due});
    return Status::ok;
  }
  // NOLINTEND(bugprone-easily-swappable-parameters)

  [[nodiscard]] Status schedule_cancel(const common::StrategyId strategy_id,
                                       const StrategyAction& action,
                                       const std::uint64_t now_ns) noexcept {
    for (std::size_t cursor = orders.size(); cursor > 0U; --cursor) {
      const auto index = cursor - 1U;
      auto& order = orders[index];
      if (order.strategy_id != strategy_id ||
          order.client_order_key != action.client_order_key || terminal(order.state)) {
        continue;
      }
      std::uint64_t due{};
      if (!checked_add_u64(now_ns, sample_latency(++submission_ordinal), due)) {
        return Status::arithmetic_overflow;
      }
      order_metadata[index].cancel_due_ns = due;
      return Status::ok;
    }
    return Status::invalid_strategy_action;
  }

  [[nodiscard]] Status submit_action(const common::StrategyId strategy_id,
                                     const StrategyAction& action,
                                     const std::uint64_t now_ns) noexcept {
    const auto market_index = find_market(action.venue_id, action.instrument_id);
    if (market_index == markets.size() ||
        !valid_forecast(action.forecast, config.default_forecast_horizon_ns)) {
      return Status::invalid_strategy_action;
    }
    auto forecast_status = record_forecast(strategy_id, action, market_index, now_ns);
    if (forecast_status != Status::ok) {
      return forecast_status;
    }
    if (action.kind == ActionKind::forecast_only) {
      return Status::ok;
    }
    if (action.kind == ActionKind::cancel_order) {
      return schedule_cancel(strategy_id, action, now_ns);
    }
    const auto& market = markets[market_index];
    const auto allowed = market.status == synthetic::TradingStatus::open ||
                         (config.allow_auction_orders &&
                          market.status == synthetic::TradingStatus::auction);
    if (!allowed || action.client_order_key == 0U || action.quantity_units == 0U ||
        (action.kind == ActionKind::limit_order && action.limit_price_ticks <= 0)) {
      return Status::invalid_strategy_action;
    }
    for (const auto& existing : orders) {
      if (existing.strategy_id == strategy_id &&
          existing.client_order_key == action.client_order_key &&
          !terminal(existing.state)) {
        return Status::invalid_strategy_action;
      }
    }
    if (orders.size() >= config.maximum_orders ||
        action.quantity_units >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return Status::capacity_exhausted;
    }
    const auto mid = midpoint_x2(market_index);
    if (!mid.has_value()) {
      return Status::invalid_strategy_action;
    }
    const auto ordinal = ++submission_ordinal;
    const auto latency = sample_latency(ordinal);
    std::uint64_t ack_due{};
    const auto tif = action.time_in_force_ns == 0U ? config.stale_order_after_ns
                                                   : action.time_in_force_ns;
    std::uint64_t expiry{};
    if (!checked_add_u64(now_ns, latency, ack_due) ||
        !checked_add_u64(ack_due, tif, expiry)) {
      return Status::arithmetic_overflow;
    }
    const common::OrderId order_id{kBacktestIdentifierNamespace ^ strategy_id.high(),
                                   strategy_id.low() ^ action.client_order_key ^
                                       ordinal};
    orders.push_back({.order_id = order_id,
                      .strategy_id = strategy_id,
                      .venue_id = action.venue_id,
                      .instrument_id = action.instrument_id,
                      .client_order_key = action.client_order_key,
                      .side = action.side,
                      .kind = action.kind,
                      .state = OrderState::pending_ack,
                      .limit_price_ticks = action.limit_price_ticks,
                      .requested_quantity_units = action.quantity_units,
                      .decision_time_ns = now_ns,
                      .acknowledgement_time_ns = ack_due,
                      .expiry_time_ns = expiry,
                      .arrival_midpoint_x2 = *mid,
                      .shadow = action.shadow});
    order_metadata.push_back({.venue_number = market.mapping.venue_number,
                              .instrument_number = market.mapping.instrument_number,
                              .submission_ordinal = ordinal,
                              .priority_ordinal = ordinal * 2U,
                              .acknowledgement_due_ns = ack_due});
    return Status::ok;
  }

  [[nodiscard]] Status
  invoke_strategies(const MarketView& view,
                    const synthetic::SyntheticEvent& event) noexcept {
    for (const auto& slot : strategies) {
      StrategyActionBuffer buffer;
      const auto result = slot.strategy->on_market_event(view, event, buffer);
      if (result != Status::ok) {
        return Status::strategy_failed;
      }
      for (const auto& action : buffer.actions()) {
        const auto submit =
            submit_action(slot.strategy_id, action, event.process_monotonic_time_ns);
        if (submit != Status::ok) {
          return submit;
        }
      }
    }
    return settle_controls(event.process_monotonic_time_ns);
  }

  [[nodiscard]] std::uint64_t
  impact_ticks(const std::uint64_t quantity) const noexcept {
    std::uint64_t product{};
    if (!checked_mul_u64(quantity, config.impact_ticks_per_million_units, product)) {
      return std::numeric_limits<std::uint64_t>::max();
    }
    return product == 0U ? 0U : ((product - 1U) / kPartsPerMillion) + 1U;
  }

  [[nodiscard]] Status apply_position(const SimulatedFill& fill) noexcept {
    if (fill.shadow) {
      return Status::ok;
    }
    const auto market_index = find_market(fill.venue_id, fill.instrument_id);
    std::int64_t notional{};
    if (market_index == markets.size() ||
        !checked_notional(fill.execution_price_ticks,
                          markets[market_index].mapping.tick_value_currency_nanos,
                          fill.quantity_units, notional)) {
      return Status::arithmetic_overflow;
    }
    const auto position_index =
        find_or_create_position(fill.strategy_id, fill.instrument_id);
    auto& position = positions[position_index];
    const auto quantity = static_cast<std::int64_t>(fill.quantity_units);
    std::int64_t cash_delta{};
    if (fill.side == OrderSide::buy) {
      if (!checked_sub_i64(-notional, fill.fee_currency_nanos, cash_delta) ||
          !checked_add_i64(position.quantity_units, quantity,
                           position.quantity_units)) {
        return Status::arithmetic_overflow;
      }
    } else {
      if (!checked_sub_i64(notional, fill.fee_currency_nanos, cash_delta) ||
          !checked_add_i64(position.quantity_units, -quantity,
                           position.quantity_units)) {
        return Status::arithmetic_overflow;
      }
    }
    if (!checked_add_i64(position.cash_currency_nanos, cash_delta,
                         position.cash_currency_nanos)) {
      return Status::arithmetic_overflow;
    }
    const auto strategy_index = find_strategy_runtime(fill.strategy_id);
    std::uint64_t turnover_add{};
    if (strategy_index == strategy_runtime.size() ||
        !checked_mul_u64(static_cast<std::uint64_t>(fill.raw_price_ticks),
                         static_cast<std::uint64_t>(
                             markets[market_index].mapping.tick_value_currency_nanos),
                         turnover_add) ||
        !checked_mul_u64(turnover_add, fill.quantity_units, turnover_add) ||
        !checked_add_u64(strategy_runtime[strategy_index].turnover, turnover_add,
                         strategy_runtime[strategy_index].turnover)) {
      return Status::arithmetic_overflow;
    }
    return Status::ok;
  }

  // NOLINTBEGIN(bugprone-easily-swappable-parameters)
  [[nodiscard]] Status
  fill_order(const std::size_t order_index, const std::uint64_t quantity,
             const std::int64_t raw_price, const std::uint64_t event_hash,
             const std::uint64_t now_ns, const bool maker) noexcept {
    if (quantity == 0U || fills.size() >= config.maximum_fills) {
      return quantity == 0U ? Status::ok : Status::capacity_exhausted;
    }
    auto& order = orders[order_index];
    const auto impact = impact_ticks(quantity);
    std::uint64_t modeled_cost{};
    if (impact == std::numeric_limits<std::uint64_t>::max() ||
        !checked_add_u64(config.slippage_ticks, impact, modeled_cost) ||
        modeled_cost >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return Status::arithmetic_overflow;
    }
    const auto signed_cost = static_cast<std::int64_t>(modeled_cost);
    std::int64_t execution_price{};
    if (order.side == OrderSide::buy) {
      if (!checked_add_i64(raw_price, signed_cost, execution_price)) {
        return Status::arithmetic_overflow;
      }
      if (order.kind == ActionKind::limit_order) {
        execution_price = std::min(execution_price, order.limit_price_ticks);
      }
    } else {
      if (!checked_sub_i64(raw_price, signed_cost, execution_price)) {
        return Status::arithmetic_overflow;
      }
      if (order.kind == ActionKind::limit_order) {
        execution_price = std::max(execution_price, order.limit_price_ticks);
      }
    }
    if (execution_price <= 0) {
      return Status::arithmetic_overflow;
    }
    const auto fee_per_unit = maker ? config.maker_fee_per_unit_currency_nanos
                                    : config.taker_fee_per_unit_currency_nanos;
    std::int64_t fee{};
    if (!checked_signed_product(fee_per_unit, quantity, fee)) {
      return Status::arithmetic_overflow;
    }
    std::uint64_t mark_due{};
    if (!checked_add_u64(now_ns, config.adverse_selection_horizon_ns, mark_due)) {
      return Status::arithmetic_overflow;
    }
    const SimulatedFill fill{
        .fill_id = common::GlobalEventId{kBacktestIdentifierNamespace,
                                         static_cast<std::uint64_t>(fills.size() + 1U)},
        .order_id = order.order_id,
        .strategy_id = order.strategy_id,
        .venue_id = order.venue_id,
        .instrument_id = order.instrument_id,
        .side = order.side,
        .quantity_units = quantity,
        .raw_price_ticks = raw_price,
        .execution_price_ticks = execution_price,
        .arrival_midpoint_x2 = order.arrival_midpoint_x2,
        .fee_currency_nanos = fee,
        .process_monotonic_time_ns = now_ns,
        .mark_due_time_ns = mark_due,
        .source_event_hash = event_hash,
        .modeled_slippage_ticks = config.slippage_ticks,
        .modeled_impact_ticks = impact,
        .maker = maker,
        .shadow = order.shadow};
    const auto apply = apply_position(fill);
    if (apply != Status::ok) {
      return apply;
    }
    fills.push_back(fill);
    if (order.first_fill_time_ns == 0U) {
      order.first_fill_time_ns = now_ns;
    }
    order.last_fill_time_ns = now_ns;
    order.filled_quantity_units += quantity;
    order.state = order.filled_quantity_units == order.requested_quantity_units
                      ? OrderState::filled
                      : OrderState::partially_filled;
    return Status::ok;
  }
  // NOLINTEND(bugprone-easily-swappable-parameters)

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] Status aggressive_fill(const std::size_t order_index,
                                       const std::size_t market_index,
                                       const std::uint64_t now_ns) noexcept {
    auto& order = orders[order_index];
    auto& runtime = markets[market_index];
    const auto* book =
        books.find(runtime.mapping.venue_number, runtime.mapping.instrument_number);
    if (book == nullptr || runtime.status != synthetic::TradingStatus::open) {
      if (order.kind == ActionKind::market_order) {
        order.state = OrderState::expired;
      }
      return Status::ok;
    }
    const auto* level = order.side == OrderSide::buy ? synthetic::best_ask(*book)
                                                     : synthetic::best_bid(*book);
    if (level == nullptr) {
      if (order.kind == ActionKind::market_order) {
        order.state = OrderState::expired;
      }
      return Status::ok;
    }
    const bool marketable =
        order.kind == ActionKind::market_order ||
        (order.side == OrderSide::buy ? order.limit_price_ticks >= level->price_ticks
                                      : order.limit_price_ticks <= level->price_ticks);
    if (!marketable) {
      return Status::ok;
    }
    auto& consumed = order.side == OrderSide::buy ? runtime.consumed_ask_units
                                                  : runtime.consumed_bid_units;
    const auto visible =
        consumed >= level->quantity_units ? 0U : level->quantity_units - consumed;
    const auto hidden = hidden_quantity(order_metadata[order_index].submission_ordinal);
    std::uint64_t available{};
    if (!checked_add_u64(visible, hidden, available)) {
      return Status::arithmetic_overflow;
    }
    const auto remaining = order.requested_quantity_units - order.filled_quantity_units;
    const auto quantity = std::min(remaining, available);
    if (quantity != 0U) {
      const auto fill_status = fill_order(order_index, quantity, level->price_ticks,
                                          last_event_hash, now_ns, false);
      if (fill_status != Status::ok) {
        return fill_status;
      }
      const auto visible_used = std::min(quantity, visible);
      if (!order.shadow && !checked_add_u64(consumed, visible_used, consumed)) {
        return Status::arithmetic_overflow;
      }
    }
    if (order.kind == ActionKind::market_order && !terminal(order.state)) {
      order.state = OrderState::expired;
    }
    return Status::ok;
  }

  [[nodiscard]] Status acknowledge_order(const std::size_t index,
                                         const std::uint64_t now_ns) noexcept {
    auto& order = orders[index];
    if (order.state != OrderState::pending_ack) {
      return Status::ok;
    }
    if (rejected(order_metadata[index].submission_ordinal)) {
      order.state = OrderState::rejected;
      return Status::ok;
    }
    order.state = OrderState::working;
    const auto inferred_hidden =
        hidden_quantity(order_metadata[index].submission_ordinal);
    order.inferred_hidden_ahead_units = inferred_hidden;
    if (order.kind == ActionKind::limit_order) {
      if (!order.shadow) {
        const auto hidden_status = add_hidden_ahead(index, inferred_hidden);
        if (hidden_status != Status::ok) {
          return hidden_status;
        }
      }
      order.initial_queue_ahead_units = queue_ahead(index);
      if (order.initial_queue_ahead_units ==
          std::numeric_limits<std::uint64_t>::max()) {
        return Status::arithmetic_overflow;
      }
      order_metadata[index].shadow_queue_ahead_units = order.initial_queue_ahead_units;
      if (order.shadow &&
          !checked_add_u64(order_metadata[index].shadow_queue_ahead_units,
                           inferred_hidden,
                           order_metadata[index].shadow_queue_ahead_units)) {
        return Status::arithmetic_overflow;
      }
      if (order.shadow) {
        order.initial_queue_ahead_units =
            order_metadata[index].shadow_queue_ahead_units;
      }
    }
    return aggressive_fill(index, find_market(order.venue_id, order.instrument_id),
                           now_ns);
  }

  [[nodiscard]] Status settle_controls(const std::uint64_t now_ns) noexcept {
    std::vector<std::size_t> due;
    due.reserve(orders.size());
    for (std::size_t index = 0U; index < orders.size(); ++index) {
      const auto& order = orders[index];
      const auto& metadata = order_metadata[index];
      if (order.state == OrderState::pending_ack &&
          metadata.acknowledgement_due_ns <= now_ns) {
        due.push_back(index);
      }
    }
    std::ranges::sort(due, [this](const auto left, const auto right) {
      const auto& lhs = order_metadata[left];
      const auto& rhs = order_metadata[right];
      return lhs.acknowledgement_due_ns < rhs.acknowledgement_due_ns ||
             (lhs.acknowledgement_due_ns == rhs.acknowledgement_due_ns &&
              lhs.submission_ordinal < rhs.submission_ordinal);
    });
    for (const auto index : due) {
      const auto result = acknowledge_order(index, now_ns);
      if (result != Status::ok) {
        return result;
      }
    }
    for (std::size_t index = 0U; index < orders.size(); ++index) {
      auto& order = orders[index];
      const auto& metadata = order_metadata[index];
      if (!terminal(order.state) && metadata.cancel_due_ns != 0U &&
          metadata.cancel_due_ns <= now_ns) {
        order.state = OrderState::canceled;
      } else if (!terminal(order.state) && order.expiry_time_ns <= now_ns) {
        order.state = OrderState::expired;
      }
    }
    return Status::ok;
  }

  struct Candidate {
    bool own{false};
    std::size_t index{};
    std::int64_t price_ticks{};
    std::uint64_t priority_time_ns{};
    std::uint64_t priority_ordinal{};
  };

  [[nodiscard]] static bool price_eligible(const OrderSide side,
                                           const std::int64_t resting_price,
                                           const std::int64_t trade_price) noexcept {
    return side == OrderSide::buy ? resting_price >= trade_price
                                  : resting_price <= trade_price;
  }

  void record_execution_ahead(const ExternalOrder& external,
                              const std::uint64_t quantity) noexcept {
    for (std::size_t index = 0U; index < orders.size(); ++index) {
      auto& order = orders[index];
      if (terminal(order.state) || order.shadow ||
          order.venue_id != mapping_for(external).venue_id ||
          order.instrument_id != mapping_for(external).instrument_id ||
          market_side(order.side) != external.side ||
          order.limit_price_ticks != external.price_ticks ||
          !external_before_order(external, index)) {
        continue;
      }
      order.executions_ahead_units += quantity;
    }
  }

  [[nodiscard]] Status match_actual(const synthetic::SyntheticEvent& event,
                                    const OrderSide resting_side,
                                    std::uint64_t available) noexcept {
    std::vector<Candidate> candidates;
    candidates.reserve(external_orders.size() + orders.size());
    for (std::size_t index = 0U; index < external_orders.size(); ++index) {
      const auto& external = external_orders[index];
      if (!external.active || external.venue_number != event.venue_number ||
          external.instrument_number != event.instrument_number ||
          external.side != market_side(resting_side) ||
          !price_eligible(resting_side, external.price_ticks, event.price_ticks)) {
        continue;
      }
      candidates.push_back({.own = false,
                            .index = index,
                            .price_ticks = external.price_ticks,
                            .priority_time_ns = external.priority_time_ns,
                            .priority_ordinal = external.priority_ordinal});
    }
    for (std::size_t index = 0U; index < orders.size(); ++index) {
      const auto& order = orders[index];
      const auto& metadata = order_metadata[index];
      if (order.shadow ||
          (order.state != OrderState::working &&
           order.state != OrderState::partially_filled) ||
          metadata.venue_number != event.venue_number ||
          metadata.instrument_number != event.instrument_number ||
          order.side != resting_side || order.kind != ActionKind::limit_order ||
          !price_eligible(resting_side, order.limit_price_ticks, event.price_ticks)) {
        continue;
      }
      candidates.push_back({.own = true,
                            .index = index,
                            .price_ticks = order.limit_price_ticks,
                            .priority_time_ns = order.acknowledgement_time_ns,
                            .priority_ordinal = metadata.priority_ordinal});
    }
    std::ranges::sort(candidates, [resting_side](const Candidate& left,
                                                 const Candidate& right) {
      if (left.price_ticks != right.price_ticks) {
        return resting_side == OrderSide::buy ? left.price_ticks > right.price_ticks
                                              : left.price_ticks < right.price_ticks;
      }
      if (left.priority_time_ns != right.priority_time_ns) {
        return left.priority_time_ns < right.priority_time_ns;
      }
      if (left.priority_ordinal != right.priority_ordinal) {
        return left.priority_ordinal < right.priority_ordinal;
      }
      return static_cast<unsigned>(left.own) < static_cast<unsigned>(right.own);
    });
    for (const auto& candidate : candidates) {
      if (available == 0U) {
        break;
      }
      if (!candidate.own) {
        auto& external = external_orders[candidate.index];
        const auto consumed = std::min(available, external.remaining_quantity_units);
        record_execution_ahead(external, consumed);
        external.remaining_quantity_units -= consumed;
        available -= consumed;
        external.active = external.remaining_quantity_units != 0U;
        continue;
      }
      auto& order = orders[candidate.index];
      const auto remaining =
          order.requested_quantity_units - order.filled_quantity_units;
      const auto quantity = std::min(available, remaining);
      const auto result =
          fill_order(candidate.index, quantity, event.price_ticks, event.event_hash,
                     event.process_monotonic_time_ns, true);
      if (result != Status::ok) {
        return result;
      }
      available -= quantity;
    }
    return Status::ok;
  }

  [[nodiscard]] Status match_shadow(const synthetic::SyntheticEvent& event,
                                    const OrderSide resting_side) noexcept {
    for (std::size_t index = 0U; index < orders.size(); ++index) {
      auto& order = orders[index];
      auto& metadata = order_metadata[index];
      if (!order.shadow ||
          (order.state != OrderState::working &&
           order.state != OrderState::partially_filled) ||
          metadata.venue_number != event.venue_number ||
          metadata.instrument_number != event.instrument_number ||
          order.side != resting_side || order.kind != ActionKind::limit_order ||
          !price_eligible(resting_side, order.limit_price_ticks, event.price_ticks)) {
        continue;
      }
      auto available = event.quantity_units;
      const auto consumed_ahead =
          std::min(available, metadata.shadow_queue_ahead_units);
      metadata.shadow_queue_ahead_units -= consumed_ahead;
      order.executions_ahead_units += consumed_ahead;
      available -= consumed_ahead;
      const auto remaining =
          order.requested_quantity_units - order.filled_quantity_units;
      const auto quantity = std::min(available, remaining);
      const auto result =
          fill_order(index, quantity, event.price_ticks, event.event_hash,
                     event.process_monotonic_time_ns, true);
      if (result != Status::ok) {
        return result;
      }
    }
    return Status::ok;
  }

  [[nodiscard]] Status match_event(const synthetic::SyntheticEvent& event,
                                   const std::size_t market_index) noexcept {
    if (markets[market_index].status == synthetic::TradingStatus::halted) {
      return Status::ok;
    }
    if (event.type == synthetic::NativeMessageType::trade &&
        markets[market_index].status == synthetic::TradingStatus::open) {
      const auto side =
          event.side == synthetic::Side::bid ? OrderSide::buy : OrderSide::sell;
      auto result = match_actual(event, side, event.quantity_units);
      if (result != Status::ok) {
        return result;
      }
      return match_shadow(event, side);
    }
    if (event.type == synthetic::NativeMessageType::auction_imbalance &&
        markets[market_index].status == synthetic::TradingStatus::auction &&
        config.allow_auction_orders) {
      auto result = match_actual(event, OrderSide::buy, event.quantity_units);
      if (result != Status::ok) {
        return result;
      }
      result = match_actual(event, OrderSide::sell, event.quantity_units);
      if (result != Status::ok) {
        return result;
      }
      result = match_shadow(event, OrderSide::buy);
      return result == Status::ok ? match_shadow(event, OrderSide::sell) : result;
    }
    return Status::ok;
  }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  void resolve_horizons(const std::size_t market_index,
                        const std::uint64_t now_ns) noexcept {
    const auto midpoint = midpoint_x2(market_index);
    if (!midpoint.has_value()) {
      return;
    }
    const auto instrument_id = markets[market_index].mapping.instrument_id;
    for (auto& forecast : forecasts) {
      if (!forecast.resolved && forecast.instrument_id == instrument_id &&
          forecast.due_time_ns <= now_ns) {
        forecast.realized_return_ppm =
            (static_cast<double>(*midpoint - forecast.initial_midpoint_x2) /
             static_cast<double>(forecast.initial_midpoint_x2)) *
            static_cast<double>(kPartsPerMillion);
        forecast.resolved = true;
      }
    }
    for (auto& fill : fills) {
      if (!fill.future_mark_resolved && fill.instrument_id == instrument_id &&
          fill.mark_due_time_ns <= now_ns) {
        fill.future_midpoint_x2 = *midpoint;
        fill.future_mark_resolved = true;
      }
    }
  }

  [[nodiscard]] std::int64_t
  equity(const common::StrategyId strategy_id) const noexcept {
    long double value = 0.0L;
    for (const auto& position : positions) {
      if (position.strategy_id != strategy_id) {
        continue;
      }
      value += static_cast<long double>(position.cash_currency_nanos);
      const auto market_index = find_market_by_instrument(position.instrument_id);
      const auto midpoint = midpoint_x2(market_index);
      if (!midpoint.has_value()) {
        continue;
      }
      value += static_cast<long double>(position.quantity_units) *
               static_cast<long double>(*midpoint) *
               static_cast<long double>(
                   markets[market_index].mapping.tick_value_currency_nanos) /
               2.0L;
    }
    if (value > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
      return std::numeric_limits<std::int64_t>::max();
    }
    if (value < static_cast<long double>(std::numeric_limits<std::int64_t>::min())) {
      return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(std::llround(value));
  }

  [[nodiscard]] std::size_t
  find_market_by_instrument(const common::InstrumentId instrument_id) const noexcept {
    for (std::size_t index = 0U; index < markets.size(); ++index) {
      if (markets[index].mapping.instrument_id == instrument_id) {
        return index;
      }
    }
    return markets.size();
  }

  void sample_equity(const EventPeriod period) noexcept {
    for (auto& runtime : strategy_runtime) {
      const auto current = equity(runtime.strategy_id);
      std::int64_t change{};
      if (!checked_sub_i64(current, runtime.last_equity, change)) {
        change = current >= runtime.last_equity
                     ? std::numeric_limits<std::int64_t>::max()
                     : std::numeric_limits<std::int64_t>::min();
      }
      runtime.pnl_changes.push_back(change);
      auto& period_metrics = runtime.periods[period_index(period)];
      ++period_metrics.event_count;
      static_cast<void>(checked_add_i64(period_metrics.pnl_change_currency_nanos,
                                        change,
                                        period_metrics.pnl_change_currency_nanos));
      runtime.last_equity = current;
      runtime.peak_equity = std::max(runtime.peak_equity, current);
      std::int64_t drawdown{};
      if (checked_sub_i64(runtime.peak_equity, current, drawdown)) {
        runtime.maximum_drawdown = std::max(runtime.maximum_drawdown, drawdown);
      }
    }
  }

  [[nodiscard]] Status
  validate_sequence(const synthetic::SyntheticEvent& event) noexcept {
    if (event.channel_sequence == 0U || event.global_ordinal == 0U ||
        event.process_monotonic_time_ns < last_process_time_ns ||
        (last_global_ordinal != 0U &&
         event.global_ordinal != last_global_ordinal + 1U)) {
      return Status::invalid_sequence;
    }
    for (auto& channel : channels) {
      if (channel.venue_number == event.venue_number &&
          channel.channel_number == event.channel_number) {
        if (channel.last_sequence == std::numeric_limits<std::uint64_t>::max() ||
            event.channel_sequence != channel.last_sequence + 1U) {
          return Status::invalid_sequence;
        }
        channel.last_sequence = event.channel_sequence;
        last_process_time_ns = event.process_monotonic_time_ns;
        last_global_ordinal = event.global_ordinal;
        return Status::ok;
      }
    }
    channels.push_back({.venue_number = event.venue_number,
                        .channel_number = event.channel_number,
                        .last_sequence = event.channel_sequence});
    last_process_time_ns = event.process_monotonic_time_ns;
    last_global_ordinal = event.global_ordinal;
    return Status::ok;
  }

  [[nodiscard]] Status process_event(const synthetic::SyntheticEvent& event) noexcept {
    ++report_value.source_events;
    if (report_value.source_events > config.maximum_events) {
      return Status::capacity_exhausted;
    }
    if (!synthetic::valid_event_shape(event) ||
        synthetic::calculate_event_hash(event) != event.event_hash) {
      return Status::invalid_event;
    }
    const auto sequence_status = validate_sequence(event);
    if (sequence_status != Status::ok) {
      return sequence_status;
    }
    const auto market_index = find_market(event.venue_number, event.instrument_number);
    if (market_index == markets.size()) {
      return Status::invalid_event;
    }
    last_event_hash = event.event_hash;
    const auto settle = settle_controls(event.process_monotonic_time_ns);
    if (settle != Status::ok) {
      return settle;
    }
    auto& market = markets[market_index];
    market.previous_status = market.status;
    const auto period = period_for(event, market.previous_status);
    source_event_hash.add_u64(event.event_hash);
    if (event.data_quality == synthetic::DataQuality::invalid) {
      return Status::invalid_event;
    }
    if (event.data_quality != synthetic::DataQuality::valid) {
      ++report_value.suppressed_unsafe_events;
      sample_equity(period);
      return Status::ok;
    }
    if (event.type != synthetic::NativeMessageType::trading_status &&
        market.status != synthetic::TradingStatus::open &&
        market.status != synthetic::TradingStatus::auction) {
      ++report_value.suppressed_unsafe_events;
      sample_equity(period);
      return Status::ok;
    }
    const auto external_status = update_external_queue(event);
    if (external_status != Status::ok) {
      return external_status;
    }
    const auto book_status = books.apply(event);
    if (book_status != synthetic::BookApplyError::none) {
      return Status::invalid_event;
    }
    if (event.type == synthetic::NativeMessageType::trading_status) {
      market.status = event.status;
    } else if (event.type == synthetic::NativeMessageType::trade) {
      market.last_trade_ticks = event.price_ticks;
    }
    if (event.type >= synthetic::NativeMessageType::add_order &&
        event.type <= synthetic::NativeMessageType::quote) {
      market.consumed_bid_units = 0U;
      market.consumed_ask_units = 0U;
    }
    const auto match = match_event(event, market_index);
    if (match != Status::ok) {
      return match;
    }
    resolve_horizons(market_index, event.process_monotonic_time_ns);
    const auto view =
        market_view(market_index, event.data_quality, event.global_ordinal,
                    event.process_monotonic_time_ns);
    if (view.valid_book && (view.status == synthetic::TradingStatus::open ||
                            (config.allow_auction_orders &&
                             view.status == synthetic::TradingStatus::auction))) {
      const auto strategy_status = invoke_strategies(view, event);
      if (strategy_status != Status::ok) {
        return strategy_status;
      }
    }
    ++report_value.accepted_events;
    sample_equity(period);
    return Status::ok;
  }

  [[nodiscard]] Status finish() noexcept {
    if (finalized) {
      return status;
    }
    finalized = true;
    for (auto& order : orders) {
      if (!terminal(order.state)) {
        order.state = OrderState::expired;
      }
    }
    build_report();
    status = Status::complete;
    report_value.status = status;
    report_value.order_count = orders.size();
    report_value.fill_count = fills.size();
    report_value.final_book_hash = books.stable_hash();
    report_value.source_event_chain_hash = source_event_hash.value();
    report_value.deterministic_result_sha256 = {};
    const auto serialized = report_json(report_value);
    report_value.deterministic_result_sha256 = common::sha256(
        {reinterpret_cast<const std::uint8_t*>(serialized.data()), serialized.size()});
    return status;
  }

  void build_forecast_metrics(const common::StrategyId strategy_id,
                              ForecastMetrics& output) const noexcept {
    std::array<std::uint64_t, 10U> bin_count{};
    std::array<double, 10U> bin_probability{};
    std::array<double, 10U> bin_outcome{};
    double log_loss{};
    double brier{};
    double correct{};
    double directional{};
    double covered{};
    for (const auto& forecast : forecasts) {
      if (forecast.strategy_id != strategy_id) {
        continue;
      }
      if (!forecast.resolved) {
        ++output.unresolved_samples;
        continue;
      }
      ++output.resolved_samples;
      const auto probability = static_cast<double>(forecast.probability_up_ppm) /
                               static_cast<double>(kProbabilityScale);
      const auto outcome = forecast.realized_return_ppm > 0.0 ? 1.0 : 0.0;
      const auto bounded_probability =
          std::clamp(probability, kProbabilityFloor, 1.0 - kProbabilityFloor);
      log_loss += -((outcome * std::log(bounded_probability)) +
                    ((1.0 - outcome) * std::log(1.0 - bounded_probability)));
      const auto error = probability - outcome;
      brier += error * error;
      if (forecast.probability_up_ppm != kProbabilityScale / 2U) {
        directional += 1.0;
        const auto predicts_up = forecast.probability_up_ppm > kProbabilityScale / 2U;
        correct += predicts_up == (outcome > 0.5) ? 1.0 : 0.0;
      }
      covered +=
          forecast.realized_return_ppm >= static_cast<double>(forecast.p10_ppm) &&
                  forecast.realized_return_ppm <= static_cast<double>(forecast.p90_ppm)
              ? 1.0
              : 0.0;
      const auto bin = std::min<std::size_t>(
          9U, static_cast<std::size_t>(forecast.probability_up_ppm / 100'000U));
      ++bin_count[bin];
      bin_probability[bin] += probability;
      bin_outcome[bin] += outcome;
    }
    if (output.resolved_samples == 0U) {
      return;
    }
    const auto count = static_cast<double>(output.resolved_samples);
    output.log_loss = log_loss / count;
    output.brier_score = brier / count;
    output.directional_precision =
        directional <= kProbabilityFloor ? 0.0 : correct / directional;
    output.p10_p90_coverage = covered / count;
    double calibration{};
    for (std::size_t index = 0U; index < bin_count.size(); ++index) {
      if (bin_count[index] == 0U) {
        continue;
      }
      const auto bin_size = static_cast<double>(bin_count[index]);
      calibration += (bin_size / count) * std::abs((bin_probability[index] / bin_size) -
                                                   (bin_outcome[index] / bin_size));
    }
    output.expected_calibration_error = calibration;
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void build_execution_metrics(const common::StrategyId strategy_id,
                               ExecutionMetrics& output) const noexcept {
    double fill_time{};
    double effective{};
    double realized{};
    double slippage{};
    double adverse{};
    double shortfall{};
    std::uint64_t fill_samples{};
    std::uint64_t resolved_marks{};
    for (const auto& order : orders) {
      if (order.strategy_id != strategy_id) {
        continue;
      }
      ++output.orders;
      output.shadow_orders += order.shadow ? 1U : 0U;
      output.rejected_orders += order.state == OrderState::rejected ? 1U : 0U;
      output.expired_orders += order.state == OrderState::expired ? 1U : 0U;
      output.requested_quantity_units += order.requested_quantity_units;
      output.filled_quantity_units += order.filled_quantity_units;
      if (order.first_fill_time_ns != 0U) {
        fill_time +=
            static_cast<double>(order.first_fill_time_ns - order.decision_time_ns);
        ++fill_samples;
      }
    }
    for (const auto& fill : fills) {
      if (fill.strategy_id != strategy_id) {
        continue;
      }
      const auto sign = static_cast<double>(side_sign(fill.side));
      const auto effective_value =
          sign * ((2.0 * static_cast<double>(fill.execution_price_ticks)) -
                  static_cast<double>(fill.arrival_midpoint_x2));
      effective += effective_value * static_cast<double>(fill.quantity_units);
      slippage +=
          sign *
          static_cast<double>(fill.execution_price_ticks - fill.raw_price_ticks) *
          static_cast<double>(fill.quantity_units);
      if (fill.future_mark_resolved) {
        const auto realized_value =
            sign * ((2.0 * static_cast<double>(fill.execution_price_ticks)) -
                    static_cast<double>(fill.future_midpoint_x2));
        realized += realized_value * static_cast<double>(fill.quantity_units);
        adverse += (effective_value - realized_value) *
                   static_cast<double>(fill.quantity_units);
        resolved_marks += fill.quantity_units;
      }
      const auto market_index = find_market(fill.venue_id, fill.instrument_id);
      const auto tick_value =
          static_cast<double>(markets[market_index].mapping.tick_value_currency_nanos);
      shortfall += (sign *
                    (static_cast<double>(fill.execution_price_ticks) -
                     (static_cast<double>(fill.arrival_midpoint_x2) / 2.0)) *
                    tick_value * static_cast<double>(fill.quantity_units)) +
                   static_cast<double>(fill.fee_currency_nanos);
      for (const auto& order : orders) {
        if (order.order_id == fill.order_id &&
            fill.quantity_units < order.requested_quantity_units) {
          ++output.partial_fill_events;
          break;
        }
      }
    }
    output.fill_ratio = output.requested_quantity_units == 0U
                            ? 0.0
                            : static_cast<double>(output.filled_quantity_units) /
                                  static_cast<double>(output.requested_quantity_units);
    output.mean_time_to_fill_ns =
        fill_samples == 0U ? 0.0 : fill_time / static_cast<double>(fill_samples);
    if (output.filled_quantity_units != 0U) {
      const auto quantity = static_cast<double>(output.filled_quantity_units);
      output.effective_spread_ticks = effective / quantity;
      output.slippage_ticks = slippage / quantity;
      output.implementation_shortfall_currency_nanos = shortfall;
    }
    if (resolved_marks != 0U) {
      const auto quantity = static_cast<double>(resolved_marks);
      output.realized_spread_ticks = realized / quantity;
      output.adverse_selection_ticks = adverse / quantity;
    }
  }

  [[nodiscard]] std::int64_t
  no_cost_pnl(const common::StrategyId strategy_id) const noexcept {
    long double cash{};
    std::array<std::int64_t, kMaximumBacktestInstruments> quantities{};
    for (const auto& fill : fills) {
      if (fill.strategy_id != strategy_id || fill.shadow) {
        continue;
      }
      const auto market_index = find_market(fill.venue_id, fill.instrument_id);
      std::int64_t notional{};
      if (!checked_notional(fill.raw_price_ticks,
                            markets[market_index].mapping.tick_value_currency_nanos,
                            fill.quantity_units, notional)) {
        continue;
      }
      const auto signed_quantity = static_cast<std::int64_t>(fill.quantity_units);
      if (fill.side == OrderSide::buy) {
        cash -= static_cast<long double>(notional);
        quantities[market_index] += signed_quantity;
      } else {
        cash += static_cast<long double>(notional);
        quantities[market_index] -= signed_quantity;
      }
    }
    for (std::size_t index = 0U; index < markets.size(); ++index) {
      const auto midpoint = midpoint_x2(index);
      if (!midpoint.has_value()) {
        continue;
      }
      cash +=
          static_cast<long double>(quantities[index]) *
          static_cast<long double>(*midpoint) *
          static_cast<long double>(markets[index].mapping.tick_value_currency_nanos) /
          2.0L;
    }
    return static_cast<std::int64_t>(std::llround(cash));
  }

  [[nodiscard]] std::int64_t
  transaction_cost(const common::StrategyId strategy_id) const noexcept {
    long double total{};
    for (const auto& fill : fills) {
      if (fill.strategy_id != strategy_id || fill.shadow) {
        continue;
      }
      const auto market_index = find_market(fill.venue_id, fill.instrument_id);
      const auto signed_ticks =
          static_cast<long double>(side_sign(fill.side)) *
          static_cast<long double>(fill.execution_price_ticks - fill.raw_price_ticks);
      total += ((signed_ticks *
                 static_cast<long double>(
                     markets[market_index].mapping.tick_value_currency_nanos) *
                 static_cast<long double>(fill.quantity_units)) +
                static_cast<long double>(fill.fee_currency_nanos));
    }
    if (total > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
      return std::numeric_limits<std::int64_t>::max();
    }
    if (total < static_cast<long double>(std::numeric_limits<std::int64_t>::min())) {
      return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(std::llround(total));
  }

  void build_portfolio_metrics(
      const common::StrategyId strategy_id, PortfolioMetrics& output,
      std::array<CostSensitivityPoint, kCostScenarioCount>& sensitivity) const {
    const auto runtime_index = find_strategy_runtime(strategy_id);
    const auto& runtime = strategy_runtime[runtime_index];
    const auto gross = no_cost_pnl(strategy_id);
    const auto cost = transaction_cost(strategy_id);
    output.gross_pnl_currency_nanos = gross;
    output.transaction_cost_currency_nanos = cost;
    static_cast<void>(checked_sub_i64(gross, cost, output.net_pnl_currency_nanos));
    output.turnover_currency_nanos = runtime.turnover;
    output.maximum_drawdown_currency_nanos = runtime.maximum_drawdown;
    for (std::size_t index = 0U; index < sensitivity.size(); ++index) {
      const auto multiplier = config.cost_multipliers_ppm[index];
      const auto scaled = static_cast<long double>(cost) *
                          static_cast<long double>(multiplier) /
                          static_cast<long double>(kPartsPerMillion);
      sensitivity[index] = {
          .cost_multiplier_ppm = multiplier,
          .net_pnl_currency_nanos = static_cast<std::int64_t>(
              std::llround(static_cast<long double>(gross) - scaled))};
    }
    if (runtime.pnl_changes.empty()) {
      return;
    }
    double mean{};
    for (const auto value : runtime.pnl_changes) {
      mean += static_cast<double>(value);
    }
    mean /= static_cast<double>(runtime.pnl_changes.size());
    double variance{};
    double downside_square{};
    std::size_t downside_count{};
    std::vector<std::int64_t> sorted = runtime.pnl_changes;
    for (const auto value : runtime.pnl_changes) {
      const auto centered = static_cast<double>(value) - mean;
      variance += centered * centered;
      if (value < 0) {
        const auto negative = static_cast<double>(value);
        downside_square += negative * negative;
        ++downside_count;
      }
    }
    variance /= static_cast<double>(runtime.pnl_changes.size());
    const auto deviation = std::sqrt(variance);
    output.sharpe_event_sample =
        deviation <= kProbabilityFloor ? 0.0 : mean / deviation;
    const auto downside =
        downside_count == 0U
            ? 0.0
            : std::sqrt(downside_square / static_cast<double>(downside_count));
    output.sortino_event_sample = downside <= kProbabilityFloor ? 0.0 : mean / downside;
    std::ranges::sort(sorted);
    const auto tail_count = std::max<std::size_t>(1U, (sorted.size() + 19U) / 20U);
    double tail_sum{};
    for (std::size_t index = 0U; index < tail_count; ++index) {
      tail_sum += static_cast<double>(sorted[index]);
    }
    output.cvar_95_currency_nanos = tail_sum / static_cast<double>(tail_count);
  }

  void build_report() noexcept {
    report_value.strategies.clear();
    report_value.strategies.reserve(strategies.size());
    for (const auto& slot : strategies) {
      StrategyReport report{};
      report.strategy_id = slot.strategy_id;
      build_forecast_metrics(slot.strategy_id, report.forecast);
      build_execution_metrics(slot.strategy_id, report.execution);
      build_portfolio_metrics(slot.strategy_id, report.portfolio,
                              report.cost_sensitivity);
      report.event_periods =
          strategy_runtime[find_strategy_runtime(slot.strategy_id)].periods;
      report_value.strategies.push_back(report);
    }
  }

private:
  friend class EventBacktester;

  BacktestConfig config;
  Status status{Status::ok};
  std::uint64_t config_hash{};
  synthetic::BookSet books;
  std::vector<MarketRuntime> markets;
  std::vector<StrategySlot> strategies;
  std::vector<StrategyRuntime> strategy_runtime;
  std::vector<ExternalOrder> external_orders;
  std::vector<SimulatedOrder> orders;
  std::vector<OrderMetadata> order_metadata;
  std::vector<SimulatedFill> fills;
  std::vector<ForecastRuntime> forecasts;
  std::vector<PositionRuntime> positions;
  std::vector<ChannelSequence> channels;
  BacktestReport report_value;
  std::uint64_t submission_ordinal{};
  std::uint64_t last_process_time_ns{};
  std::uint64_t last_global_ordinal{};
  std::uint64_t last_event_hash{};
  synthetic::StableHash64 source_event_hash;
  bool started{false};
  bool finalized{false};
};

EventBacktester::EventBacktester(BacktestConfig config)
    : impl_(std::make_unique<Impl>(config)) {}

EventBacktester::~EventBacktester() = default;
EventBacktester::EventBacktester(EventBacktester&&) noexcept = default;
EventBacktester& EventBacktester::operator=(EventBacktester&&) noexcept = default;

Status EventBacktester::add_strategy(const common::StrategyId strategy_id,
                                     IBacktestStrategy& strategy) noexcept {
  if (impl_->status != Status::ok || impl_->started || !strategy_id.valid() ||
      impl_->strategies.size() >= kMaximumBacktestStrategies) {
    return Status::invalid_configuration;
  }
  for (const auto& existing : impl_->strategies) {
    if (existing.strategy_id == strategy_id) {
      return Status::invalid_configuration;
    }
  }
  impl_->strategies.push_back({.strategy_id = strategy_id, .strategy = &strategy});
  Impl::StrategyRuntime runtime{};
  runtime.strategy_id = strategy_id;
  for (std::size_t index = 0U; index < runtime.periods.size(); ++index) {
    runtime.periods[index].period = static_cast<EventPeriod>(index);
  }
  runtime.pnl_changes.reserve(static_cast<std::size_t>(impl_->config.maximum_events));
  impl_->strategy_runtime.push_back(std::move(runtime));
  return Status::ok;
}

Status EventBacktester::run_events(
    const std::span<const synthetic::SyntheticEvent> events) noexcept {
  if (impl_->status != Status::ok || impl_->started || impl_->strategies.empty()) {
    return Status::invalid_configuration;
  }
  impl_->started = true;
  for (const auto& event : events) {
    const auto status = impl_->process_event(event);
    if (status != Status::ok) {
      impl_->status = status;
      impl_->report_value.status = status;
      return status;
    }
  }
  return impl_->finish();
}

Status
EventBacktester::run_synthetic(const synthetic::GeneratorConfig& config) noexcept {
  if (impl_->status != Status::ok || impl_->started || impl_->strategies.empty()) {
    return Status::invalid_configuration;
  }
  auto generator = synthetic::SyntheticExchangeGenerator::create(config);
  if (!generator.has_value()) {
    return Status::invalid_configuration;
  }
  impl_->started = true;
  for (;;) {
    synthetic::SyntheticEvent event{};
    const auto generated = generator->next(event);
    if (generated.error == synthetic::GenerationError::complete) {
      break;
    }
    if (!generated.ok()) {
      impl_->status = Status::source_invalid;
      impl_->report_value.status = impl_->status;
      return impl_->status;
    }
    const auto result = impl_->process_event(event);
    if (result != Status::ok) {
      impl_->status = result;
      impl_->report_value.status = result;
      return result;
    }
  }
  return impl_->finish();
}

Status EventBacktester::run_capture(const std::filesystem::path& path) noexcept {
  if (impl_->status != Status::ok || impl_->started || impl_->strategies.empty()) {
    return Status::invalid_configuration;
  }
  synthetic::CaptureError error{};
  auto reader = synthetic::CaptureReader::open(path, error);
  if (!reader.has_value()) {
    return error == synthetic::CaptureError::open_failed ? Status::source_open_failed
                                                         : Status::source_invalid;
  }
  impl_->started = true;
  for (std::uint64_t index = 0U; index < reader->header().physical_packet_count;
       ++index) {
    synthetic::CaptureRecord record{};
    if (reader->next(record) != synthetic::CaptureError::none) {
      impl_->status = Status::source_invalid;
      impl_->report_value.status = impl_->status;
      return impl_->status;
    }
    synthetic::SyntheticEvent event{};
    if (synthetic::decode_packet(record.packet, event) !=
        synthetic::DecodeError::none) {
      impl_->status = Status::source_invalid;
      impl_->report_value.status = impl_->status;
      return impl_->status;
    }
    const auto result = impl_->process_event(event);
    if (result != Status::ok) {
      impl_->status = result;
      impl_->report_value.status = result;
      return result;
    }
  }
  if (reader->finish() != synthetic::CaptureError::none) {
    impl_->status = Status::source_invalid;
    impl_->report_value.status = impl_->status;
    return impl_->status;
  }
  return impl_->finish();
}

const BacktestReport& EventBacktester::report() const noexcept {
  return impl_->report_value;
}

std::span<const SimulatedOrder> EventBacktester::orders() const noexcept {
  return impl_->orders;
}

std::span<const SimulatedFill> EventBacktester::fills() const noexcept {
  return impl_->fills;
}

} // namespace aegis::backtesting
