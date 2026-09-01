#include "aegis/market_data/synthetic/generator.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace aegis::market_data::synthetic {
namespace {

struct ActiveOrder {
  std::uint64_t id{};
  Side side{Side::none};
  std::int64_t price_ticks{};
  std::uint64_t quantity_units{};
};

struct InstrumentRuntime {
  std::int64_t mid_price_ticks{};
  std::uint64_t next_order_number{1U};
  std::array<ActiveOrder, kMaximumActiveOrdersPerInstrument> orders{};
  std::uint32_t order_count{};
  BookLevel hidden_level{};
  bool hidden_level_saved{false};
};

struct ChannelRuntime {
  std::uint32_t venue_number{};
  std::uint32_t channel_number{};
  std::uint64_t sequence{};
};

[[nodiscard]] bool add_i64(const std::int64_t left, const std::int64_t right,
                           std::int64_t& output) noexcept {
  if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
    return false;
  }
  output = left + right;
  return true;
}

[[nodiscard]] bool add_u64(const std::uint64_t left, const std::uint64_t right,
                           std::uint64_t& output) noexcept {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    return false;
  }
  output = left + right;
  return true;
}

[[nodiscard]] std::uint64_t interval_for_rate(const std::uint64_t rate) noexcept {
  return std::max<std::uint64_t>(1U, 1'000'000'000U / rate);
}

[[nodiscard]] std::uint32_t spread_for(const GeneratorConfig& config,
                                       const std::uint64_t ordinal) noexcept {
  const auto regime = ((ordinal - 1U) / config.spread_regime_period_events) % 3U;
  if (regime == 0U) {
    return config.tight_spread_ticks;
  }
  if (regime == 1U) {
    return config.normal_spread_ticks;
  }
  return config.wide_spread_ticks;
}

} // namespace

class SyntheticExchangeGenerator::Implementation {
public:
  Implementation(const GeneratorConfig& config, BookSet books) noexcept
      : config_(config), random_(config.seed), books_(books),
        exchange_time_ns_(config.session_start_exchange_time_ns),
        monotonic_time_ns_(config.process_monotonic_start_ns) {
    for (std::size_t index = 0; index < config_.instrument_count; ++index) {
      instruments_[index].mid_price_ticks =
          config_.instruments[index].initial_mid_price_ticks;
      const auto& instrument = config_.instruments[index];
      bool known_channel = false;
      for (std::size_t channel = 0; channel < channel_count_; ++channel) {
        known_channel =
            known_channel ||
            (channels_[channel].venue_number == instrument.venue_number &&
             channels_[channel].channel_number == instrument.channel_number);
      }
      if (!known_channel) {
        channels_[channel_count_++] = {
            .venue_number = instrument.venue_number,
            .channel_number = instrument.channel_number,
        };
      }
    }
  }

  [[nodiscard]] GenerationResult next(SyntheticEvent& output) noexcept {
    if (generated_ >= config_.event_count) {
      return {.error = GenerationError::complete};
    }
    const auto ordinal = generated_ + 1U;
    const auto instrument_index = select_instrument(ordinal);
    auto* channel = channel_for(config_.instruments[instrument_index]);
    if (channel == nullptr ||
        channel->sequence == std::numeric_limits<std::uint64_t>::max()) {
      return {.error = GenerationError::sequence_overflow};
    }
    const auto time_error = advance_time(ordinal);
    if (time_error != GenerationError::none) {
      return {.error = time_error};
    }

    auto event = base_event(instrument_index, ordinal, *channel);
    auto error = make_event(instrument_index, ordinal, event);
    if (error != GenerationError::none) {
      return {.error = error};
    }
    event.event_hash = calculate_event_hash(event);
    if (!valid_event_shape(event)) {
      return {.error = GenerationError::book_invariant_failure};
    }

    const auto book_result = books_.apply(event);
    if (book_result == BookApplyError::capacity_exhausted) {
      return {.error = GenerationError::book_capacity_exhausted};
    }
    const bool expected_cross = config_.scenario == Scenario::crossed_book_fault &&
                                book_result == BookApplyError::crossed_book;
    if (book_result != BookApplyError::none && !expected_cross) {
      return {.error = GenerationError::book_invariant_failure};
    }
    ++generated_;
    output = event;
    return {};
  }

