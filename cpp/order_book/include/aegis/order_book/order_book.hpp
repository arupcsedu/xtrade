#ifndef AEGIS_ORDER_BOOK_ORDER_BOOK_HPP
#define AEGIS_ORDER_BOOK_ORDER_BOOK_HPP

#include "aegis/order_book/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace aegis::order_book {

namespace detail {
struct BookStorage;
}

class OrderBook final {
public:
  explicit OrderBook(BookConfig config);
  ~OrderBook();

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) = delete;
  OrderBook& operator=(OrderBook&&) = delete;

  [[nodiscard]] ApplyResult apply(const BookUpdate& update) noexcept;
  [[nodiscard]] ApplyResult reject(ApplyError error) noexcept;
  [[nodiscard]] ApplyResult load_snapshot(const BookSnapshot& snapshot) noexcept;
  [[nodiscard]] ApplyResult begin_recovery() noexcept;
  [[nodiscard]] ApplyResult complete_recovery(std::uint64_t last_sequence) noexcept;
  [[nodiscard]] ApplyResult session_reset(aegis::common::SessionId session_id) noexcept;
  [[nodiscard]] ApplyResult corporate_action_reset(
      aegis::common::SessionId new_session_id,
      aegis::common::ConfigurationVersion reference_version) noexcept;

  [[nodiscard]] const BookConfig& config() const noexcept;
  [[nodiscard]] aegis::common::SessionId session_id() const noexcept;
  [[nodiscard]] BookValidity validity() const noexcept;
  [[nodiscard]] market_data::synthetic::TradingStatus trading_status() const noexcept;
  [[nodiscard]] std::uint64_t version() const noexcept;
  [[nodiscard]] std::uint64_t last_valid_sequence() const noexcept;
  [[nodiscard]] std::size_t live_order_count() const noexcept;
  [[nodiscard]] std::size_t bid_level_count() const noexcept;
  [[nodiscard]] std::size_t ask_level_count() const noexcept;
  [[nodiscard]] const TopOfBook& top() const noexcept;
  [[nodiscard]] const AuctionImbalanceState& auction_imbalance() const noexcept;
  [[nodiscard]] const TradeState& last_trade() const noexcept;
  [[nodiscard]] const OrderState* find_order(aegis::common::OrderId id) const noexcept;
  [[nodiscard]] const Level* find_level(market_data::synthetic::Side side,
                                        std::int64_t price_ticks) const noexcept;
  [[nodiscard]] ApplyError depth(DepthSnapshot& output,
                                 std::uint32_t requested_levels) const noexcept;
  [[nodiscard]] ApplyError make_snapshot(BookSnapshot& output) const noexcept;
  [[nodiscard]] InvariantResult check_invariants() const noexcept;
  [[nodiscard]] bool owns_current_thread() const noexcept;

private:
  [[nodiscard]] ApplyResult fail(ApplyError error) noexcept;
  [[nodiscard]] ApplyResult success(const TopOfBook& previous_top,
                                    std::uint64_t sequence) noexcept;
  [[nodiscard]] ApplyError validate_update(const BookUpdate& update) noexcept;
  [[nodiscard]] ApplyError validate_sequence(std::uint64_t sequence) const noexcept;
  [[nodiscard]] ApplyError validate_state(BookOperation operation) const noexcept;
  [[nodiscard]] ApplyError apply_order_update(const BookUpdate& update) noexcept;
  [[nodiscard]] ApplyError apply_level_update(const BookUpdate& update) noexcept;
  [[nodiscard]] ApplyError apply_control_update(const BookUpdate& update) noexcept;
  [[nodiscard]] ApplyError add_order(const BookUpdate& update) noexcept;
  [[nodiscard]] ApplyError remove_quantity(const BookUpdate& update,
                                           bool require_partial,
                                           bool require_full) noexcept;
  [[nodiscard]] ApplyError replace_order(const BookUpdate& update) noexcept;
  [[nodiscard]] ApplyError clear_side(market_data::synthetic::Side side) noexcept;
  void clear_all() noexcept;
  void refresh_top() noexcept;
  [[nodiscard]] InvariantResult
  check_storage(const detail::BookStorage& storage) const noexcept;
  [[nodiscard]] ApplyError rebuild_snapshot(const BookSnapshot& snapshot,
                                            detail::BookStorage& target) noexcept;
  [[nodiscard]] ApplyError rebuild_order_snapshot(const BookSnapshot& snapshot,
                                                  detail::BookStorage& target) noexcept;
  [[nodiscard]] static ApplyError
  rebuild_level_snapshot(const BookSnapshot& snapshot,
                         detail::BookStorage& target) noexcept;

  BookConfig config_;
  aegis::common::SessionId session_id_;
  aegis::common::ConfigurationVersion reference_version_;
  std::thread::id owner_thread_;
  std::unique_ptr<detail::BookStorage> active_;
  std::unique_ptr<detail::BookStorage> staging_;
  TopOfBook top_{};
  AuctionImbalanceState auction_imbalance_{};
  TradeState last_trade_{};
  market_data::synthetic::TradingStatus trading_status_{
      market_data::synthetic::TradingStatus::pre_open};
  BookValidity validity_{BookValidity::recovering};
  std::uint64_t version_{};
  std::uint64_t last_valid_sequence_{};
  std::uint64_t next_priority_{1U};
  bool configuration_valid_{false};
};

} // namespace aegis::order_book

#endif // AEGIS_ORDER_BOOK_ORDER_BOOK_HPP
