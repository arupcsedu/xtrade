#ifndef AEGIS_MARKET_DATA_SYNTHETIC_EVENT_HPP
#define AEGIS_MARKET_DATA_SYNTHETIC_EVENT_HPP

#include "aegis/market_data/synthetic/hash.hpp"

#include <cstdint>
#include <string_view>

namespace aegis::market_data::synthetic {

enum class NativeMessageType : std::uint8_t {
  add_order = 1,
  cancel_order = 2,
  modify_order = 3,
  price_level = 4,
  trade = 5,
  quote = 6,
  auction_imbalance = 7,
  trading_status = 8,
};

enum class Side : std::uint8_t {
  none = 0,
  bid = 1,
  ask = 2,
};

enum class BookAction : std::uint8_t {
  none = 0,
  add = 1,
  change = 2,
  delete_level = 3,
  clear_side = 4,
  clear_book = 5,
};

enum class TradingStatus : std::uint8_t {
  none = 0,
  pre_open = 1,
  open = 2,
  halted = 3,
  auction = 4,
  closed = 5,
};

enum class DataQuality : std::uint8_t {
  valid = 1,
  degraded = 2,
  stale = 3,
  invalid = 4,
};

inline constexpr std::uint16_t kMessageFlagOrderLevel = 0x0001U;
inline constexpr std::uint16_t kMessageFlagPriceLevel = 0x0002U;
inline constexpr std::uint16_t kMessageFlagScenarioFault = 0x0004U;
inline constexpr std::uint16_t kMessageFlagHiddenReplenishment = 0x0008U;
inline constexpr std::uint16_t kMessageFlagShock = 0x0010U;
inline constexpr std::uint16_t kKnownMessageFlags = 0x001FU;

inline constexpr std::uint32_t kPacketFlagBurst = 0x0000'0001U;
inline constexpr std::uint32_t kPacketFlagAfterStaleGap = 0x0000'0002U;
inline constexpr std::uint32_t kPacketFlagScenario = 0x0000'0004U;
inline constexpr std::uint32_t kKnownPacketFlags = 0x0000'0007U;

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct SyntheticEvent {
  NativeMessageType type{NativeMessageType::add_order};
  Side side{Side::none};
  BookAction action{BookAction::none};
  TradingStatus status{TradingStatus::none};
  DataQuality data_quality{DataQuality::valid};
  std::uint16_t message_flags{};
  std::uint32_t packet_flags{};
  std::uint32_t venue_number{};
  std::uint32_t channel_number{};
  std::uint32_t instrument_number{};
  std::uint64_t channel_sequence{};
  std::uint64_t global_ordinal{};
  std::int64_t exchange_event_time_ns{};
  std::int64_t nic_receive_time_ns{};
  std::uint64_t process_monotonic_time_ns{};
  std::uint64_t synthetic_order_id{};
  std::int64_t price_ticks{};
  std::uint64_t quantity_units{};
  std::uint64_t level_quantity_units{};
  std::uint32_t order_count{};
  std::uint32_t auxiliary_code{};
  std::uint64_t auxiliary_value{};
  std::uint64_t event_hash{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] std::uint64_t calculate_event_hash(const SyntheticEvent& event) noexcept;
[[nodiscard]] bool valid_event_shape(const SyntheticEvent& event) noexcept;
[[nodiscard]] std::string_view message_type_name(NativeMessageType type) noexcept;
[[nodiscard]] std::string_view side_name(Side side) noexcept;
[[nodiscard]] std::string_view action_name(BookAction action) noexcept;
[[nodiscard]] std::string_view status_name(TradingStatus status) noexcept;

} // namespace aegis::market_data::synthetic

#endif // AEGIS_MARKET_DATA_SYNTHETIC_EVENT_HPP
