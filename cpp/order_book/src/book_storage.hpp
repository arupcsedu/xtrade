#ifndef AEGIS_ORDER_BOOK_SRC_BOOK_STORAGE_HPP
#define AEGIS_ORDER_BOOK_SRC_BOOK_STORAGE_HPP

#include "aegis/order_book/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace aegis::order_book::detail {

inline constexpr std::uint32_t kEmptyIndexSlot = 0U;
inline constexpr std::uint32_t kTombstoneIndexSlot =
    std::numeric_limits<std::uint32_t>::max();

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct StoredLevel {
  Level aggregate;
  std::uint32_t head{kNoOrderIndex};
  std::uint32_t tail{kNoOrderIndex};
};

struct BookStorage {
  std::array<OrderState, kMaximumOrdersPerBook> orders{};
  // zero = empty, UINT32_MAX = tombstone, otherwise order array index + 1.
  std::array<std::uint32_t, kOrderIndexCapacity> order_index{};
  std::array<StoredLevel, kMaximumLevelsPerSide> bids{};
  std::array<StoredLevel, kMaximumLevelsPerSide> asks{};
  std::uint32_t bid_count{};
  std::uint32_t ask_count{};
  std::uint32_t live_order_count{};
  std::uint32_t free_head{kNoOrderIndex};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

} // namespace aegis::order_book::detail

#endif // AEGIS_ORDER_BOOK_SRC_BOOK_STORAGE_HPP
