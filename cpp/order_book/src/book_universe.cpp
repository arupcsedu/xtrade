#include "aegis/order_book/book_universe.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

namespace aegis::order_book {
namespace {

namespace synthetic = market_data::synthetic;

[[nodiscard]] ApplyError merge_level(std::array<Level, kMaximumPublishedDepth>& levels,
                                     std::uint32_t& count, const std::uint32_t capacity,
                                     const synthetic::Side side,
                                     const Level& incoming) noexcept {
  for (std::uint32_t index = 0U; index < count; ++index) {
    if (levels[index].price_ticks == incoming.price_ticks) {
      if (incoming.quantity_units > std::numeric_limits<std::uint64_t>::max() -
                                        levels[index].quantity_units ||
          incoming.order_count >
              std::numeric_limits<std::uint32_t>::max() - levels[index].order_count) {
        return ApplyError::quantity_overflow;
      }
      levels[index].quantity_units += incoming.quantity_units;
      levels[index].order_count += incoming.order_count;
      return ApplyError::none;
    }
  }

  std::uint32_t insertion = 0U;
  while (insertion < count) {
    const bool before = side == synthetic::Side::bid
                            ? levels[insertion].price_ticks > incoming.price_ticks
                            : levels[insertion].price_ticks < incoming.price_ticks;
    if (!before) {
      break;
    }
    ++insertion;
  }
  if (insertion >= capacity) {
    return ApplyError::none;
  }
  if (count < capacity) {
    ++count;
  }
  for (std::uint32_t cursor = count - 1U; cursor > insertion; --cursor) {
    levels[cursor] = levels[cursor - 1U];
  }
  levels[insertion] = incoming;
  return ApplyError::none;
}

} // namespace

BookUniverse::BookUniverse(const std::size_t capacity) noexcept
    : capacity_(std::min(capacity, kMaximumBooks)),
      owner_thread_(std::this_thread::get_id()) {}

ApplyError BookUniverse::register_book(const BookConfig& config) {
  if (!owns_current_thread()) {
    return ApplyError::wrong_thread;
  }
  if (!config.valid()) {
    return ApplyError::invalid_configuration;
  }
  if (find(config.identity) != nullptr) {
    return ApplyError::invalid_identity;
  }
  if (size_ >= capacity_) {
    return ApplyError::order_capacity_exhausted;
  }
  books_[size_] = std::make_unique<OrderBook>(config);
  ++size_;
  return ApplyError::none;
}

ApplyResult BookUniverse::apply(const BookIdentity& identity,
                                const BookUpdate& update) noexcept {
  if (!owns_current_thread()) {
    return {.error = ApplyError::wrong_thread};
  }
  auto* destination = find_mutable(identity);
  if (destination == nullptr) {
    return {.error = ApplyError::unknown_book};
  }
  if (update.operation == BookOperation::add && update.order_id.valid() &&
      duplicate_in_venue(*destination, update.order_id)) {
    return {.error = ApplyError::duplicate_order_id,
            .version = destination->version(),
            .last_valid_sequence = destination->last_valid_sequence(),
            .validity = destination->validity()};
  }
  return destination->apply(update);
}

ApplyError BookUniverse::load_snapshot(const BookSnapshot& snapshot) noexcept {
  if (!owns_current_thread()) {
    return ApplyError::wrong_thread;
  }
  auto* destination = find_mutable(snapshot.identity);
  if (destination == nullptr) {
    return ApplyError::unknown_book;
  }
  if (snapshot.mode == BookMode::order_by_order) {
    for (std::uint32_t index = 0U; index < snapshot.order_count; ++index) {
      if (duplicate_in_venue(*destination, snapshot.orders[index].order_id)) {
        return ApplyError::duplicate_order_id;
      }
    }
  }
  return destination->load_snapshot(snapshot).error;
}

ApplyError BookUniverse::session_reset_venue(
    const aegis::common::VenueId venue_id,
    const aegis::common::SessionId new_session_id) noexcept {
  if (!owns_current_thread()) {
    return ApplyError::wrong_thread;
  }
  if (!venue_id.valid() || !new_session_id.valid()) {
    return ApplyError::invalid_identity;
  }
  bool matched = false;
  for (std::size_t index = 0U; index < size_; ++index) {
    if (books_[index]->config().identity.venue_id == venue_id) {
      matched = true;
      const auto result = books_[index]->session_reset(new_session_id);
      if (!result.accepted()) {
        return result.error;
      }
    }
  }
  return matched ? ApplyError::none : ApplyError::unknown_book;
}

ApplyError
BookUniverse::consolidated_depth(const aegis::common::InstrumentId instrument_id,
                                 ConsolidatedDepth& output,
                                 const std::uint32_t requested_levels) const noexcept {
  output = {};
  if (!owns_current_thread()) {
    return ApplyError::wrong_thread;
  }
  if (!instrument_id.valid() || requested_levels == 0U ||
      requested_levels > kMaximumPublishedDepth) {
    return ApplyError::invalid_configuration;
  }
  bool matched = false;
  for (std::size_t index = 0U; index < size_; ++index) {
    const auto& book = *books_[index];
    if (book.config().identity.instrument_id != instrument_id) {
      continue;
    }
    matched = true;
    if (book.validity() != BookValidity::valid) {
      return ApplyError::consolidated_constituent_invalid;
    }
    if (requested_levels > book.config().published_depth) {
      return ApplyError::invalid_configuration;
    }
    DepthSnapshot depth{};
    const auto result = book.depth(depth, requested_levels);
    if (result != ApplyError::none) {
      return result;
    }
    for (std::uint32_t level = 0U; level < depth.bid_count; ++level) {
      const auto merged = merge_level(output.bids, output.bid_count, requested_levels,
                                      synthetic::Side::bid, depth.bids[level]);
      if (merged != ApplyError::none) {
        return merged;
      }
    }
    for (std::uint32_t level = 0U; level < depth.ask_count; ++level) {
      const auto merged = merge_level(output.asks, output.ask_count, requested_levels,
                                      synthetic::Side::ask, depth.asks[level]);
      if (merged != ApplyError::none) {
        return merged;
      }
    }
    ++output.contributing_venues;
  }
  if (!matched) {
    return ApplyError::unknown_book;
  }
  output.crossed = output.bid_count != 0U && output.ask_count != 0U &&
                   output.bids[0U].price_ticks > output.asks[0U].price_ticks;
  output.valid = !output.crossed;
  return output.crossed ? ApplyError::crossed_book : ApplyError::none;
}

const OrderBook* BookUniverse::find(const BookIdentity& identity) const noexcept {
  if (!owns_current_thread()) {
    return nullptr;
  }
  for (std::size_t index = 0U; index < size_; ++index) {
    if (books_[index]->config().identity == identity) {
      return books_[index].get();
    }
  }
  return nullptr;
}

const OrderBook*
BookUniverse::find(const aegis::common::VenueId venue_id,
                   const aegis::common::InstrumentId instrument_id) const noexcept {
  if (!owns_current_thread()) {
    return nullptr;
  }
  for (std::size_t index = 0U; index < size_; ++index) {
    const auto& identity = books_[index]->config().identity;
    if (identity.venue_id == venue_id && identity.instrument_id == instrument_id) {
      return books_[index].get();
    }
  }
  return nullptr;
}

OrderBook* BookUniverse::find_mutable(const BookIdentity& identity) noexcept {
  for (std::size_t index = 0U; index < size_; ++index) {
    if (books_[index]->config().identity == identity) {
      return books_[index].get();
    }
  }
  return nullptr;
}

bool BookUniverse::duplicate_in_venue(
    const OrderBook& destination,
    const aegis::common::OrderId order_id) const noexcept {
  for (std::size_t index = 0U; index < size_; ++index) {
    const auto& candidate = *books_[index];
    if (&candidate != &destination &&
        candidate.config().identity.venue_id ==
            destination.config().identity.venue_id &&
        candidate.session_id() == destination.session_id() &&
        candidate.find_order(order_id) != nullptr) {
      return true;
    }
  }
  return false;
}

std::size_t BookUniverse::size() const noexcept {
  assert(owns_current_thread());
  return size_;
}

bool BookUniverse::owns_current_thread() const noexcept {
  return owner_thread_ == std::this_thread::get_id();
}

} // namespace aegis::order_book
