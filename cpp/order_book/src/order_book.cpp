#include "aegis/order_book/order_book.hpp"

#include "book_storage.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

namespace aegis::order_book {
namespace {

namespace synthetic = market_data::synthetic;
using detail::BookStorage;
using detail::StoredLevel;

[[nodiscard]] constexpr bool same_level(const Level& left,
                                        const Level& right) noexcept {
  return left.price_ticks == right.price_ticks &&
         left.quantity_units == right.quantity_units &&
         left.order_count == right.order_count;
}

[[nodiscard]] constexpr bool same_top(const TopOfBook& left,
                                      const TopOfBook& right) noexcept {
  return left.has_bid == right.has_bid && left.has_ask == right.has_ask &&
         (!left.has_bid || same_level(left.bid, right.bid)) &&
         (!left.has_ask || same_level(left.ask, right.ask));
}

[[nodiscard]] constexpr bool valid_status(const synthetic::TradingStatus status) {
  return status >= synthetic::TradingStatus::pre_open &&
         status <= synthetic::TradingStatus::closed;
}

[[nodiscard]] ApplyError validate_order_shape(const BookMode mode,
                                              const BookUpdate& update) noexcept {
  if (mode != BookMode::order_by_order) {
    return ApplyError::mode_mismatch;
  }
  if (!update.order_id.valid()) {
    return ApplyError::invalid_identity;
  }
  if (update.operation == BookOperation::cancel ||
      update.operation == BookOperation::delete_order) {
    return ApplyError::none;
  }
  if (update.operation == BookOperation::partial_cancel ||
      update.operation == BookOperation::execute ||
      update.operation == BookOperation::partial_execute) {
    return update.quantity_units == 0U ? ApplyError::invalid_quantity
                                       : ApplyError::none;
  }
  if (!valid_side(update.side)) {
    return ApplyError::invalid_side;
  }
  if (update.price_ticks <= 0) {
    return ApplyError::invalid_price;
  }
  return update.quantity_units == 0U ? ApplyError::invalid_quantity : ApplyError::none;
}

[[nodiscard]] ApplyError validate_level_shape(const BookMode mode,
                                              const BookUpdate& update) noexcept {
  if (update.operation == BookOperation::clear_book) {
    return ApplyError::none;
  }
  if (!valid_side(update.side)) {
    return ApplyError::invalid_side;
  }
  if (update.operation == BookOperation::clear_side) {
    return ApplyError::none;
  }
  if (mode != BookMode::price_level) {
    return ApplyError::mode_mismatch;
  }
  if (update.price_ticks <= 0) {
    return ApplyError::invalid_price;
  }
  if (update.operation == BookOperation::delete_level) {
    return ApplyError::none;
  }
  return update.level_quantity_units == 0U || update.order_count == 0U
             ? ApplyError::invalid_quantity
             : ApplyError::none;
}

[[nodiscard]] ApplyError validate_control_shape(const BookUpdate& update) noexcept {
  switch (update.operation) {
  case BookOperation::trade:
    if (!valid_side(update.side)) {
      return ApplyError::invalid_side;
    }
    if (update.price_ticks <= 0) {
      return ApplyError::invalid_price;
    }
    return update.quantity_units == 0U || update.venue_trade_id == 0U
               ? ApplyError::invalid_quantity
               : ApplyError::none;
  case BookOperation::set_trading_status:
    return valid_status(update.trading_status) ? ApplyError::none
                                               : ApplyError::invalid_status;
  case BookOperation::set_auction_imbalance:
    if (!valid_side(update.side)) {
      return ApplyError::invalid_side;
    }
    if (update.price_ticks <= 0) {
      return ApplyError::invalid_price;
    }
    return update.quantity_units == 0U ? ApplyError::invalid_quantity
                                       : ApplyError::none;
  case BookOperation::observe_quote:
    return ApplyError::none;
  default:
    return ApplyError::invalid_operation;
  }
}

[[nodiscard]] constexpr bool add_u64(const std::uint64_t left,
                                     const std::uint64_t right,
                                     std::uint64_t& result) noexcept {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

[[nodiscard]] constexpr std::uint64_t
identifier_hash(const aegis::common::OrderId id) noexcept {
  auto value = id.high() ^ std::rotl(id.low(), 29);
  value ^= value >> 30U;
  value *= 0xbf58'476d'1ce4'e5b9ULL;
  value ^= value >> 27U;
  value *= 0x94d0'49bb'1331'11ebULL;
  return value ^ (value >> 31U);
}

[[nodiscard]] const std::array<StoredLevel, kMaximumLevelsPerSide>&
levels_for(const BookStorage& storage, const synthetic::Side side) noexcept {
  return side == synthetic::Side::bid ? storage.bids : storage.asks;
}

[[nodiscard]] std::array<StoredLevel, kMaximumLevelsPerSide>&
levels_for(BookStorage& storage, const synthetic::Side side) noexcept {
  return side == synthetic::Side::bid ? storage.bids : storage.asks;
}

[[nodiscard]] std::uint32_t count_for(const BookStorage& storage,
                                      const synthetic::Side side) noexcept {
  return side == synthetic::Side::bid ? storage.bid_count : storage.ask_count;
}

[[nodiscard]] std::uint32_t& count_for(BookStorage& storage,
                                       const synthetic::Side side) noexcept {
  return side == synthetic::Side::bid ? storage.bid_count : storage.ask_count;
}

[[nodiscard]] std::uint32_t find_level_index(const BookStorage& storage,
                                             const synthetic::Side side,
                                             const std::int64_t price) noexcept {
  const auto& levels = levels_for(storage, side);
  const auto count = count_for(storage, side);
  for (std::uint32_t index = 0U; index < count; ++index) {
    if (levels[index].aggregate.price_ticks == price) {
      return index;
    }
  }
  return kNoOrderIndex;
}

[[nodiscard]] std::uint32_t insertion_index(const BookStorage& storage,
                                            const synthetic::Side side,
                                            const std::int64_t price) noexcept {
  const auto& levels = levels_for(storage, side);
  const auto count = count_for(storage, side);
  std::uint32_t index = 0U;
  while (index < count) {
    const auto current = levels[index].aggregate.price_ticks;
    const bool before =
        side == synthetic::Side::bid ? current > price : current < price;
    if (!before) {
      break;
    }
    ++index;
  }
  return index;
}

// Price and capacity have intentionally different units despite both being
// integer-representable.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] StoredLevel* insert_level(BookStorage& storage,
                                        const synthetic::Side side,
                                        const std::int64_t price,
                                        const std::uint32_t capacity) noexcept {
  auto& count = count_for(storage, side);
  if (count >= capacity) {
    return nullptr;
  }
  auto& levels = levels_for(storage, side);
  const auto index = insertion_index(storage, side, price);
  for (std::uint32_t cursor = count; cursor > index; --cursor) {
    levels[cursor] = levels[cursor - 1U];
  }
  levels[index] = {};
  levels[index].aggregate.price_ticks = price;
  ++count;
  return &levels[index];
}
// NOLINTEND(bugprone-easily-swappable-parameters)

void erase_level(BookStorage& storage, const synthetic::Side side,
                 const std::uint32_t index) noexcept {
  auto& levels = levels_for(storage, side);
  auto& count = count_for(storage, side);
  for (std::uint32_t cursor = index + 1U; cursor < count; ++cursor) {
    levels[cursor - 1U] = levels[cursor];
  }
  --count;
  levels[count] = {};
}

[[nodiscard]] std::uint32_t
lookup_order_index(const BookStorage& storage,
                   const aegis::common::OrderId id) noexcept {
  if (!id.valid()) {
    return kNoOrderIndex;
  }
  const auto first = identifier_hash(id) & (kOrderIndexCapacity - 1U);
  for (std::size_t probe = 0U; probe < kOrderIndexCapacity; ++probe) {
    const auto bucket = (first + probe) & (kOrderIndexCapacity - 1U);
    const auto entry = storage.order_index[bucket];
    if (entry == detail::kEmptyIndexSlot) {
      return kNoOrderIndex;
    }
    if (entry != detail::kTombstoneIndexSlot) {
      const auto index = entry - 1U;
      if (storage.orders[index].live && storage.orders[index].order_id == id) {
        return index;
      }
    }
  }
  return kNoOrderIndex;
}

[[nodiscard]] bool insert_order_index(BookStorage& storage,
                                      const aegis::common::OrderId id,
                                      const std::uint32_t order_index) noexcept {
  const auto first = identifier_hash(id) & (kOrderIndexCapacity - 1U);
  auto first_tombstone = kNoOrderIndex;
  for (std::size_t probe = 0U; probe < kOrderIndexCapacity; ++probe) {
    const auto bucket =
        static_cast<std::uint32_t>((first + probe) & (kOrderIndexCapacity - 1U));
    const auto entry = storage.order_index[bucket];
    if (entry == detail::kTombstoneIndexSlot && first_tombstone == kNoOrderIndex) {
      first_tombstone = bucket;
    }
    if (entry == detail::kEmptyIndexSlot) {
      const auto destination =
          first_tombstone == kNoOrderIndex ? bucket : first_tombstone;
      storage.order_index[destination] = order_index + 1U;
      return true;
    }
  }
  if (first_tombstone != kNoOrderIndex) {
    storage.order_index[first_tombstone] = order_index + 1U;
    return true;
  }
  return false;
}

void erase_order_index(BookStorage& storage, const aegis::common::OrderId id) noexcept {
  const auto first = identifier_hash(id) & (kOrderIndexCapacity - 1U);
  for (std::size_t probe = 0U; probe < kOrderIndexCapacity; ++probe) {
    const auto bucket = (first + probe) & (kOrderIndexCapacity - 1U);
    const auto entry = storage.order_index[bucket];
    if (entry == detail::kEmptyIndexSlot) {
      return;
    }
    if (entry != detail::kTombstoneIndexSlot &&
        storage.orders[entry - 1U].order_id == id) {
      storage.order_index[bucket] = detail::kTombstoneIndexSlot;
      return;
    }
  }
}

[[nodiscard]] std::uint32_t allocate_order_slot(BookStorage& storage) noexcept {
  if (storage.free_head == kNoOrderIndex) {
    return kNoOrderIndex;
  }
  const auto index = storage.free_head;
  storage.free_head = storage.orders[index].next;
  return index;
}

void clear_storage(BookStorage& storage, const std::uint32_t order_capacity) noexcept {
  storage.orders.fill({});
  storage.order_index.fill(detail::kEmptyIndexSlot);
  storage.bids.fill({});
  storage.asks.fill({});
  storage.bid_count = 0U;
  storage.ask_count = 0U;
  storage.live_order_count = 0U;
  storage.free_head = order_capacity == 0U ? kNoOrderIndex : 0U;
  for (std::uint32_t index = 0U; index < order_capacity; ++index) {
    storage.orders[index].next =
        index + 1U == order_capacity ? kNoOrderIndex : index + 1U;
  }
}

[[nodiscard]] bool would_cross(const BookStorage& storage, const synthetic::Side side,
                               const std::int64_t price) noexcept {
  if (side == synthetic::Side::bid) {
    return storage.ask_count != 0U && price > storage.asks[0U].aggregate.price_ticks;
  }
  return storage.bid_count != 0U && price < storage.bids[0U].aggregate.price_ticks;
}

[[nodiscard]] bool
would_cross_after_removal(const BookStorage& storage, const OrderState& removed,
                          const synthetic::Side replacement_side,
                          const std::int64_t replacement_price) noexcept {
  const auto opposite = replacement_side == synthetic::Side::bid ? synthetic::Side::ask
                                                                 : synthetic::Side::bid;
  const auto& levels = levels_for(storage, opposite);
  const auto count = count_for(storage, opposite);
  if (count == 0U) {
    return false;
  }
  std::uint32_t opposing_index = 0U;
  if (removed.side == opposite &&
      removed.price_ticks == levels[0U].aggregate.price_ticks &&
      levels[0U].aggregate.order_count == 1U) {
    opposing_index = 1U;
  }
  if (opposing_index >= count) {
    return false;
  }
  const auto opposing_price = levels[opposing_index].aggregate.price_ticks;
  return replacement_side == synthetic::Side::bid ? replacement_price > opposing_price
                                                  : replacement_price < opposing_price;
}

void unlink_order(BookStorage& storage, const std::uint32_t order_index,
                  StoredLevel& level) noexcept {
  const auto order = storage.orders[order_index];
  if (order.previous != kNoOrderIndex) {
    storage.orders[order.previous].next = order.next;
  } else {
    level.head = order.next;
  }
  if (order.next != kNoOrderIndex) {
    storage.orders[order.next].previous = order.previous;
  } else {
    level.tail = order.previous;
  }
}

void link_at_tail(BookStorage& storage, const std::uint32_t order_index,
                  StoredLevel& level) noexcept {
  auto& order = storage.orders[order_index];
  order.previous = level.tail;
  order.next = kNoOrderIndex;
  if (level.tail != kNoOrderIndex) {
    storage.orders[level.tail].next = order_index;
  } else {
    level.head = order_index;
  }
  level.tail = order_index;
}

void remove_order(BookStorage& storage, const std::uint32_t order_index) noexcept {
  const auto order = storage.orders[order_index];
  const auto level_index = find_level_index(storage, order.side, order.price_ticks);
  auto& level = levels_for(storage, order.side)[level_index];
  unlink_order(storage, order_index, level);
  level.aggregate.quantity_units -= order.quantity_units;
  --level.aggregate.order_count;
  erase_order_index(storage, order.order_id);
  storage.orders[order_index] = {};
  storage.orders[order_index].next = storage.free_head;
  storage.free_head = order_index;
  --storage.live_order_count;
  if (level.aggregate.order_count == 0U) {
    erase_level(storage, order.side, level_index);
  }
}

[[nodiscard]] ApplyError append_snapshot_order(BookStorage& storage,
                                               const BookConfig& config,
                                               const SnapshotOrder& input) noexcept {
  if (!input.order_id.valid() || !valid_side(input.side) || input.price_ticks <= 0 ||
      input.quantity_units == 0U || input.priority == 0U) {
    return ApplyError::malformed_snapshot;
  }
  if (lookup_order_index(storage, input.order_id) != kNoOrderIndex) {
    return ApplyError::duplicate_order_id;
  }
  auto level_index = find_level_index(storage, input.side, input.price_ticks);
  StoredLevel* level = nullptr;
  if (level_index == kNoOrderIndex) {
    level = insert_level(storage, input.side, input.price_ticks, config.level_capacity);
    if (level == nullptr) {
      return ApplyError::level_capacity_exhausted;
    }
  } else {
    level = &levels_for(storage, input.side)[level_index];
  }
  std::uint64_t aggregate{};
  if (!add_u64(level->aggregate.quantity_units, input.quantity_units, aggregate)) {
    return ApplyError::quantity_overflow;
  }
  const auto order_index = allocate_order_slot(storage);
  if (order_index == kNoOrderIndex) {
    return ApplyError::order_capacity_exhausted;
  }
  storage.orders[order_index] = {
      .order_id = input.order_id,
      .side = input.side,
      .price_ticks = input.price_ticks,
      .quantity_units = input.quantity_units,
      .priority = input.priority,
      .previous = kNoOrderIndex,
      .next = kNoOrderIndex,
      .live = true,
  };
  link_at_tail(storage, order_index, *level);
  level->aggregate.quantity_units = aggregate;
  ++level->aggregate.order_count;
  ++storage.live_order_count;
  if (!insert_order_index(storage, input.order_id, order_index)) {
    return ApplyError::order_capacity_exhausted;
  }
  return ApplyError::none;
}

[[nodiscard]] InvariantResult check_sorted_side(const BookStorage& storage,
                                                const synthetic::Side side) noexcept {
  const auto& levels = levels_for(storage, side);
  const auto count = count_for(storage, side);
  for (std::uint32_t index = 0U; index < count; ++index) {
    const auto& level = levels[index].aggregate;
    if (level.price_ticks <= 0 || level.quantity_units == 0U ||
        level.order_count == 0U) {
      return {.error = InvariantError::invalid_level,
              .index = index,
              .price_ticks = level.price_ticks};
    }
    if (index == 0U) {
      continue;
    }
    const auto previous = levels[index - 1U].aggregate.price_ticks;
    const bool sorted = side == synthetic::Side::bid ? previous > level.price_ticks
                                                     : previous < level.price_ticks;
    if (!sorted) {
      return {.error = side == synthetic::Side::bid ? InvariantError::unsorted_bid
                                                    : InvariantError::unsorted_ask,
              .index = index,
              .price_ticks = level.price_ticks};
    }
  }
  return {};
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] InvariantResult
check_level_queue(const BookStorage& storage, const BookConfig& config,
                  const synthetic::Side side, const std::uint32_t level_index,
                  std::uint32_t& observed_orders) noexcept {
  const auto& level = levels_for(storage, side)[level_index];
  std::uint64_t quantity = 0U;
  std::uint32_t orders = 0U;
  auto previous = kNoOrderIndex;
  std::uint64_t previous_priority = 0U;
  auto order_index = level.head;
  while (order_index != kNoOrderIndex) {
    if (order_index >= config.order_capacity || orders >= config.order_capacity) {
      return {.error = InvariantError::broken_queue,
              .index = level_index,
              .price_ticks = level.aggregate.price_ticks};
    }
    const auto& order = storage.orders[order_index];
    if (!order.live || order.side != side ||
        order.price_ticks != level.aggregate.price_ticks ||
        order.quantity_units == 0U || order.previous != previous ||
        order.priority <= previous_priority) {
      return {.error = InvariantError::invalid_order,
              .index = order_index,
              .price_ticks = order.price_ticks};
    }
    if (lookup_order_index(storage, order.order_id) != order_index) {
      return {.error = InvariantError::missing_order_index,
              .index = order_index,
              .price_ticks = order.price_ticks};
    }
    if (!add_u64(quantity, order.quantity_units, quantity)) {
      return {.error = InvariantError::aggregate_quantity_mismatch,
              .index = level_index,
              .price_ticks = level.aggregate.price_ticks};
    }
    previous = order_index;
    previous_priority = order.priority;
    order_index = order.next;
    ++orders;
    ++observed_orders;
  }
  if (previous != level.tail) {
    return {.error = InvariantError::broken_queue,
            .index = level_index,
            .price_ticks = level.aggregate.price_ticks};
  }
  if (quantity != level.aggregate.quantity_units) {
    return {.error = InvariantError::aggregate_quantity_mismatch,
            .index = level_index,
            .price_ticks = level.aggregate.price_ticks};
  }
  if (orders != level.aggregate.order_count) {
    return {.error = InvariantError::aggregate_order_count_mismatch,
            .index = level_index,
            .price_ticks = level.aggregate.price_ticks};
  }
  return {};
}

[[nodiscard]] InvariantResult
check_side_queues(const BookStorage& storage, const BookConfig& config,
                  const synthetic::Side side, std::uint32_t& observed_orders) noexcept {
  const auto count = count_for(storage, side);
  for (std::uint32_t level_index = 0U; level_index < count; ++level_index) {
    const auto result =
        check_level_queue(storage, config, side, level_index, observed_orders);
    if (!result.valid()) {
      return result;
    }
  }
  return {};
}
// NOLINTEND(bugprone-easily-swappable-parameters)

[[nodiscard]] InvariantResult
check_live_order_slots(const BookStorage& storage, const BookConfig& config) noexcept {
  for (std::uint32_t index = 0U; index < config.order_capacity; ++index) {
    const auto& order = storage.orders[index];
    if (order.live && (!order.order_id.valid() || order.quantity_units == 0U ||
                       !valid_side(order.side))) {
      return {.error = InvariantError::invalid_order,
              .index = index,
              .price_ticks = order.price_ticks};
    }
  }
  return {};
}

[[nodiscard]] InvariantResult check_free_list(const BookStorage& storage,
                                              const BookConfig& config) noexcept {
  std::uint32_t free_count = 0U;
  auto index = storage.free_head;
  while (index != kNoOrderIndex) {
    if (index >= config.order_capacity || storage.orders[index].live ||
        free_count >= config.order_capacity) {
      return {.error = InvariantError::broken_free_list, .index = index};
    }
    index = storage.orders[index].next;
    ++free_count;
  }
  return free_count + storage.live_order_count == config.order_capacity
             ? InvariantResult{}
             : InvariantResult{.error = InvariantError::broken_free_list};
}

class StableHash final {
public:
  void add_u8(const std::uint8_t value) noexcept {
    value_ ^= value;
    value_ *= 1'099'511'628'211ULL;
  }

