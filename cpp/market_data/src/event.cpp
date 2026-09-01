#include "aegis/market_data/synthetic/event.hpp"

#include <cstdint>
#include <limits>
#include <string_view>

namespace aegis::market_data::synthetic {
namespace {

[[nodiscard]] constexpr bool valid_side(const Side side) noexcept {
  return side == Side::none || side == Side::bid || side == Side::ask;
}

[[nodiscard]] constexpr bool valid_action(const BookAction action) noexcept {
  return action >= BookAction::none && action <= BookAction::clear_book;
}

[[nodiscard]] constexpr bool valid_status(const TradingStatus status) noexcept {
  return status >= TradingStatus::none && status <= TradingStatus::closed;
}

[[nodiscard]] constexpr bool valid_quality(const DataQuality quality) noexcept {
  return quality >= DataQuality::valid && quality <= DataQuality::invalid;
}

[[nodiscard]] constexpr bool is_book_message(const NativeMessageType type) noexcept {
  return type >= NativeMessageType::add_order && type <= NativeMessageType::price_level;
}

[[nodiscard]] bool valid_common_shape(const SyntheticEvent& event) noexcept {
  return event.venue_number != 0U && event.channel_number != 0U &&
         event.instrument_number != 0U && event.channel_sequence != 0U &&
         event.global_ordinal != 0U && event.exchange_event_time_ns > 0 &&
         event.nic_receive_time_ns >= event.exchange_event_time_ns &&
         event.process_monotonic_time_ns != 0U &&
         (event.message_flags & ~kKnownMessageFlags) == 0U &&
         (event.packet_flags & ~kKnownPacketFlags) == 0U && valid_side(event.side) &&
         valid_action(event.action) && valid_status(event.status) &&
         valid_quality(event.data_quality);
}

[[nodiscard]] bool valid_book_shape(const SyntheticEvent& event) noexcept {
  if ((event.side != Side::bid && event.side != Side::ask) ||
      event.status != TradingStatus::none || event.price_ticks <= 0 ||
      event.action == BookAction::none) {
    return false;
  }
  const bool deletion = event.action == BookAction::delete_level ||
                        event.action == BookAction::clear_side ||
                        event.action == BookAction::clear_book;
  if (!deletion && (event.level_quantity_units == 0U || event.order_count == 0U)) {
    return false;
  }
  const bool order_level = event.type != NativeMessageType::price_level;
  const bool has_order_flag = (event.message_flags & kMessageFlagOrderLevel) != 0U;
  const bool has_price_flag = (event.message_flags & kMessageFlagPriceLevel) != 0U;
  if (order_level) {
    return event.synthetic_order_id != 0U && has_order_flag && !has_price_flag;
  }
  return event.synthetic_order_id == 0U && has_price_flag && !has_order_flag;
}

[[nodiscard]] bool valid_nonbook_shape(const SyntheticEvent& event) noexcept {
  if (event.action != BookAction::none || event.synthetic_order_id != 0U) {
    return false;
  }
  switch (event.type) {
  case NativeMessageType::trade:
    return (event.side == Side::bid || event.side == Side::ask) &&
           event.status == TradingStatus::none && event.price_ticks > 0 &&
           event.quantity_units > 0U && event.auxiliary_value > 0U;
  case NativeMessageType::quote: {
    if (event.auxiliary_value >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
    const auto ask_price = static_cast<std::int64_t>(event.auxiliary_value);
    return event.side == Side::none && event.status == TradingStatus::none &&
           event.price_ticks > 0 && ask_price >= event.price_ticks &&
           event.quantity_units > 0U && event.level_quantity_units > 0U;
  }
  case NativeMessageType::auction_imbalance:
    return event.status == TradingStatus::none && event.price_ticks > 0 &&
           event.quantity_units > 0U;
  case NativeMessageType::trading_status:
    return event.side == Side::none && event.status != TradingStatus::none;
  case NativeMessageType::add_order:
  case NativeMessageType::cancel_order:
  case NativeMessageType::modify_order:
  case NativeMessageType::price_level:
    return false;
  }
  return false;
}

} // namespace

std::uint64_t calculate_event_hash(const SyntheticEvent& event) noexcept {
  StableHash64 hash;
  hash.add_u16(108U);
  hash.add_byte(static_cast<std::uint8_t>(event.type));
  hash.add_byte(static_cast<std::uint8_t>(event.side));
  hash.add_byte(static_cast<std::uint8_t>(event.action));
  hash.add_byte(static_cast<std::uint8_t>(event.status));
  hash.add_u16(event.message_flags);
  hash.add_u32(event.instrument_number);
  hash.add_u64(event.channel_sequence);
  hash.add_u64(event.global_ordinal);
  hash.add_i64(event.exchange_event_time_ns);
  hash.add_i64(event.nic_receive_time_ns);
  hash.add_u64(event.process_monotonic_time_ns);
  hash.add_u64(event.synthetic_order_id);
  hash.add_i64(event.price_ticks);
  hash.add_u64(event.quantity_units);
  hash.add_u64(event.level_quantity_units);
  hash.add_u32(event.order_count);
  hash.add_u32(event.auxiliary_code);
  hash.add_u64(event.auxiliary_value);
  return hash.value();
}

bool valid_event_shape(const SyntheticEvent& event) noexcept {
  if (!valid_common_shape(event)) {
    return false;
  }
  if (is_book_message(event.type)) {
    return valid_book_shape(event);
  }
  return valid_nonbook_shape(event);
}

std::string_view message_type_name(const NativeMessageType type) noexcept {
  switch (type) {
  case NativeMessageType::add_order:
    return "ADD_ORDER";
  case NativeMessageType::cancel_order:
    return "CANCEL_ORDER";
  case NativeMessageType::modify_order:
    return "MODIFY_ORDER";
  case NativeMessageType::price_level:
    return "PRICE_LEVEL";
  case NativeMessageType::trade:
    return "TRADE";
  case NativeMessageType::quote:
    return "QUOTE";
  case NativeMessageType::auction_imbalance:
    return "AUCTION_IMBALANCE";
  case NativeMessageType::trading_status:
    return "TRADING_STATUS";
  }
  return "INVALID";
}

std::string_view side_name(const Side side) noexcept {
  switch (side) {
  case Side::none:
    return "NONE";
  case Side::bid:
    return "BID";
  case Side::ask:
    return "ASK";
  }
  return "INVALID";
}

std::string_view action_name(const BookAction action) noexcept {
  switch (action) {
  case BookAction::none:
    return "NONE";
  case BookAction::add:
    return "ADD";
  case BookAction::change:
    return "CHANGE";
  case BookAction::delete_level:
    return "DELETE";
  case BookAction::clear_side:
    return "CLEAR_SIDE";
  case BookAction::clear_book:
    return "CLEAR_BOOK";
  }
  return "INVALID";
}

std::string_view status_name(const TradingStatus status) noexcept {
  switch (status) {
  case TradingStatus::none:
    return "NONE";
  case TradingStatus::pre_open:
    return "PRE_OPEN";
  case TradingStatus::open:
    return "OPEN";
  case TradingStatus::halted:
    return "HALTED";
  case TradingStatus::auction:
    return "AUCTION";
  case TradingStatus::closed:
    return "CLOSED";
  }
  return "INVALID";
}

} // namespace aegis::market_data::synthetic
