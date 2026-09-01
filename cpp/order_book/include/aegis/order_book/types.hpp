#ifndef AEGIS_ORDER_BOOK_TYPES_HPP
#define AEGIS_ORDER_BOOK_TYPES_HPP

#include "aegis/common/identifiers.hpp"
#include "aegis/market_data/synthetic/event.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace aegis::order_book {

inline constexpr std::size_t kMaximumOrdersPerBook = 4'096U;
inline constexpr std::size_t kMaximumLevelsPerSide = 512U;
inline constexpr std::size_t kMaximumPublishedDepth = 64U;
inline constexpr std::size_t kMaximumBooks = 64U;
inline constexpr std::size_t kOrderIndexCapacity = 8'192U;
inline constexpr std::uint32_t kNoOrderIndex =
    std::numeric_limits<std::uint32_t>::max();

enum class BookMode : std::uint8_t {
  order_by_order = 1,
  price_level = 2,
};

enum class BookValidity : std::uint8_t {
  recovering = 1,
  valid = 2,
  invalid = 3,
};

enum class BookOperation : std::uint8_t {
  add = 1,
  cancel = 2,
  partial_cancel = 3,
  replace = 4,
  execute = 5,
  partial_execute = 6,
  delete_order = 7,
  trade = 8,
  set_level = 9,
  delete_level = 10,
  clear_side = 11,
  clear_book = 12,
  set_trading_status = 13,
  set_auction_imbalance = 14,
  observe_quote = 15,
};

enum class ApplyError : std::uint8_t {
  none = 0,
  wrong_thread,
  invalid_configuration,
  invalid_identity,
  invalid_session,
  invalid_sequence,
  sequence_gap,
  stale_sequence,
  book_invalid,
  invalid_operation,
  invalid_side,
  invalid_price,
  invalid_quantity,
  invalid_status,
  duplicate_order_id,
  order_not_found,
  quantity_exceeds_remaining,
  quantity_overflow,
  priority_overflow,
  order_capacity_exhausted,
  level_capacity_exhausted,
  level_not_found,
  mode_mismatch,
  update_while_halted,
  crossed_book,
  aggregate_mismatch,
  malformed_snapshot,
  snapshot_hash_mismatch,
  unknown_book,
  consolidated_constituent_invalid,
};

enum class InvariantError : std::uint8_t {
  none = 0,
  invalid_level,
  unsorted_bid,
  unsorted_ask,
  crossed,
  invalid_order,
  duplicate_order,
  missing_order_index,
  broken_queue,
  broken_free_list,
  aggregate_quantity_mismatch,
  aggregate_order_count_mismatch,
  capacity_exceeded,
};

// Strong external identifiers are canonical. The numeric fields are the
// license-clean SMX/1 routing keys and are never treated as interchangeable
// with canonical identifiers.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct BookIdentity {
  aegis::common::VenueId venue_id;
  aegis::common::InstrumentId instrument_id;
  std::uint32_t venue_number{};
  std::uint32_t instrument_number{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return venue_id.valid() && instrument_id.valid() && venue_number != 0U &&
           instrument_number != 0U;
  }

  auto operator<=>(const BookIdentity&) const = default;
};

struct BookConfig {
  BookIdentity identity;
  aegis::common::SessionId session_id;
  BookMode mode{BookMode::order_by_order};
  std::uint32_t order_capacity{1'024U};
  std::uint32_t level_capacity{128U};
  std::uint32_t published_depth{16U};
  std::uint64_t maximum_sequence{std::numeric_limits<std::uint64_t>::max()};
  bool permit_crossed_transition{false};
  bool require_contiguous_sequence{false};
  bool start_recovering{true};

  [[nodiscard]] constexpr bool valid() const noexcept {
    const bool valid_mode =
        mode == BookMode::order_by_order || mode == BookMode::price_level;
    return identity.valid() && session_id.valid() && valid_mode &&
           order_capacity != 0U && order_capacity <= kMaximumOrdersPerBook &&
           level_capacity != 0U && level_capacity <= kMaximumLevelsPerSide &&
           published_depth != 0U && published_depth <= kMaximumPublishedDepth &&
           published_depth <= level_capacity && maximum_sequence != 0U;
  }
};

struct Level {
  std::int64_t price_ticks{};
  std::uint64_t quantity_units{};
  std::uint32_t order_count{};
};

struct TopOfBook {
  Level bid;
  Level ask;
  bool has_bid{false};
  bool has_ask{false};
  std::uint64_t version{};
};

struct AuctionImbalanceState {
  market_data::synthetic::Side side{market_data::synthetic::Side::none};
  std::int64_t indicative_price_ticks{};
  std::uint64_t imbalance_quantity_units{};
  std::uint64_t paired_quantity_units{};
  std::uint64_t sequence{};
  bool active{false};
};

struct TradeState {
  market_data::synthetic::Side aggressor_side{market_data::synthetic::Side::none};
  std::int64_t price_ticks{};
  std::uint64_t quantity_units{};
  std::uint64_t venue_trade_id{};
  std::uint64_t sequence{};
  bool present{false};
};

struct BookUpdate {
  BookOperation operation{BookOperation::add};
  market_data::synthetic::Side side{market_data::synthetic::Side::none};
  market_data::synthetic::TradingStatus trading_status{
      market_data::synthetic::TradingStatus::none};
  aegis::common::OrderId order_id;
  std::int64_t price_ticks{};
  std::uint64_t quantity_units{};
  std::uint64_t level_quantity_units{};
  std::uint64_t paired_quantity_units{};
  std::uint64_t venue_trade_id{};
  std::uint32_t order_count{};
  std::uint32_t reason_code{};
  std::uint64_t sequence{};
  std::uint64_t process_monotonic_time_ns{};
  bool verify_aggregate{false};
};

struct ApplyResult {
  ApplyError error{ApplyError::none};
  std::uint64_t version{};
  std::uint64_t last_valid_sequence{};
  BookValidity validity{BookValidity::recovering};
  bool top_changed{false};