  void add_u32(const std::uint32_t value) noexcept {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
      add_u8(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void add_u64(const std::uint64_t value) noexcept {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
      add_u8(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void add_i64(const std::int64_t value) noexcept {
    add_u64(static_cast<std::uint64_t>(value));
  }

  void add_identifier(const auto& value) noexcept {
    add_u64(value.high());
    add_u64(value.low());
  }

  [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

private:
  std::uint64_t value_{1'469'598'103'934'665'603ULL};
};

void hash_level(StableHash& hash, const Level& level) noexcept {
  hash.add_i64(level.price_ticks);
  hash.add_u64(level.quantity_units);
  hash.add_u32(level.order_count);
}

} // namespace

std::uint64_t snapshot_hash(const BookSnapshot& snapshot) noexcept {
  StableHash hash;
  hash.add_u32(1U);
  hash.add_identifier(snapshot.identity.venue_id);
  hash.add_identifier(snapshot.identity.instrument_id);
  hash.add_u32(snapshot.identity.venue_number);
  hash.add_u32(snapshot.identity.instrument_number);
  hash.add_identifier(snapshot.session_id);
  hash.add_u8(static_cast<std::uint8_t>(snapshot.mode));
  hash.add_u8(static_cast<std::uint8_t>(snapshot.trading_status));
  hash.add_u8(static_cast<std::uint8_t>(snapshot.auction_imbalance.side));
  hash.add_i64(snapshot.auction_imbalance.indicative_price_ticks);
  hash.add_u64(snapshot.auction_imbalance.imbalance_quantity_units);
  hash.add_u64(snapshot.auction_imbalance.paired_quantity_units);
  hash.add_u64(snapshot.auction_imbalance.sequence);
  hash.add_u8(static_cast<std::uint8_t>(snapshot.auction_imbalance.active));
  hash.add_u8(static_cast<std::uint8_t>(snapshot.last_trade.aggressor_side));
  hash.add_i64(snapshot.last_trade.price_ticks);
  hash.add_u64(snapshot.last_trade.quantity_units);
  hash.add_u64(snapshot.last_trade.venue_trade_id);
  hash.add_u64(snapshot.last_trade.sequence);
  hash.add_u8(static_cast<std::uint8_t>(snapshot.last_trade.present));
  hash.add_u64(snapshot.version);
  hash.add_u64(snapshot.last_valid_sequence);
  hash.add_u64(snapshot.next_priority);
  hash.add_u32(snapshot.order_count);
  for (std::uint32_t index = 0U; index < snapshot.order_count; ++index) {
    const auto& order = snapshot.orders[index];
    hash.add_identifier(order.order_id);
    hash.add_u8(static_cast<std::uint8_t>(order.side));
    hash.add_i64(order.price_ticks);
    hash.add_u64(order.quantity_units);
    hash.add_u64(order.priority);
  }
  hash.add_u32(snapshot.bid_count);
  for (std::uint32_t index = 0U; index < snapshot.bid_count; ++index) {
    hash_level(hash, snapshot.bids[index]);
  }
  hash.add_u32(snapshot.ask_count);
  for (std::uint32_t index = 0U; index < snapshot.ask_count; ++index) {
    hash_level(hash, snapshot.asks[index]);
  }
  return hash.value();
}

OrderBook::OrderBook(BookConfig config)
    : config_(config), session_id_(config.session_id),
      owner_thread_(std::this_thread::get_id()),
      active_(std::make_unique<BookStorage>()),
      staging_(std::make_unique<BookStorage>()),
      validity_(config.start_recovering ? BookValidity::recovering
                                        : BookValidity::valid),
      configuration_valid_(config.valid()) {
  const auto order_capacity = std::min(
      config_.order_capacity, static_cast<std::uint32_t>(kMaximumOrdersPerBook));
  clear_storage(*active_, order_capacity);
  clear_storage(*staging_, order_capacity);
  if (!configuration_valid_) {
    validity_ = BookValidity::invalid;
  }
}

OrderBook::~OrderBook() = default;

ApplyResult OrderBook::apply(const BookUpdate& update) noexcept {
  if (!owns_current_thread()) {
    return {.error = ApplyError::wrong_thread};
  }
  const auto previous_top = top_;
  const auto validation = validate_update(update);
  if (validation != ApplyError::none) {
    return fail(validation);
  }

  ApplyError result = ApplyError::invalid_operation;
  if (update.operation >= BookOperation::add &&
      update.operation <= BookOperation::delete_order) {
    result = apply_order_update(update);
  } else if (update.operation >= BookOperation::set_level &&
             update.operation <= BookOperation::clear_book) {
    result = apply_level_update(update);
  } else {
    result = apply_control_update(update);
  }
  if (result != ApplyError::none) {
    return fail(result);
  }
  if (update.verify_aggregate && update.operation >= BookOperation::add &&
      update.operation <= BookOperation::delete_order) {
    const auto* level = find_level(update.side, update.price_ticks);
    const bool expected_empty =
        update.level_quantity_units == 0U && update.order_count == 0U;
    const bool matches =
        expected_empty ? level == nullptr
                       : level != nullptr &&
                             level->quantity_units == update.level_quantity_units &&
                             level->order_count == update.order_count;
    if (!matches) {
      return fail(ApplyError::aggregate_mismatch);
    }
  }
  return success(previous_top, update.sequence);
}

ApplyResult OrderBook::reject(const ApplyError error) noexcept {
  if (!owns_current_thread()) {
    return {.error = ApplyError::wrong_thread};
  }
  return fail(error == ApplyError::none ? ApplyError::invalid_operation : error);
}

ApplyError OrderBook::validate_update(const BookUpdate& update) noexcept {
  if (!configuration_valid_) {
    return ApplyError::invalid_configuration;
  }
  if (validity_ == BookValidity::invalid) {
    return ApplyError::book_invalid;
  }
  const auto sequence_result = validate_sequence(update.sequence);
  if (sequence_result != ApplyError::none) {
    return sequence_result;
  }
  const auto state_result = validate_state(update.operation);
  if (state_result != ApplyError::none) {
    return state_result;
  }
  if (update.operation >= BookOperation::add &&
      update.operation <= BookOperation::delete_order) {
    return validate_order_shape(config_.mode, update);
  }
  if (update.operation >= BookOperation::set_level &&
      update.operation <= BookOperation::clear_book) {
    return validate_level_shape(config_.mode, update);
  }
  return validate_control_shape(update);
}

ApplyError OrderBook::validate_sequence(const std::uint64_t sequence) const noexcept {
  if (sequence == 0U || sequence > config_.maximum_sequence) {
    return ApplyError::invalid_sequence;
  }
  const auto expected =
      last_valid_sequence_ == 0U
          ? 1U
          : next_sequence(last_valid_sequence_, config_.maximum_sequence);
  if (last_valid_sequence_ == 0U && config_.require_contiguous_sequence &&
      sequence != 1U) {
    return ApplyError::sequence_gap;
  }
  if (last_valid_sequence_ != 0U && sequence != expected) {
    if (sequence == last_valid_sequence_) {
      return ApplyError::stale_sequence;
    }
    const auto forward = sequence >= expected
                             ? sequence - expected
                             : (config_.maximum_sequence - expected) + sequence;
    const auto backward = expected >= sequence
                              ? expected - sequence
                              : (config_.maximum_sequence - sequence) + expected;
    if (forward >= backward) {
      return ApplyError::stale_sequence;
    }
    if (config_.require_contiguous_sequence) {
      return ApplyError::sequence_gap;
    }
  }
  return ApplyError::none;
}

ApplyError OrderBook::validate_state(const BookOperation operation) const noexcept {
  if (trading_status_ == synthetic::TradingStatus::halted ||
      trading_status_ == synthetic::TradingStatus::closed) {
    const bool permitted = operation == BookOperation::set_trading_status ||
                           operation == BookOperation::clear_side ||
                           operation == BookOperation::clear_book ||
                           operation == BookOperation::set_auction_imbalance;
    if (!permitted) {
      return ApplyError::update_while_halted;
    }
  }
  return ApplyError::none;
}

ApplyError OrderBook::apply_order_update(const BookUpdate& update) noexcept {
  switch (update.operation) {
  case BookOperation::add:
    return add_order(update);
  case BookOperation::cancel:
  case BookOperation::delete_order:
    return remove_quantity(update, false, true);
  case BookOperation::partial_cancel:
    return remove_quantity(update, true, false);
  case BookOperation::execute:
    return remove_quantity(update, false, true);
  case BookOperation::partial_execute:
    return remove_quantity(update, true, false);
  case BookOperation::replace:
    return replace_order(update);
  default:
    return ApplyError::invalid_operation;
  }
}

ApplyError OrderBook::add_order(const BookUpdate& update) noexcept {
  if (lookup_order_index(*active_, update.order_id) != kNoOrderIndex) {
    return ApplyError::duplicate_order_id;
  }
  if (active_->live_order_count >= config_.order_capacity) {
    return ApplyError::order_capacity_exhausted;
  }
  if (next_priority_ == std::numeric_limits<std::uint64_t>::max()) {
    return ApplyError::priority_overflow;
  }
  if (!config_.permit_crossed_transition &&
      would_cross(*active_, update.side, update.price_ticks)) {
    return ApplyError::crossed_book;
  }
  auto level_index = find_level_index(*active_, update.side, update.price_ticks);
  StoredLevel* level = nullptr;
  if (level_index == kNoOrderIndex) {
    level =
        insert_level(*active_, update.side, update.price_ticks, config_.level_capacity);
    if (level == nullptr) {
      return ApplyError::level_capacity_exhausted;
    }
  } else {
    level = &levels_for(*active_, update.side)[level_index];
  }
  std::uint64_t aggregate{};
  if (!add_u64(level->aggregate.quantity_units, update.quantity_units, aggregate)) {
    if (level->aggregate.order_count == 0U) {
      erase_level(*active_, update.side,
                  find_level_index(*active_, update.side, update.price_ticks));
    }
    return ApplyError::quantity_overflow;
  }
  const auto order_index = allocate_order_slot(*active_);
  if (order_index == kNoOrderIndex) {
    return ApplyError::order_capacity_exhausted;
  }
  active_->orders[order_index] = {
      .order_id = update.order_id,
      .side = update.side,
      .price_ticks = update.price_ticks,
      .quantity_units = update.quantity_units,
      .priority = next_priority_++,
      .previous = kNoOrderIndex,
      .next = kNoOrderIndex,
      .live = true,
  };
  link_at_tail(*active_, order_index, *level);
  level->aggregate.quantity_units = aggregate;
  ++level->aggregate.order_count;
  ++active_->live_order_count;
  if (!insert_order_index(*active_, update.order_id, order_index)) {
    remove_order(*active_, order_index);
    return ApplyError::order_capacity_exhausted;
  }
  return ApplyError::none;
}

ApplyError OrderBook::remove_quantity(const BookUpdate& update,
                                      const bool require_partial,
                                      const bool require_full) noexcept {
  const auto order_index = lookup_order_index(*active_, update.order_id);
  if (order_index == kNoOrderIndex) {
    return ApplyError::order_not_found;
  }
  auto& order = active_->orders[order_index];
  const auto removal = (update.operation == BookOperation::cancel ||
                        update.operation == BookOperation::delete_order)
                           ? order.quantity_units
                           : update.quantity_units;
  if (removal > order.quantity_units ||
      (require_partial && removal >= order.quantity_units) ||
      (require_full && update.operation == BookOperation::execute &&
       removal != order.quantity_units)) {
    return ApplyError::quantity_exceeds_remaining;
  }
  if (removal == order.quantity_units) {
    remove_order(*active_, order_index);
    return ApplyError::none;
  }
  const auto level_index = find_level_index(*active_, order.side, order.price_ticks);
  auto& level = levels_for(*active_, order.side)[level_index];
  order.quantity_units -= removal;
  level.aggregate.quantity_units -= removal;
  return ApplyError::none;
}

ApplyError OrderBook::replace_order(const BookUpdate& update) noexcept {
  const auto order_index = lookup_order_index(*active_, update.order_id);
  if (order_index == kNoOrderIndex) {
    return ApplyError::order_not_found;
  }
  if (next_priority_ == std::numeric_limits<std::uint64_t>::max()) {
    return ApplyError::priority_overflow;
  }
  const auto old_order = active_->orders[order_index];
  if (!config_.permit_crossed_transition &&
      would_cross_after_removal(*active_, old_order, update.side, update.price_ticks)) {
    return ApplyError::crossed_book;
  }
  const auto old_level_index =
      find_level_index(*active_, old_order.side, old_order.price_ticks);
  auto& old_level = levels_for(*active_, old_order.side)[old_level_index];
  const bool same_price =
      old_order.side == update.side && old_order.price_ticks == update.price_ticks;
  if (same_price) {
    const auto base = old_level.aggregate.quantity_units - old_order.quantity_units;
    std::uint64_t aggregate{};
    if (!add_u64(base, update.quantity_units, aggregate)) {
      return ApplyError::quantity_overflow;
    }
    unlink_order(*active_, order_index, old_level);
    auto& order = active_->orders[order_index];
    order.quantity_units = update.quantity_units;
    order.priority = next_priority_++;
    link_at_tail(*active_, order_index, old_level);
    old_level.aggregate.quantity_units = aggregate;
    return ApplyError::none;
  }

  auto target_index = find_level_index(*active_, update.side, update.price_ticks);
  if (target_index == kNoOrderIndex) {
    const auto side_count = count_for(*active_, update.side);
    const bool old_level_will_disappear =
        old_order.side == update.side && old_level.aggregate.order_count == 1U;
    if (side_count >= config_.level_capacity && !old_level_will_disappear) {
      return ApplyError::level_capacity_exhausted;
    }
  } else {
    std::uint64_t ignored{};
    if (!add_u64(
            levels_for(*active_, update.side)[target_index].aggregate.quantity_units,
            update.quantity_units, ignored)) {
      return ApplyError::quantity_overflow;
    }
  }

  remove_order(*active_, order_index);
  auto replacement = update;
  const auto result = add_order(replacement);
  return result;
}

ApplyError OrderBook::apply_level_update(const BookUpdate& update) noexcept {
  if (update.operation == BookOperation::clear_book) {
    clear_all();
    return ApplyError::none;
  }
  if (update.operation == BookOperation::clear_side) {
    return clear_side(update.side);
  }
  if (config_.mode != BookMode::price_level) {
    return ApplyError::mode_mismatch;
  }
  const auto index = find_level_index(*active_, update.side, update.price_ticks);
  if (update.operation == BookOperation::delete_level) {
    if (index == kNoOrderIndex) {
      return ApplyError::level_not_found;
    }
    erase_level(*active_, update.side, index);
    return ApplyError::none;
  }
  if (update.operation != BookOperation::set_level) {
    return ApplyError::invalid_operation;
  }
  if (!config_.permit_crossed_transition &&
      would_cross(*active_, update.side, update.price_ticks)) {
    return ApplyError::crossed_book;
  }
  StoredLevel* level = nullptr;
  if (index == kNoOrderIndex) {
    level =
        insert_level(*active_, update.side, update.price_ticks, config_.level_capacity);
    if (level == nullptr) {
      return ApplyError::level_capacity_exhausted;
    }
  } else {
    level = &levels_for(*active_, update.side)[index];
  }
  level->aggregate.quantity_units = update.level_quantity_units;
  level->aggregate.order_count = update.order_count;
  return ApplyError::none;
}

ApplyError OrderBook::apply_control_update(const BookUpdate& update) noexcept {
  switch (update.operation) {
  case BookOperation::trade:
    last_trade_ = {
        .aggressor_side = update.side,
        .price_ticks = update.price_ticks,
        .quantity_units = update.quantity_units,
        .venue_trade_id = update.venue_trade_id,
        .sequence = update.sequence,
        .present = true,
    };
    return ApplyError::none;
  case BookOperation::set_trading_status:
    trading_status_ = update.trading_status;
    if (trading_status_ != synthetic::TradingStatus::auction) {
      auction_imbalance_.active = false;
    }
    return ApplyError::none;
  case BookOperation::set_auction_imbalance:
    auction_imbalance_ = {
        .side = update.side,
        .indicative_price_ticks = update.price_ticks,
        .imbalance_quantity_units = update.quantity_units,
        .paired_quantity_units = update.paired_quantity_units,
        .sequence = update.sequence,
        .active = true,
    };
    return ApplyError::none;
  case BookOperation::observe_quote:
    return ApplyError::none;
  default:
    return ApplyError::invalid_operation;
  }
}

ApplyError OrderBook::clear_side(const synthetic::Side side) noexcept {
  if (config_.mode == BookMode::order_by_order) {
    auto& levels = levels_for(*active_, side);
    auto& count = count_for(*active_, side);
    while (count != 0U) {
      auto order_index = levels[0U].head;
      while (order_index != kNoOrderIndex) {
        const auto next = active_->orders[order_index].next;
        erase_order_index(*active_, active_->orders[order_index].order_id);
        active_->orders[order_index] = {};
        active_->orders[order_index].next = active_->free_head;
        active_->free_head = order_index;
        --active_->live_order_count;
        order_index = next;
      }
      erase_level(*active_, side, 0U);
    }
  } else {
    auto& levels = levels_for(*active_, side);
    auto& count = count_for(*active_, side);
    for (std::uint32_t index = 0U; index < count; ++index) {
      levels[index] = {};
    }
    count = 0U;
  }
  return ApplyError::none;
}

void OrderBook::clear_all() noexcept {
  clear_storage(*active_, config_.order_capacity);
  top_ = {};
  auction_imbalance_ = {};
  last_trade_ = {};
  next_priority_ = 1U;
}

ApplyResult OrderBook::fail(const ApplyError error) noexcept {
  validity_ = BookValidity::invalid;
  return {.error = error,
          .version = version_,
          .last_valid_sequence = last_valid_sequence_,
          .validity = validity_,
          .top_changed = false};
}

ApplyResult OrderBook::success(const TopOfBook& previous_top,
                               const std::uint64_t sequence) noexcept {
  ++version_;
  last_valid_sequence_ = sequence;
  refresh_top();
  top_.version = version_;
  return {.error = ApplyError::none,
          .version = version_,
          .last_valid_sequence = last_valid_sequence_,
          .validity = validity_,
          .top_changed = !same_top(previous_top, top_)};
}

void OrderBook::refresh_top() noexcept {
  top_.has_bid = active_->bid_count != 0U;
  top_.has_ask = active_->ask_count != 0U;
  top_.bid = top_.has_bid ? active_->bids[0U].aggregate : Level{};
  top_.ask = top_.has_ask ? active_->asks[0U].aggregate : Level{};
  top_.version = version_;
}

ApplyResult OrderBook::begin_recovery() noexcept {
  if (!owns_current_thread()) {
    return {.error = ApplyError::wrong_thread};
  }
  clear_all();
  trading_status_ = synthetic::TradingStatus::pre_open;
  validity_ = BookValidity::recovering;
  last_valid_sequence_ = 0U;
  ++version_;
  top_.version = version_;
  return {.error = ApplyError::none,
          .version = version_,
          .last_valid_sequence = last_valid_sequence_,
          .validity = validity_,
          .top_changed = true};
}

ApplyResult OrderBook::complete_recovery(const std::uint64_t last_sequence) noexcept {
  if (!owns_current_thread()) {
    return {.error = ApplyError::wrong_thread};
  }
  if (!configuration_valid_ || last_sequence > config_.maximum_sequence) {
    return fail(ApplyError::invalid_sequence);
  }
  const auto invariant = check_storage(*active_);
  if (!invariant.valid()) {
    return fail(ApplyError::aggregate_mismatch);
  }
  last_valid_sequence_ = last_sequence;
  validity_ = BookValidity::valid;
  ++version_;
  refresh_top();
  return {.error = ApplyError::none,
          .version = version_,
          .last_valid_sequence = last_valid_sequence_,
          .validity = validity_,
          .top_changed = false};
}

ApplyResult
OrderBook::session_reset(const aegis::common::SessionId session_id) noexcept {
  if (!owns_current_thread()) {
    return {.error = ApplyError::wrong_thread};
  }
  if (!session_id.valid()) {
    return fail(ApplyError::invalid_session);
  }
  session_id_ = session_id;
  reference_version_ = {};
  return begin_recovery();
}

ApplyResult OrderBook::corporate_action_reset(
    const aegis::common::SessionId new_session_id,
    const aegis::common::ConfigurationVersion reference_version) noexcept {
  if (!owns_current_thread()) {
    return {.error = ApplyError::wrong_thread};
  }
  if (!new_session_id.valid() || !reference_version.valid()) {
    return fail(ApplyError::invalid_configuration);
  }
  session_id_ = new_session_id;
  reference_version_ = reference_version;
  return begin_recovery();
}

ApplyResult OrderBook::load_snapshot(const BookSnapshot& snapshot) noexcept {
  if (!owns_current_thread()) {
    return {.error = ApplyError::wrong_thread};
  }
  if (!configuration_valid_ || snapshot.identity != config_.identity ||
      snapshot.session_id != session_id_ || snapshot.mode != config_.mode ||
      snapshot.version == 0U ||
      snapshot.last_valid_sequence > config_.maximum_sequence ||
      !valid_status(snapshot.trading_status)) {
    return fail(ApplyError::malformed_snapshot);
  }
  if (snapshot.snapshot_hash == 0U ||
      snapshot_hash(snapshot) != snapshot.snapshot_hash) {
    return fail(ApplyError::snapshot_hash_mismatch);
  }
  clear_storage(*staging_, config_.order_capacity);
  const auto rebuilt = rebuild_snapshot(snapshot, *staging_);
  if (rebuilt != ApplyError::none) {
    return fail(rebuilt);
  }
  const auto invariant = check_storage(*staging_);
  if (!invariant.valid()) {
    return fail(ApplyError::aggregate_mismatch);
  }
  const auto previous_top = top_;
  std::swap(active_, staging_);
  trading_status_ = snapshot.trading_status;
  auction_imbalance_ = snapshot.auction_imbalance;
  last_trade_ = snapshot.last_trade;
  version_ = snapshot.version;
  last_valid_sequence_ = snapshot.last_valid_sequence;
  next_priority_ = snapshot.next_priority;
  validity_ = BookValidity::valid;
  refresh_top();
  return {.error = ApplyError::none,
          .version = version_,
          .last_valid_sequence = last_valid_sequence_,
          .validity = validity_,
          .top_changed = !same_top(previous_top, top_)};
}

ApplyError OrderBook::rebuild_snapshot(const BookSnapshot& snapshot,
                                       BookStorage& target) noexcept {
  if (snapshot.order_count > config_.order_capacity ||
      snapshot.bid_count > config_.level_capacity ||
      snapshot.ask_count > config_.level_capacity || snapshot.next_priority == 0U) {
    return ApplyError::malformed_snapshot;
  }
  if (snapshot.mode == BookMode::order_by_order) {
    return rebuild_order_snapshot(snapshot, target);
  }
  return rebuild_level_snapshot(snapshot, target);
}

ApplyError OrderBook::rebuild_order_snapshot(const BookSnapshot& snapshot,
                                             BookStorage& target) noexcept {
  for (std::uint32_t index = 0U; index < snapshot.order_count; ++index) {
    if (snapshot.orders[index].priority >= snapshot.next_priority) {
      return ApplyError::malformed_snapshot;
    }
    const auto result = append_snapshot_order(target, config_, snapshot.orders[index]);
    if (result != ApplyError::none) {
      return result;
    }
  }
  if (target.bid_count != snapshot.bid_count ||
      target.ask_count != snapshot.ask_count) {
    return ApplyError::aggregate_mismatch;
  }
  for (std::uint32_t index = 0U; index < snapshot.bid_count; ++index) {
    if (!same_level(target.bids[index].aggregate, snapshot.bids[index])) {
      return ApplyError::aggregate_mismatch;
    }
  }
  for (std::uint32_t index = 0U; index < snapshot.ask_count; ++index) {
    if (!same_level(target.asks[index].aggregate, snapshot.asks[index])) {
      return ApplyError::aggregate_mismatch;
    }
  }
  return ApplyError::none;
}

ApplyError OrderBook::rebuild_level_snapshot(const BookSnapshot& snapshot,
                                             BookStorage& target) noexcept {
  if (snapshot.order_count != 0U) {
    return ApplyError::malformed_snapshot;
  }
  for (std::uint32_t index = 0U; index < snapshot.bid_count; ++index) {
    target.bids[index].aggregate = snapshot.bids[index];
  }
  for (std::uint32_t index = 0U; index < snapshot.ask_count; ++index) {
    target.asks[index].aggregate = snapshot.asks[index];
  }
  target.bid_count = snapshot.bid_count;
  target.ask_count = snapshot.ask_count;
  return ApplyError::none;
}

const BookConfig& OrderBook::config() const noexcept {
  assert(owns_current_thread());
  return config_;
}

aegis::common::SessionId OrderBook::session_id() const noexcept {
  assert(owns_current_thread());
  return session_id_;
}

BookValidity OrderBook::validity() const noexcept {
  assert(owns_current_thread());
  return validity_;
}

synthetic::TradingStatus OrderBook::trading_status() const noexcept {
  assert(owns_current_thread());
  return trading_status_;
}

std::uint64_t OrderBook::version() const noexcept {
  assert(owns_current_thread());
  return version_;
}

std::uint64_t OrderBook::last_valid_sequence() const noexcept {
  assert(owns_current_thread());
  return last_valid_sequence_;
}

std::size_t OrderBook::live_order_count() const noexcept {
  assert(owns_current_thread());
  return active_->live_order_count;
}

std::size_t OrderBook::bid_level_count() const noexcept {
  assert(owns_current_thread());
  return active_->bid_count;
}

std::size_t OrderBook::ask_level_count() const noexcept {
  assert(owns_current_thread());
  return active_->ask_count;
}

const TopOfBook& OrderBook::top() const noexcept {
  assert(owns_current_thread());
  return top_;
}

const AuctionImbalanceState& OrderBook::auction_imbalance() const noexcept {
  assert(owns_current_thread());
  return auction_imbalance_;
}

const TradeState& OrderBook::last_trade() const noexcept {
  assert(owns_current_thread());
  return last_trade_;
}

const OrderState*
OrderBook::find_order(const aegis::common::OrderId id) const noexcept {
  if (!owns_current_thread()) {
    return nullptr;
  }
  const auto index = lookup_order_index(*active_, id);
  return index == kNoOrderIndex ? nullptr : &active_->orders[index];
}

const Level* OrderBook::find_level(const synthetic::Side side,
                                   const std::int64_t price_ticks) const noexcept {
  if (!owns_current_thread() || !valid_side(side)) {
    return nullptr;
  }
  const auto index = find_level_index(*active_, side, price_ticks);
  return index == kNoOrderIndex ? nullptr
                                : &levels_for(*active_, side)[index].aggregate;
}

ApplyError OrderBook::depth(DepthSnapshot& output,
                            const std::uint32_t requested_levels) const noexcept {
  if (!owns_current_thread()) {
    return ApplyError::wrong_thread;
  }
  if (requested_levels == 0U || requested_levels > config_.published_depth) {
    return ApplyError::invalid_configuration;
  }
  output = {};
  output.bid_count = std::min(requested_levels, active_->bid_count);
  output.ask_count = std::min(requested_levels, active_->ask_count);
  for (std::uint32_t index = 0U; index < output.bid_count; ++index) {
    output.bids[index] = active_->bids[index].aggregate;
  }
  for (std::uint32_t index = 0U; index < output.ask_count; ++index) {
    output.asks[index] = active_->asks[index].aggregate;
  }
  output.version = version_;
  output.validity = validity_;
  return ApplyError::none;
}

ApplyError OrderBook::make_snapshot(BookSnapshot& output) const noexcept {
  if (!owns_current_thread()) {
    return ApplyError::wrong_thread;
  }
  if (validity_ != BookValidity::valid) {
    return ApplyError::book_invalid;
  }
  output = {};
  output.identity = config_.identity;
  output.session_id = session_id_;
  output.mode = config_.mode;
  output.trading_status = trading_status_;
  output.auction_imbalance = auction_imbalance_;
  output.last_trade = last_trade_;
  output.version = version_;
  output.last_valid_sequence = last_valid_sequence_;
  output.next_priority = next_priority_;
  output.bid_count = active_->bid_count;
  output.ask_count = active_->ask_count;
  for (std::uint32_t index = 0U; index < output.bid_count; ++index) {
    output.bids[index] = active_->bids[index].aggregate;
  }
  for (std::uint32_t index = 0U; index < output.ask_count; ++index) {
    output.asks[index] = active_->asks[index].aggregate;
  }
  if (config_.mode == BookMode::order_by_order) {
    const auto append_side = [&](const synthetic::Side side) {
      const auto& levels = levels_for(*active_, side);
      const auto count = count_for(*active_, side);
      for (std::uint32_t level_index = 0U; level_index < count; ++level_index) {
        auto order_index = levels[level_index].head;
        while (order_index != kNoOrderIndex) {
          const auto& order = active_->orders[order_index];
          output.orders[output.order_count++] = {
              .order_id = order.order_id,
              .side = order.side,
              .price_ticks = order.price_ticks,
              .quantity_units = order.quantity_units,
              .priority = order.priority,
          };
          order_index = order.next;
        }
      }
    };
    append_side(synthetic::Side::bid);
    append_side(synthetic::Side::ask);
  }
  output.snapshot_hash = snapshot_hash(output);
  return ApplyError::none;
}

InvariantResult OrderBook::check_storage(const BookStorage& storage) const noexcept {
  if (storage.bid_count > config_.level_capacity ||
      storage.ask_count > config_.level_capacity ||
      storage.live_order_count > config_.order_capacity) {
    return {.error = InvariantError::capacity_exceeded};
  }
  auto result = check_sorted_side(storage, synthetic::Side::bid);
  if (!result.valid()) {
    return result;
  }
  result = check_sorted_side(storage, synthetic::Side::ask);
  if (!result.valid()) {
    return result;
  }
  if (!config_.permit_crossed_transition && storage.bid_count != 0U &&
      storage.ask_count != 0U &&
      storage.bids[0U].aggregate.price_ticks > storage.asks[0U].aggregate.price_ticks) {
    return {.error = InvariantError::crossed};
  }
  if (config_.mode == BookMode::price_level) {
    return storage.live_order_count == 0U
               ? check_free_list(storage, config_)
               : InvariantResult{.error = InvariantError::invalid_order};
  }

  std::uint32_t observed_orders = 0U;
  result = check_side_queues(storage, config_, synthetic::Side::bid, observed_orders);
  if (!result.valid()) {
    return result;
  }
  result = check_side_queues(storage, config_, synthetic::Side::ask, observed_orders);
  if (!result.valid()) {
    return result;
  }
  if (observed_orders != storage.live_order_count) {
    return {.error = InvariantError::aggregate_order_count_mismatch};
  }
  result = check_live_order_slots(storage, config_);
  return result.valid() ? check_free_list(storage, config_) : result;
}

InvariantResult OrderBook::check_invariants() const noexcept {
  if (!owns_current_thread()) {
    return {.error = InvariantError::invalid_order};
  }
  return check_storage(*active_);
}

bool OrderBook::owns_current_thread() const noexcept {
  return owner_thread_ == std::this_thread::get_id();
}

} // namespace aegis::order_book