  [[nodiscard]] const GeneratorConfig& config() const noexcept { return config_; }
  [[nodiscard]] const BookSet& books() const noexcept { return books_; }
  [[nodiscard]] std::uint64_t generated() const noexcept { return generated_; }

private:
  [[nodiscard]] std::size_t
  select_instrument(const std::uint64_t ordinal) const noexcept {
    const auto injection = scenario_injection_ordinal(config_);
    if (config_.scenario != Scenario::normal && ordinal >= injection &&
        ordinal <= injection + 4U) {
      return 0U;
    }
    return static_cast<std::size_t>((ordinal - 1U) % config_.instrument_count);
  }

  [[nodiscard]] GenerationError advance_time(const std::uint64_t ordinal) noexcept {
    std::uint64_t rate = config_.normal_message_rate_per_second;
    if (config_.scenario == Scenario::high_message_rate_burst) {
      const auto injection = scenario_injection_ordinal(config_);
      const auto burst_length = std::max<std::uint64_t>(1U, config_.event_count / 10U);
      if (ordinal >= injection && ordinal < injection + burst_length) {
        rate = config_.burst_message_rate_per_second;
        current_packet_flags_ = kPacketFlagBurst | kPacketFlagScenario;
      } else {
        current_packet_flags_ = 0U;
      }
    } else {
      current_packet_flags_ = 0U;
    }

    auto interval = interval_for_rate(rate);
    if (config_.scenario == Scenario::stale_feed &&
        ordinal == scenario_injection_ordinal(config_)) {
      if (!add_u64(interval, config_.stale_threshold_ns, interval)) {
        return GenerationError::time_overflow;
      }
      current_packet_flags_ = kPacketFlagAfterStaleGap | kPacketFlagScenario;
    }
    if (ordinal == 1U) {
      return GenerationError::none;
    }
    if (interval >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return GenerationError::time_overflow;
    }
    std::int64_t next_exchange{};
    if (!add_i64(exchange_time_ns_, static_cast<std::int64_t>(interval),
                 next_exchange) ||
        !add_u64(monotonic_time_ns_, interval, monotonic_time_ns_)) {
      return GenerationError::time_overflow;
    }
    exchange_time_ns_ = next_exchange;
    return GenerationError::none;
  }

