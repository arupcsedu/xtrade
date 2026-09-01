#include "aegis/order_book/synthetic_adapter.hpp"

#include <cstdint>

namespace aegis::order_book {

aegis::common::OrderId
synthetic_order_id(const market_data::synthetic::SyntheticEvent& event) noexcept {
  const auto scope =
      (static_cast<std::uint64_t>(event.venue_number) << 32U) | event.instrument_number;
  return {scope, event.synthetic_order_id};
}

ApplyError synthetic_update(const market_data::synthetic::SyntheticEvent& event,
                            const BookMode mode, BookUpdate& output) noexcept {
  namespace synthetic = market_data::synthetic;
  output = {};
  if (!synthetic::valid_event_shape(event) || event.event_hash == 0U ||
      synthetic::calculate_event_hash(event) != event.event_hash ||
      event.data_quality != synthetic::DataQuality::valid || !valid_mode(mode)) {
    return ApplyError::invalid_operation;
  }
  output.side = event.side;
  output.price_ticks = event.price_ticks;
  output.quantity_units = event.quantity_units;
  output.level_quantity_units = event.level_quantity_units;
  output.order_count = event.order_count;
  output.sequence = event.channel_sequence;
  output.process_monotonic_time_ns = event.process_monotonic_time_ns;

  switch (event.type) {
  case synthetic::NativeMessageType::add_order:
    if (mode != BookMode::order_by_order) {
      return ApplyError::mode_mismatch;
    }
    output.operation = BookOperation::add;
    output.order_id = synthetic_order_id(event);
    output.verify_aggregate = true;
    return ApplyError::none;
  case synthetic::NativeMessageType::cancel_order:
    if (mode != BookMode::order_by_order) {
      return ApplyError::mode_mismatch;
    }
    output.operation = BookOperation::cancel;
    output.order_id = synthetic_order_id(event);
    output.verify_aggregate = true;
    return ApplyError::none;
  case synthetic::NativeMessageType::modify_order:
    if (mode != BookMode::order_by_order) {
      return ApplyError::mode_mismatch;
    }
    output.operation = BookOperation::replace;
    output.order_id = synthetic_order_id(event);
    output.verify_aggregate = true;
    return ApplyError::none;
  case synthetic::NativeMessageType::price_level:
    if (mode != BookMode::price_level) {
      return ApplyError::mode_mismatch;
    }
    switch (event.action) {
    case synthetic::BookAction::add:
    case synthetic::BookAction::change:
      output.operation = BookOperation::set_level;
      return ApplyError::none;
    case synthetic::BookAction::delete_level:
      output.operation = BookOperation::delete_level;
      return ApplyError::none;
    case synthetic::BookAction::clear_side:
      output.operation = BookOperation::clear_side;
      return ApplyError::none;
    case synthetic::BookAction::clear_book:
      output.operation = BookOperation::clear_book;
      return ApplyError::none;
    case synthetic::BookAction::none:
      return ApplyError::invalid_operation;
    }
    return ApplyError::invalid_operation;
  case synthetic::NativeMessageType::trade:
    output.operation = BookOperation::trade;
    output.venue_trade_id = event.auxiliary_value;
    return ApplyError::none;
  case synthetic::NativeMessageType::auction_imbalance:
    output.operation = BookOperation::set_auction_imbalance;
    output.paired_quantity_units = event.auxiliary_value;
    return ApplyError::none;
  case synthetic::NativeMessageType::trading_status:
    output.operation = BookOperation::set_trading_status;
    output.trading_status = event.status;
    return ApplyError::none;
  case synthetic::NativeMessageType::quote:
    output.operation = BookOperation::observe_quote;
    return ApplyError::none;
  }
  return ApplyError::invalid_operation;
}

ApplyResult
apply_synthetic(OrderBook& book, const aegis::common::SessionId session_id,
                const market_data::synthetic::SyntheticEvent& event) noexcept {
  if (!book.owns_current_thread()) {
    return {.error = ApplyError::wrong_thread};
  }
  const auto& config = book.config();
  if (session_id != book.session_id()) {
    return book.reject(ApplyError::invalid_session);
  }
  if (event.venue_number != config.identity.venue_number ||
      event.instrument_number != config.identity.instrument_number) {
    return book.reject(ApplyError::invalid_identity);
  }
  BookUpdate update{};
  const auto conversion = synthetic_update(event, config.mode, update);
  if (conversion != ApplyError::none) {
    return book.reject(conversion);
  }
  return book.apply(update);
}

} // namespace aegis::order_book
