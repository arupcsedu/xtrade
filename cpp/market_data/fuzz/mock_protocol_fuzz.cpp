#include "aegis/market_data/synthetic/mock_protocol.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

[[nodiscard]] aegis::market_data::synthetic::SyntheticEvent fixture_event() noexcept {
  namespace synthetic = aegis::market_data::synthetic;
  return {
      .type = synthetic::NativeMessageType::price_level,
      .side = synthetic::Side::bid,
      .action = synthetic::BookAction::change,
      .status = synthetic::TradingStatus::none,
      .data_quality = synthetic::DataQuality::valid,
      .message_flags = synthetic::kMessageFlagPriceLevel,
      .packet_flags = 0U,
      .venue_number = 1U,
      .channel_number = 1U,
      .instrument_number = 1U,
      .channel_sequence = 1U,
      .global_ordinal = 1U,
      .exchange_event_time_ns = 1'800'000'000'000'000'000LL,
      .nic_receive_time_ns = 1'800'000'000'000'000'100LL,
      .process_monotonic_time_ns = 1U,
      .synthetic_order_id = 0U,
      .price_ticks = 10'000,
      .quantity_units = 0U,
      .level_quantity_units = 100U,
      .order_count = 1U,
      .auxiliary_code = 0U,
      .auxiliary_value = 0U,
      .event_hash = 0U,
  };
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      const std::size_t size) {
  namespace synthetic = aegis::market_data::synthetic;
  synthetic::SyntheticEvent event{};
  static_cast<void>(
      synthetic::decode_packet(std::span<const std::uint8_t>{data, size}, event));

  auto corrupted = synthetic::encode_packet(fixture_event());
  const auto mutations = std::min(size, corrupted.size());
  for (std::size_t index = 0U; index < mutations; ++index) {
    corrupted[index] ^= data[index];
  }
  static_cast<void>(synthetic::decode_packet(corrupted, event));
  return 0;
}