  [[nodiscard]] ChannelRuntime*
  channel_for(const InstrumentConfig& instrument) noexcept {
    for (std::size_t index = 0; index < channel_count_; ++index) {
      if (channels_[index].venue_number == instrument.venue_number &&
          channels_[index].channel_number == instrument.channel_number) {
        return &channels_[index];
      }
    }
    return nullptr;
  }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] SyntheticEvent base_event(const std::size_t instrument_index,
                                          const std::uint64_t ordinal,
                                          ChannelRuntime& channel) noexcept {
    const auto& instrument = config_.instruments[instrument_index];
    ++channel.sequence;
    std::int64_t nic_time{};
    if (!add_i64(exchange_time_ns_, 100, nic_time)) {
      nic_time = exchange_time_ns_;
    }
    return {
        .data_quality = (current_packet_flags_ & kPacketFlagAfterStaleGap) != 0U
                            ? DataQuality::stale
                            : DataQuality::valid,
        .packet_flags = current_packet_flags_,
        .venue_number = instrument.venue_number,
        .channel_number = instrument.channel_number,
        .instrument_number = instrument.instrument_number,
        .channel_sequence = channel.sequence,
        .global_ordinal = ordinal,
        .exchange_event_time_ns = exchange_time_ns_,
        .nic_receive_time_ns = nic_time,
        .process_monotonic_time_ns = monotonic_time_ns_,
    };
  }

  [[nodiscard]] GenerationError make_event(const std::size_t instrument_index,
                                           const std::uint64_t ordinal,
                                           SyntheticEvent& event) noexcept {
    const auto bootstrap_events =
        2U * static_cast<std::uint64_t>(config_.instrument_count);
    if (ordinal <= bootstrap_events) {
      return make_bootstrap(instrument_index, ordinal, event);
    }
    if (exchange_time_ns_ >= config_.session_end_exchange_time_ns) {
      make_status(TradingStatus::closed, event);
      return GenerationError::none;
    }
    if (config_.auction_start_event != 0U && ordinal >= config_.auction_start_event &&
        ordinal <= config_.auction_end_event) {
      if (ordinal == config_.auction_start_event) {
        make_status(TradingStatus::auction, event);
      } else if (ordinal == config_.auction_end_event) {
        make_status(TradingStatus::open, event);
      } else {
        make_imbalance(instrument_index, event);
      }
      return GenerationError::none;
    }
    const auto special = make_scenario_event(instrument_index, ordinal, event);
    if (special.has_value()) {
      return *special;
    }
    return make_normal_event(instrument_index, event);
  }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] GenerationError make_bootstrap(const std::size_t instrument_index,
                                               const std::uint64_t ordinal,
                                               SyntheticEvent& event) noexcept {
    const auto side = ordinal <= config_.instrument_count ? Side::bid : Side::ask;
    const auto spread = static_cast<std::int64_t>(config_.normal_spread_ticks);
    auto& runtime = instruments_[instrument_index];
    const auto price = side == Side::bid ? runtime.mid_price_ticks - spread
                                         : runtime.mid_price_ticks + spread;
    const auto quantity = sample_quantity();
    if (config_.instruments[instrument_index].feed_mode == FeedMode::order_level) {
      return make_add(instrument_index, side, price, quantity, event);
    }
    event.type = NativeMessageType::price_level;
    event.side = side;
    event.action = BookAction::add;
    event.message_flags = kMessageFlagPriceLevel;
    event.price_ticks = price;
    event.quantity_units = quantity;
    event.level_quantity_units = quantity;
    event.order_count = 1U;
    return GenerationError::none;
  }

  // Scenario dispatch intentionally mirrors the documented state table.
  // NOLINTBEGIN(readability-function-cognitive-complexity,bugprone-easily-swappable-parameters)
  [[nodiscard]] std::optional<GenerationError>
  make_scenario_event(const std::size_t instrument_index, const std::uint64_t ordinal,
                      SyntheticEvent& event) noexcept {
    if (config_.scenario == Scenario::normal ||
        config_.scenario == Scenario::high_message_rate_burst ||
        config_.scenario == Scenario::duplicate_packet ||
        config_.scenario == Scenario::missing_sequence ||
        config_.scenario == Scenario::out_of_order_packet ||
        config_.scenario == Scenario::stale_feed) {
      return std::nullopt;
    }
    const auto injection = scenario_injection_ordinal(config_);
    if (ordinal < injection || ordinal > injection + 4U) {
      return std::nullopt;
    }
    auto& runtime = instruments_[instrument_index];
    event.packet_flags |= kPacketFlagScenario;
    event.message_flags |= kMessageFlagScenarioFault;

    switch (config_.scenario) {
    case Scenario::crossed_book_fault:
      if (ordinal != injection) {
        return std::nullopt;
      }
      return make_crossed(instrument_index, event);
    case Scenario::trading_halt:
      make_status(TradingStatus::halted, event);
      return GenerationError::none;
    case Scenario::reopening_auction:
      if (ordinal == injection) {
        make_status(TradingStatus::halted, event);
      } else if (ordinal == injection + 1U) {
        make_status(TradingStatus::pre_open, event);
      } else if (ordinal == injection + 2U) {
        make_status(TradingStatus::auction, event);
      } else if (ordinal == injection + 3U) {
        make_imbalance(instrument_index, event);
      } else {
        make_status(TradingStatus::open, event);
      }
      return GenerationError::none;
    case Scenario::earnings_shock:
      if (ordinal != injection) {
        return std::nullopt;
      }
      event.message_flags |= kMessageFlagShock;
      return make_shock_trade(instrument_index, 20, 5U, event);
    case Scenario::macro_shock:
      if (ordinal != injection) {
        return std::nullopt;
      }
      for (std::size_t index = 0; index < config_.instrument_count; ++index) {
        if (instruments_[index].mid_price_ticks <= 12) {
          return GenerationError::price_overflow;
        }
      }
      for (std::size_t index = 0; index < config_.instrument_count; ++index) {
        instruments_[index].mid_price_ticks -= 12;
      }
      event.message_flags |= kMessageFlagShock;
      event.type = NativeMessageType::trade;
      event.side = Side::bid;
      event.price_ticks = instruments_[instrument_index].mid_price_ticks;
      event.quantity_units = sample_quantity();
      if (event.quantity_units > std::numeric_limits<std::uint64_t>::max() / 7U) {
        return GenerationError::quantity_overflow;
      }
      event.quantity_units *= 7U;
      event.auxiliary_code = 2U;
      event.auxiliary_value = event.global_ordinal;
      return GenerationError::none;
    case Scenario::index_rebalance:
      if (ordinal != injection) {
        return std::nullopt;
      }
      event.message_flags |= kMessageFlagShock;
      return make_rebalance_trade(instrument_index, event);
    case Scenario::hidden_liquidity_replenishment:
      if (ordinal == injection) {
        return make_hidden_delete(instrument_index, event);
      }
      if (ordinal == injection + 1U) {
        return make_hidden_replenishment(instrument_index, event);
      }
      return std::nullopt;
    case Scenario::normal:
    case Scenario::high_message_rate_burst:
    case Scenario::duplicate_packet:
    case Scenario::missing_sequence:
    case Scenario::out_of_order_packet:
    case Scenario::stale_feed:
      return std::nullopt;
    }
    (void)runtime;
    return GenerationError::book_invariant_failure;
  }
  // NOLINTEND(readability-function-cognitive-complexity,bugprone-easily-swappable-parameters)

  // The branch order is the normative intensity precedence.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  [[nodiscard]] GenerationError make_normal_event(const std::size_t instrument_index,
                                                  SyntheticEvent& event) noexcept {
    auto& runtime = instruments_[instrument_index];
    maybe_move_mid(runtime);
    const auto* book = books_.find(event.venue_number, event.instrument_number);
    if (book == nullptr) {
      return GenerationError::book_invariant_failure;
    }
    if (book->status == TradingStatus::halted) {
      make_status(TradingStatus::halted, event);
      return GenerationError::none;
    }
    if (!book->crossed && random_.bounded(32U) == 0U) {
      make_quote(instrument_index, *book, event);
      return GenerationError::none;
    }
    const auto roll = static_cast<std::uint32_t>(random_.bounded(kPartsPerMillion));
    if (roll < config_.market_order_intensity_ppm) {
      make_trade(instrument_index, event);
      return GenerationError::none;
    }
    if (config_.instruments[instrument_index].feed_mode == FeedMode::order_level) {
      if (roll <
              config_.market_order_intensity_ppm + config_.cancellation_intensity_ppm &&
          runtime.order_count != 0U) {
        return make_cancel(instrument_index, event);
      }
      if (runtime.order_count != 0U && random_.bounded(4U) == 0U) {
        return make_modify(instrument_index, event);
      }
      if (runtime.order_count >= config_.maximum_active_orders_per_instrument) {
        return make_modify(instrument_index, event);
      }
      const auto side = random_.bounded(2U) == 0U ? Side::bid : Side::ask;
      const auto* level = side == Side::bid ? best_bid(*book) : best_ask(*book);
      const auto spread =
          static_cast<std::int64_t>(spread_for(config_, event.global_ordinal));
      auto price = level == nullptr ? runtime.mid_price_ticks +
                                          (side == Side::bid ? -spread : spread)
                                    : level->price_ticks;
      if (level == nullptr && side == Side::bid && best_ask(*book) != nullptr) {
        price = std::min(price, best_ask(*book)->price_ticks - 1);
      }
      if (level == nullptr && side == Side::ask && best_bid(*book) != nullptr) {
        price = std::max(price, best_bid(*book)->price_ticks + 1);
      }
      return make_add(instrument_index, side, price, sample_quantity(), event);
    }
    return make_price_level(instrument_index, roll, event);
  }

  [[nodiscard]] GenerationError make_add(const std::size_t instrument_index,
                                         const Side side, const std::int64_t price,
                                         const std::uint64_t quantity,
                                         SyntheticEvent& event) noexcept {
    auto& runtime = instruments_[instrument_index];
    if (runtime.order_count >= config_.maximum_active_orders_per_instrument) {
      return GenerationError::order_capacity_exhausted;
    }
    const auto* book = books_.find(event.venue_number, event.instrument_number);
    if (book == nullptr) {
      return GenerationError::book_invariant_failure;
    }
    const auto* existing = find_level(*book, side, price);
    const auto level_quantity = existing == nullptr ? 0U : existing->quantity_units;
    std::uint64_t next_quantity{};
    if (!add_u64(level_quantity, quantity, next_quantity)) {
      return GenerationError::quantity_overflow;
    }
    if (runtime.next_order_number >= (1ULL << 48U)) {
      return GenerationError::sequence_overflow;
    }
    const auto order_id = (static_cast<std::uint64_t>(instrument_index + 1U) << 48U) |
                          runtime.next_order_number++;
    runtime.orders[runtime.order_count++] = {
        .id = order_id,
        .side = side,
        .price_ticks = price,
        .quantity_units = quantity,
    };
    event.type = NativeMessageType::add_order;
    event.side = side;
    event.action = existing == nullptr ? BookAction::add : BookAction::change;
    event.message_flags |= kMessageFlagOrderLevel;
    event.synthetic_order_id = order_id;
    event.price_ticks = price;
    event.quantity_units = quantity;
    event.level_quantity_units = next_quantity;
    event.order_count = existing == nullptr ? 1U : existing->order_count + 1U;
    return GenerationError::none;
  }

  [[nodiscard]] GenerationError make_cancel(const std::size_t instrument_index,
                                            SyntheticEvent& event) noexcept {
    auto& runtime = instruments_[instrument_index];
    const auto selected =
        static_cast<std::uint32_t>(random_.bounded(runtime.order_count));
    const auto order = runtime.orders[selected];
    std::uint64_t remaining_quantity = 0U;
    std::uint32_t remaining_count = 0U;
    for (std::uint32_t index = 0; index < runtime.order_count; ++index) {
      if (index != selected && runtime.orders[index].side == order.side &&
          runtime.orders[index].price_ticks == order.price_ticks) {
        if (!add_u64(remaining_quantity, runtime.orders[index].quantity_units,
                     remaining_quantity)) {
          return GenerationError::quantity_overflow;
        }
        ++remaining_count;
      }
    }
    runtime.orders[selected] = runtime.orders[runtime.order_count - 1U];
    runtime.orders[runtime.order_count - 1U] = {};
    --runtime.order_count;
    event.type = NativeMessageType::cancel_order;
    event.side = order.side;
    event.action =
        remaining_count == 0U ? BookAction::delete_level : BookAction::change;
    event.message_flags |= kMessageFlagOrderLevel;
    event.synthetic_order_id = order.id;
    event.price_ticks = order.price_ticks;
    event.quantity_units = order.quantity_units;
    event.level_quantity_units = remaining_quantity;
    event.order_count = remaining_count;
    return GenerationError::none;
  }

  [[nodiscard]] GenerationError make_modify(const std::size_t instrument_index,
                                            SyntheticEvent& event) noexcept {
    auto& runtime = instruments_[instrument_index];
    if (runtime.order_count == 0U) {
      const auto side = random_.bounded(2U) == 0U ? Side::bid : Side::ask;
      return make_add(instrument_index, side, runtime.mid_price_ticks,
                      sample_quantity(), event);
    }
    const auto selected =
        static_cast<std::uint32_t>(random_.bounded(runtime.order_count));
    auto& order = runtime.orders[selected];
    const auto* book = books_.find(event.venue_number, event.instrument_number);
    const auto* existing =
        book == nullptr ? nullptr : find_level(*book, order.side, order.price_ticks);
    if (existing == nullptr || existing->quantity_units < order.quantity_units) {
      return GenerationError::book_invariant_failure;
    }
    const auto new_quantity = sample_quantity();
    const auto base_quantity = existing->quantity_units - order.quantity_units;
    std::uint64_t level_quantity{};
    if (!add_u64(base_quantity, new_quantity, level_quantity)) {
      return GenerationError::quantity_overflow;
    }
    order.quantity_units = new_quantity;
    event.type = NativeMessageType::modify_order;
    event.side = order.side;
    event.action = BookAction::change;
    event.message_flags |= kMessageFlagOrderLevel;
    event.synthetic_order_id = order.id;
    event.price_ticks = order.price_ticks;
    event.quantity_units = new_quantity;
    event.level_quantity_units = level_quantity;
    event.order_count = existing->order_count;
    return GenerationError::none;
  }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  [[nodiscard]] GenerationError make_price_level(const std::size_t instrument_index,
                                                 const std::uint32_t roll,
                                                 SyntheticEvent& event) noexcept {
    const auto* book = books_.find(event.venue_number, event.instrument_number);
    if (book == nullptr) {
      return GenerationError::book_invariant_failure;
    }
    const auto side = random_.bounded(2U) == 0U ? Side::bid : Side::ask;
    const auto* level = side == Side::bid ? best_bid(*book) : best_ask(*book);
    const auto cancellation_limit =
        config_.market_order_intensity_ppm + config_.cancellation_intensity_ppm;
    event.type = NativeMessageType::price_level;
    event.side = side;
    event.message_flags |= kMessageFlagPriceLevel;
    if (roll < cancellation_limit && level != nullptr) {
      event.action = BookAction::delete_level;
      event.price_ticks = level->price_ticks;
      return GenerationError::none;
    }
    const auto spread =
        static_cast<std::int64_t>(spread_for(config_, event.global_ordinal));
    event.price_ticks = level == nullptr
                            ? instruments_[instrument_index].mid_price_ticks +
                                  (side == Side::bid ? -spread : spread)
                            : level->price_ticks;
    if (level == nullptr && side == Side::bid && best_ask(*book) != nullptr) {
      event.price_ticks = std::min(event.price_ticks, best_ask(*book)->price_ticks - 1);
    }
    if (level == nullptr && side == Side::ask && best_bid(*book) != nullptr) {
      event.price_ticks = std::max(event.price_ticks, best_bid(*book)->price_ticks + 1);
    }
    event.action = level == nullptr ? BookAction::add : BookAction::change;
    event.quantity_units = sample_quantity();
    event.level_quantity_units = event.quantity_units;
    event.order_count = static_cast<std::uint32_t>(1U + random_.bounded(8U));
    return GenerationError::none;
  }

  void make_trade(const std::size_t instrument_index, SyntheticEvent& event) noexcept {
    const auto* book = books_.find(event.venue_number, event.instrument_number);
    const auto side = random_.bounded(2U) == 0U ? Side::bid : Side::ask;
    const BookLevel* level = nullptr;
    if (book != nullptr) {
      level = side == Side::bid ? best_bid(*book) : best_ask(*book);
    }
    event.type = NativeMessageType::trade;
    event.side = side;
    event.price_ticks = level == nullptr
                            ? instruments_[instrument_index].mid_price_ticks
                            : level->price_ticks;
    event.quantity_units = sample_quantity();
    event.auxiliary_value = event.global_ordinal;
  }

  void make_quote(const std::size_t instrument_index, const BookState& book,
                  SyntheticEvent& event) noexcept {
    const auto spread =
        static_cast<std::int64_t>(spread_for(config_, event.global_ordinal));
    const auto* bid = best_bid(book);
    const auto* ask = best_ask(book);
    event.type = NativeMessageType::quote;
    auto bid_price = bid == nullptr
                         ? instruments_[instrument_index].mid_price_ticks - spread
                         : bid->price_ticks;
    auto ask_price = ask == nullptr
                         ? instruments_[instrument_index].mid_price_ticks + spread
                         : ask->price_ticks;
    if (bid_price > ask_price) {
      if (bid == nullptr) {
        bid_price = ask_price - 1;
      } else {
        ask_price = bid_price + 1;
      }
    }
    event.price_ticks = bid_price;
    event.auxiliary_value = static_cast<std::uint64_t>(ask_price);
    event.quantity_units = bid == nullptr ? sample_quantity() : bid->quantity_units;
    event.level_quantity_units =
        ask == nullptr ? sample_quantity() : ask->quantity_units;
  }

  // NOLINTBEGIN(bugprone-easily-swappable-parameters)
  [[nodiscard]] GenerationError
  make_shock_trade(const std::size_t instrument_index, const std::int64_t tick_change,
                   const std::uint64_t quantity_multiplier,
                   SyntheticEvent& event) noexcept {
    auto& runtime = instruments_[instrument_index];
    std::int64_t shocked{};
    if (!add_i64(runtime.mid_price_ticks, tick_change, shocked) || shocked <= 0) {
      return GenerationError::price_overflow;
    }
    runtime.mid_price_ticks = shocked;
    event.type = NativeMessageType::trade;
    event.side = tick_change > 0 ? Side::ask : Side::bid;
    event.price_ticks = shocked;
    const auto base = sample_quantity();
    if (base > std::numeric_limits<std::uint64_t>::max() / quantity_multiplier) {
      return GenerationError::quantity_overflow;
    }
    event.quantity_units = base * quantity_multiplier;
    event.auxiliary_code = 1U;
    event.auxiliary_value = event.global_ordinal;
    return GenerationError::none;
  }
  // NOLINTEND(bugprone-easily-swappable-parameters)

  [[nodiscard]] GenerationError make_rebalance_trade(const std::size_t instrument_index,
                                                     SyntheticEvent& event) noexcept {
    return make_shock_trade(instrument_index, 1, 100U, event);
  }

  [[nodiscard]] GenerationError make_crossed(const std::size_t instrument_index,
                                             SyntheticEvent& event) noexcept {
    const auto* book = books_.find(event.venue_number, event.instrument_number);
    const auto* ask = book == nullptr ? nullptr : best_ask(*book);
    if (ask == nullptr ||
        ask->price_ticks == std::numeric_limits<std::int64_t>::max()) {
      return GenerationError::book_invariant_failure;
    }
    event.type = NativeMessageType::price_level;
    event.side = Side::bid;
    event.action = BookAction::add;
    event.data_quality = DataQuality::invalid;
    event.message_flags |= kMessageFlagPriceLevel;
    event.price_ticks = ask->price_ticks + 1;
    event.quantity_units = sample_quantity();
    event.level_quantity_units = event.quantity_units;
    event.order_count = 1U;
    (void)instrument_index;
    return GenerationError::none;
  }

  [[nodiscard]] GenerationError make_hidden_delete(const std::size_t instrument_index,
                                                   SyntheticEvent& event) noexcept {
    auto& runtime = instruments_[instrument_index];
    const auto* book = books_.find(event.venue_number, event.instrument_number);
    const auto* bid = book == nullptr ? nullptr : best_bid(*book);
    if (bid == nullptr) {
      return GenerationError::book_invariant_failure;
    }
    runtime.hidden_level = *bid;
    runtime.hidden_level_saved = true;
    event.type = NativeMessageType::price_level;
    event.side = Side::bid;
    event.action = BookAction::delete_level;
    event.message_flags |= kMessageFlagPriceLevel | kMessageFlagHiddenReplenishment;
    event.price_ticks = bid->price_ticks;
    return GenerationError::none;
  }

  [[nodiscard]] GenerationError
  make_hidden_replenishment(const std::size_t instrument_index,
                            SyntheticEvent& event) noexcept {
    auto& runtime = instruments_[instrument_index];
    if (!runtime.hidden_level_saved) {
      return GenerationError::book_invariant_failure;
    }
    event.type = NativeMessageType::price_level;
    event.side = Side::bid;
    event.action = BookAction::add;
    event.message_flags |= kMessageFlagPriceLevel | kMessageFlagHiddenReplenishment;
    event.price_ticks = runtime.hidden_level.price_ticks;
    event.quantity_units = runtime.hidden_level.quantity_units;
    event.level_quantity_units = runtime.hidden_level.quantity_units;
    event.order_count = runtime.hidden_level.order_count;
    runtime.hidden_level_saved = false;
    return GenerationError::none;
  }

  void make_imbalance(const std::size_t instrument_index,
                      SyntheticEvent& event) noexcept {
    event.type = NativeMessageType::auction_imbalance;
    event.side = random_.bounded(2U) == 0U ? Side::bid : Side::ask;
    event.price_ticks = instruments_[instrument_index].mid_price_ticks;
    event.quantity_units = sample_quantity();
    event.auxiliary_code = 1U;
    event.auxiliary_value = sample_quantity();
  }

  static void make_status(const TradingStatus status, SyntheticEvent& event) noexcept {
    const auto scenario_flags =
        static_cast<std::uint16_t>(event.message_flags & kMessageFlagScenarioFault);
    event = SyntheticEvent{
        .type = NativeMessageType::trading_status,
        .status = status,
        .data_quality = event.data_quality,
        .message_flags = scenario_flags,
        .packet_flags = event.packet_flags,
        .venue_number = event.venue_number,
        .channel_number = event.channel_number,
        .instrument_number = event.instrument_number,
        .channel_sequence = event.channel_sequence,
        .global_ordinal = event.global_ordinal,
        .exchange_event_time_ns = event.exchange_event_time_ns,
        .nic_receive_time_ns = event.nic_receive_time_ns,
        .process_monotonic_time_ns = event.process_monotonic_time_ns,
    };
  }

  void maybe_move_mid(InstrumentRuntime& runtime) noexcept {
    if (random_.bounded(kPartsPerMillion) >= config_.volatility_ppm) {
      return;
    }
    const auto direction = random_.bounded(2U) == 0U ? -1 : 1;
    const auto minimum_mid = static_cast<std::int64_t>(config_.wide_spread_ticks) + 2;
    if (runtime.mid_price_ticks > minimum_mid || direction > 0) {
      runtime.mid_price_ticks += direction;
    }
  }

  [[nodiscard]] std::uint64_t sample_quantity() noexcept {
    if (config_.order_size_distribution == OrderSizeDistribution::fixed) {
      return config_.minimum_order_quantity;
    }
    if (config_.order_size_distribution == OrderSizeDistribution::two_point) {
      return random_.bounded(2U) == 0U ? config_.minimum_order_quantity
                                       : config_.maximum_order_quantity;
    }
    const auto width = config_.maximum_order_quantity - config_.minimum_order_quantity;
    return config_.minimum_order_quantity + random_.bounded(width + 1U);
  }

  GeneratorConfig config_{};
  SplitMix64 random_;
  BookSet books_;
  std::array<InstrumentRuntime, kMaximumInstruments> instruments_{};
  std::array<ChannelRuntime, kMaximumInstruments> channels_{};
  std::size_t channel_count_{};
  std::uint64_t generated_{};
  std::int64_t exchange_time_ns_{};
  std::uint64_t monotonic_time_ns_{};
  std::uint32_t current_packet_flags_{};
};

