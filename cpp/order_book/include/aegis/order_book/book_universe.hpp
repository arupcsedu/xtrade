#ifndef AEGIS_ORDER_BOOK_BOOK_UNIVERSE_HPP
#define AEGIS_ORDER_BOOK_BOOK_UNIVERSE_HPP

#include "aegis/order_book/order_book.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace aegis::order_book {

class BookUniverse final {
public:
  explicit BookUniverse(std::size_t capacity = kMaximumBooks) noexcept;

  BookUniverse(const BookUniverse&) = delete;
  BookUniverse& operator=(const BookUniverse&) = delete;
  BookUniverse(BookUniverse&&) = delete;
  BookUniverse& operator=(BookUniverse&&) = delete;

  // Registration is initialization-plane work and may allocate one pre-sized
  // book. apply(), queries, reset, and consolidation never allocate.
  [[nodiscard]] ApplyError register_book(const BookConfig& config);
  [[nodiscard]] ApplyResult apply(const BookIdentity& identity,
                                  const BookUpdate& update) noexcept;
  [[nodiscard]] ApplyError load_snapshot(const BookSnapshot& snapshot) noexcept;
  [[nodiscard]] ApplyError
  session_reset_venue(aegis::common::VenueId venue_id,
                      aegis::common::SessionId new_session_id) noexcept;
  [[nodiscard]] ApplyError
  consolidated_depth(aegis::common::InstrumentId instrument_id,
                     ConsolidatedDepth& output,
                     std::uint32_t requested_levels) const noexcept;

  [[nodiscard]] const OrderBook* find(const BookIdentity& identity) const noexcept;
  [[nodiscard]] const OrderBook*
  find(aegis::common::VenueId venue_id,
       aegis::common::InstrumentId instrument_id) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool owns_current_thread() const noexcept;

private:
  [[nodiscard]] OrderBook* find_mutable(const BookIdentity& identity) noexcept;
  [[nodiscard]] bool duplicate_in_venue(const OrderBook& destination,
                                        aegis::common::OrderId order_id) const noexcept;

  std::array<std::unique_ptr<OrderBook>, kMaximumBooks> books_{};
  std::size_t capacity_{};
  std::size_t size_{};
  std::thread::id owner_thread_;
};

} // namespace aegis::order_book

#endif // AEGIS_ORDER_BOOK_BOOK_UNIVERSE_HPP