  [[nodiscard]] constexpr bool accepted() const noexcept {
    return error == ApplyError::none;
  }
};

struct OrderState {
  aegis::common::OrderId order_id;
  market_data::synthetic::Side side{market_data::synthetic::Side::none};
  std::int64_t price_ticks{};
  std::uint64_t quantity_units{};
  std::uint64_t priority{};
  std::uint32_t previous{kNoOrderIndex};
  std::uint32_t next{kNoOrderIndex};
  bool live{false};
};

struct DepthSnapshot {
  std::array<Level, kMaximumPublishedDepth> bids{};
  std::array<Level, kMaximumPublishedDepth> asks{};
  std::uint32_t bid_count{};
  std::uint32_t ask_count{};
  std::uint64_t version{};
  BookValidity validity{BookValidity::recovering};
};

struct SnapshotOrder {
  aegis::common::OrderId order_id;
  market_data::synthetic::Side side{market_data::synthetic::Side::none};
  std::int64_t price_ticks{};
  std::uint64_t quantity_units{};
  std::uint64_t priority{};
};

struct BookSnapshot {
  BookIdentity identity;
  aegis::common::SessionId session_id;
  BookMode mode{BookMode::order_by_order};
  market_data::synthetic::TradingStatus trading_status{
      market_data::synthetic::TradingStatus::none};
  AuctionImbalanceState auction_imbalance;
  TradeState last_trade;
  std::uint64_t version{};
  std::uint64_t last_valid_sequence{};
  std::uint64_t next_priority{};
  std::uint64_t snapshot_hash{};
  std::uint32_t order_count{};
  std::uint32_t bid_count{};
  std::uint32_t ask_count{};
  std::array<SnapshotOrder, kMaximumOrdersPerBook> orders{};
  std::array<Level, kMaximumLevelsPerSide> bids{};
  std::array<Level, kMaximumLevelsPerSide> asks{};
};

struct InvariantResult {
  InvariantError error{InvariantError::none};
  std::uint32_t index{};
  std::int64_t price_ticks{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return error == InvariantError::none;
  }
};

struct ConsolidatedDepth {
  std::array<Level, kMaximumPublishedDepth> bids{};
  std::array<Level, kMaximumPublishedDepth> asks{};
  std::uint32_t bid_count{};
  std::uint32_t ask_count{};
  std::uint32_t contributing_venues{};
  bool valid{false};
  bool crossed{false};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] constexpr bool valid_side(const market_data::synthetic::Side side) {
  return side == market_data::synthetic::Side::bid ||
         side == market_data::synthetic::Side::ask;
}

[[nodiscard]] constexpr bool valid_mode(const BookMode mode) noexcept {
  return mode == BookMode::order_by_order || mode == BookMode::price_level;
}

[[nodiscard]] constexpr std::uint64_t
next_sequence(const std::uint64_t sequence,
              const std::uint64_t maximum_sequence) noexcept {
  return sequence == maximum_sequence ? 1U : sequence + 1U;
}

[[nodiscard]] std::uint64_t snapshot_hash(const BookSnapshot& snapshot) noexcept;

static_assert(std::is_trivially_copyable_v<BookUpdate>);
static_assert(std::is_trivially_copyable_v<DepthSnapshot>);
static_assert(std::is_trivially_copyable_v<BookSnapshot>);

} // namespace aegis::order_book

#endif // AEGIS_ORDER_BOOK_TYPES_HPP