std::optional<SyntheticExchangeGenerator>
SyntheticExchangeGenerator::create(const GeneratorConfig& config) {
  if (validate_config(config) != ConfigError::none) {
    return std::nullopt;
  }
  auto books = BookSet::create(config);
  if (!books.has_value()) {
    return std::nullopt;
  }
  return SyntheticExchangeGenerator(std::make_unique<Implementation>(config, *books));
}

SyntheticExchangeGenerator::SyntheticExchangeGenerator(
    std::unique_ptr<Implementation> implementation)
    : implementation_(std::move(implementation)) {}

SyntheticExchangeGenerator::SyntheticExchangeGenerator(
    SyntheticExchangeGenerator&&) noexcept = default;
SyntheticExchangeGenerator&
SyntheticExchangeGenerator::operator=(SyntheticExchangeGenerator&&) noexcept = default;
SyntheticExchangeGenerator::~SyntheticExchangeGenerator() = default;

GenerationResult SyntheticExchangeGenerator::next(SyntheticEvent& output) noexcept {
  return implementation_->next(output);
}

const GeneratorConfig& SyntheticExchangeGenerator::config() const noexcept {
  return implementation_->config();
}

const BookSet& SyntheticExchangeGenerator::expected_book() const noexcept {
  return implementation_->books();
}

std::uint64_t SyntheticExchangeGenerator::events_generated() const noexcept {
  return implementation_->generated();
}

std::size_t SyntheticExchangeGenerator::bounded_state_bytes() const noexcept {
  return sizeof(*implementation_);
}

std::string_view generation_error_name(const GenerationError error) noexcept {
  switch (error) {
  case GenerationError::none:
    return "none";
  case GenerationError::complete:
    return "complete";
  case GenerationError::invalid_config:
    return "invalid-config";
  case GenerationError::time_overflow:
    return "time-overflow";
  case GenerationError::sequence_overflow:
    return "sequence-overflow";
  case GenerationError::quantity_overflow:
    return "quantity-overflow";
  case GenerationError::price_overflow:
    return "price-overflow";
  case GenerationError::order_capacity_exhausted:
    return "order-capacity-exhausted";
  case GenerationError::book_capacity_exhausted:
    return "book-capacity-exhausted";
  case GenerationError::book_invariant_failure:
    return "book-invariant-failure";
  }
  return "invalid-error";
}

} // namespace aegis::market_data::synthetic
