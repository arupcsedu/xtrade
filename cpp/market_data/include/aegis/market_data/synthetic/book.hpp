#ifndef AEGIS_MARKET_DATA_SYNTHETIC_BOOK_HPP
#define AEGIS_MARKET_DATA_SYNTHETIC_BOOK_HPP

#include "aegis/market_data/synthetic/config.hpp"
#include "aegis/market_data/synthetic/event.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace aegis::market_data::synthetic {

enum class BookApplyError : std::uint8_t {
  none = 0,
  unknown_instrument,
  invalid_event,
  invalid_side,
  invalid_action,
  capacity_exhausted,
  update_while_halted,
  crossed_book,
};

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct BookLevel {
  std::int64_t price_ticks{};
  std::uint64_t quantity_units{};
  std::uint32_t order_count{};
};

struct BookState {
  std::uint32_t venue_number{};
  std::uint32_t instrument_number{};
  TradingStatus status{TradingStatus::open};
  bool valid{true};
  bool crossed{false};
  std::array<BookLevel, kMaximumBookDepth> bids{};
  std::array<BookLevel, kMaximumBookDepth> asks{};
  std::uint32_t bid_count{};
  std::uint32_t ask_count{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

class BookSet {
public:
  BookSet() noexcept = default;

  [[nodiscard]] static std::optional<BookSet>
  create(const GeneratorConfig& config) noexcept;
  [[nodiscard]] bool register_instrument(std::uint32_t venue_number,
                                         std::uint32_t instrument_number) noexcept;

  [[nodiscard]] BookApplyError apply(const SyntheticEvent& event) noexcept;
  [[nodiscard]] const BookState* find(std::uint32_t venue_number,
                                      std::uint32_t instrument_number) const noexcept;
  [[nodiscard]] BookState* find_mutable(std::uint32_t venue_number,
                                        std::uint32_t instrument_number) noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] const BookState& at(std::size_t index) const noexcept;
  [[nodiscard]] std::uint64_t stable_hash() const noexcept;
  [[nodiscard]] bool all_valid() const noexcept;

private:
  std::array<BookState, kMaximumInstruments> books_{};
  std::size_t size_{};
};

[[nodiscard]] const BookLevel* best_bid(const BookState& book) noexcept;
[[nodiscard]] const BookLevel* best_ask(const BookState& book) noexcept;
[[nodiscard]] const BookLevel* find_level(const BookState& book, Side side,
                                          std::int64_t price_ticks) noexcept;

} // namespace aegis::market_data::synthetic

#endif // AEGIS_MARKET_DATA_SYNTHETIC_BOOK_HPP
