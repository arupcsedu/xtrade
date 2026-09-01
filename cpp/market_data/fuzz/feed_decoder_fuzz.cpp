#include "aegis/market_data/feed/synthetic_receiver.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

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
  constexpr aegis::market_data::feed::FeedIdentity identity{
      .session_id = aegis::common::SessionId{1U, 1U},
      .venue_id = aegis::common::VenueId{1U, 1U},
      .channel_id = aegis::common::ChannelId{1U, 1U},
      .venue_number = 1U,
      .channel_number = 1U,
  };
  aegis::market_data::feed::RawPacket packet{
      .session_id = identity.session_id,
      .feed_leg = aegis::market_data::feed::FeedLeg::a,
      .source_sequence_hint = 0U,
      .received_process_monotonic_time_ns = 1U,
      .byte_count = static_cast<std::uint16_t>(
          std::min(size, aegis::market_data::feed::kMaximumRawPacketBytes)),
      .bytes = {},
  };
  std::copy_n(data, packet.byte_count, packet.bytes.begin());
  const aegis::market_data::feed::SyntheticDecoder decoder{identity};
  aegis::market_data::feed::DecodedPacket decoded{};
  static_cast<void>(decoder.decode(packet, decoded));

  auto valid_packet = aegis::market_data::feed::make_synthetic_raw_packet(
      fixture_event(), identity.session_id, aegis::market_data::feed::FeedLeg::a, 1U);
  const auto mutations = std::min(size, aegis::market_data::synthetic::kPacketBytes);
  for (std::size_t index = 0U; index < mutations; ++index) {
    valid_packet.bytes[index] ^= data[index];
  }
  static_cast<void>(decoder.decode(valid_packet, decoded));
  return 0;
}
