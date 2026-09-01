#include "aegis/market_data/synthetic/book.hpp"

#include "aegis/market_data/synthetic/hash.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace aegis::market_data::synthetic {
namespace {

[[nodiscard]] constexpr bool is_book_message(const NativeMessageType type) noexcept {
  return type >= NativeMessageType::add_order && type <= NativeMessageType::price_level;
}

[[nodiscard]] std::array<BookLevel, kMaximumBookDepth>&
levels_for(BookState& book, const Side side) noexcept {
  return side == Side::bid ? book.bids : book.asks;
}

[[nodiscard]] const std::array<BookLevel, kMaximumBookDepth>&
levels_for(const BookState& book, const Side side) noexcept {
  return side == Side::bid ? book.bids : book.asks;
}

[[nodiscard]] std::uint32_t& count_for(BookState& book, const Side side) noexcept {
  return side == Side::bid ? book.bid_count : book.ask_count;
}

[[nodiscard]] std::uint32_t count_for(const BookState& book, const Side side) noexcept {
  return side == Side::bid ? book.bid_count : book.ask_count;
}

void erase_level(std::array<BookLevel, kMaximumBookDepth>& levels, std::uint32_t& count,
                 const std::uint32_t index) noexcept {
  for (std::uint32_t cursor = index + 1U; cursor < count; ++cursor) {
    levels[cursor - 1U] = levels[cursor];
  }
  --count;
  levels[count] = {};
}

[[nodiscard]] BookApplyError set_level(BookState& book, const Side side,
                                       const std::int64_t price_ticks,
                                       const std::uint64_t quantity_units,
                                       const std::uint32_t order_count) noexcept {
  auto& levels = levels_for(book, side);
  auto& count = count_for(book, side);
  for (std::uint32_t index = 0; index < count; ++index) {
    if (levels[index].price_ticks == price_ticks) {
      levels[index].quantity_units = quantity_units;
      levels[index].order_count = order_count;
      return BookApplyError::none;
    }
  }
  if (count >= kMaximumBookDepth) {
    return BookApplyError::capacity_exhausted;
  }
  std::uint32_t insertion = 0U;
  while (insertion < count) {
    const bool before = side == Side::bid ? levels[insertion].price_ticks > price_ticks
                                          : levels[insertion].price_ticks < price_ticks;
    if (!before) {
      break;
    }
    ++insertion;
  }
  for (std::uint32_t cursor = count; cursor > insertion; --cursor) {
    levels[cursor] = levels[cursor - 1U];
  }
  levels[insertion] = {.price_ticks = price_ticks,
                       .quantity_units = quantity_units,
                       .order_count = order_count};
  ++count;
  return BookApplyError::none;
}

[[nodiscard]] BookApplyError remove_level(BookState& book, const Side side,
                                          const std::int64_t price_ticks) noexcept {
  auto& levels = levels_for(book, side);
  auto& count = count_for(book, side);
  for (std::uint32_t index = 0; index < count; ++index) {
    if (levels[index].price_ticks == price_ticks) {
      erase_level(levels, count, index);
      return BookApplyError::none;
    }
  }
  return BookApplyError::none;
}

void clear_side(BookState& book, const Side side) noexcept {
  auto& levels = levels_for(book, side);
  auto& count = count_for(book, side);
  for (std::uint32_t index = 0; index < count; ++index) {
    levels[index] = {};
  }
  count = 0U;
}

[[nodiscard]] bool now_crossed(const BookState& book) noexcept {
  return book.bid_count > 0U && book.ask_count > 0U &&
         book.bids[0].price_ticks > book.asks[0].price_ticks;
}

} // namespace

std::optional<BookSet> BookSet::create(const GeneratorConfig& config) noexcept {
  if (validate_config(config) != ConfigError::none) {
    return std::nullopt;
  }
  BookSet result;
  result.size_ = config.instrument_count;
  for (std::size_t index = 0; index < result.size_; ++index) {
    result.books_[index].venue_number = config.instruments[index].venue_number;
    result.books_[index].instrument_number =
        config.instruments[index].instrument_number;
  }
  return result;
}

bool BookSet::register_instrument(const std::uint32_t venue_number,
                                  const std::uint32_t instrument_number) noexcept {
  if (find(venue_number, instrument_number) != nullptr) {
    return true;
  }
  if (size_ >= books_.size()) {
    return false;
  }
  books_[size_].venue_number = venue_number;
  books_[size_].instrument_number = instrument_number;
  ++size_;
  return true;
}

