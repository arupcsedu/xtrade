#ifndef AEGIS_ORDER_BOOK_SYNTHETIC_ADAPTER_HPP
#define AEGIS_ORDER_BOOK_SYNTHETIC_ADAPTER_HPP

#include "aegis/order_book/order_book.hpp"

#include "aegis/market_data/synthetic/event.hpp"

namespace aegis::order_book {

[[nodiscard]] aegis::common::OrderId
synthetic_order_id(const market_data::synthetic::SyntheticEvent& event) noexcept;

[[nodiscard]] ApplyError
synthetic_update(const market_data::synthetic::SyntheticEvent& event, BookMode mode,
                 BookUpdate& output) noexcept;

[[nodiscard]] ApplyResult
apply_synthetic(OrderBook& book, aegis::common::SessionId session_id,
                const market_data::synthetic::SyntheticEvent& event) noexcept;

} // namespace aegis::order_book

#endif // AEGIS_ORDER_BOOK_SYNTHETIC_ADAPTER_HPP