BookApplyError BookSet::apply(const SyntheticEvent& event) noexcept {
  auto* book = find_mutable(event.venue_number, event.instrument_number);
  if (book == nullptr) {
    return BookApplyError::unknown_instrument;
  }
  if (!valid_event_shape(event)) {
    return BookApplyError::invalid_event;
  }
  if (event.type == NativeMessageType::trading_status) {
    book->status = event.status;
    return BookApplyError::none;
  }
  if (!is_book_message(event.type)) {
    return BookApplyError::none;
  }
  if (book->status == TradingStatus::halted) {
    return BookApplyError::update_while_halted;
  }
  if (event.side != Side::bid && event.side != Side::ask) {
    return BookApplyError::invalid_side;
  }

  BookApplyError result = BookApplyError::none;
  switch (event.action) {
  case BookAction::add:
  case BookAction::change:
    result = set_level(*book, event.side, event.price_ticks, event.level_quantity_units,
                       event.order_count);
    break;
  case BookAction::delete_level:
    result = remove_level(*book, event.side, event.price_ticks);
    break;
  case BookAction::clear_side:
    clear_side(*book, event.side);
    break;
  case BookAction::clear_book:
    clear_side(*book, Side::bid);
    clear_side(*book, Side::ask);
    break;
  case BookAction::none:
    return BookApplyError::invalid_action;
  }
  if (result != BookApplyError::none) {
    return result;
  }
  if (now_crossed(*book)) {
    book->valid = false;
    book->crossed = true;
    return BookApplyError::crossed_book;
  }
  return BookApplyError::none;
}

const BookState* BookSet::find(const std::uint32_t venue_number,
                               const std::uint32_t instrument_number) const noexcept {
  for (std::size_t index = 0; index < size_; ++index) {
    if (books_[index].venue_number == venue_number &&
        books_[index].instrument_number == instrument_number) {
      return &books_[index];
    }
  }
  return nullptr;
}

BookState* BookSet::find_mutable(const std::uint32_t venue_number,
                                 const std::uint32_t instrument_number) noexcept {
  for (std::size_t index = 0; index < size_; ++index) {
    if (books_[index].venue_number == venue_number &&
        books_[index].instrument_number == instrument_number) {
      return &books_[index];
    }
  }
  return nullptr;
}

std::size_t BookSet::size() const noexcept { return size_; }

const BookState& BookSet::at(const std::size_t index) const noexcept {
  return books_[index];
}

std::uint64_t BookSet::stable_hash() const noexcept {
  StableHash64 hash;
  hash.add_u32(static_cast<std::uint32_t>(size_));
  for (std::size_t index = 0; index < size_; ++index) {
    const auto& book = books_[index];
    hash.add_u32(book.venue_number);
    hash.add_u32(book.instrument_number);
    hash.add_byte(static_cast<std::uint8_t>(book.status));
    hash.add_byte(static_cast<std::uint8_t>(book.valid));
    hash.add_byte(static_cast<std::uint8_t>(book.crossed));
    hash.add_u32(book.bid_count);
    for (std::uint32_t level = 0; level < book.bid_count; ++level) {
      hash.add_i64(book.bids[level].price_ticks);
      hash.add_u64(book.bids[level].quantity_units);
      hash.add_u32(book.bids[level].order_count);
    }
    hash.add_u32(book.ask_count);
    for (std::uint32_t level = 0; level < book.ask_count; ++level) {
      hash.add_i64(book.asks[level].price_ticks);
      hash.add_u64(book.asks[level].quantity_units);
      hash.add_u32(book.asks[level].order_count);
    }
  }
  return hash.value();
}

bool BookSet::all_valid() const noexcept {
  for (std::size_t index = 0; index < size_; ++index) {
    if (!books_[index].valid) {
      return false;
    }
  }
  return true;
}

const BookLevel* best_bid(const BookState& book) noexcept {
  return book.bid_count == 0U ? nullptr : book.bids.data();
}

const BookLevel* best_ask(const BookState& book) noexcept {
  return book.ask_count == 0U ? nullptr : book.asks.data();
}

const BookLevel* find_level(const BookState& book, const Side side,
                            const std::int64_t price_ticks) noexcept {
  if (side != Side::bid && side != Side::ask) {
    return nullptr;
  }
  const auto& levels = levels_for(book, side);
  const auto count = count_for(book, side);
  for (std::uint32_t index = 0; index < count; ++index) {
    if (levels[index].price_ticks == price_ticks) {
      return &levels[index];
    }
  }
  return nullptr;
}

} // namespace aegis::market_data::synthetic
