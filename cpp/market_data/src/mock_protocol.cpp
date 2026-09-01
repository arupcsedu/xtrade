#include "aegis/market_data/synthetic/mock_protocol.hpp"

#include "aegis/market_data/synthetic/hash.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>

namespace aegis::market_data::synthetic {
namespace {

constexpr std::array<std::uint8_t, 4> kPacketMagic{'S', 'M', 'X', 'P'};

void put_u16(std::span<std::uint8_t> output, const std::size_t offset,
             const std::uint16_t value) noexcept {
  output[offset] = static_cast<std::uint8_t>(value >> 8U);
  output[offset + 1U] = static_cast<std::uint8_t>(value);
}

void put_u32(std::span<std::uint8_t> output, const std::size_t offset,
             const std::uint32_t value) noexcept {
  for (std::size_t index = 0; index < 4U; ++index) {
    const auto shift = static_cast<unsigned>((3U - index) * 8U);
    output[offset + index] = static_cast<std::uint8_t>(value >> shift);
  }
}

void put_u64(std::span<std::uint8_t> output, const std::size_t offset,
             const std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < 8U; ++index) {
    const auto shift = static_cast<unsigned>((7U - index) * 8U);
    output[offset + index] = static_cast<std::uint8_t>(value >> shift);
  }
}

void put_i64(const std::span<std::uint8_t> output, const std::size_t offset,
             const std::int64_t value) noexcept {
  put_u64(output, offset, std::bit_cast<std::uint64_t>(value));
}

[[nodiscard]] std::uint16_t get_u16(const std::span<const std::uint8_t> input,
                                    const std::size_t offset) noexcept {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(input[offset]) << 8U) |
                                    static_cast<std::uint16_t>(input[offset + 1U]));
}

[[nodiscard]] std::uint32_t get_u32(const std::span<const std::uint8_t> input,
                                    const std::size_t offset) noexcept {
  std::uint32_t value = 0U;
  for (std::size_t index = 0; index < 4U; ++index) {
    value = static_cast<std::uint32_t>((value << 8U) | input[offset + index]);
  }
  return value;
}

[[nodiscard]] std::uint64_t get_u64(const std::span<const std::uint8_t> input,
                                    const std::size_t offset) noexcept {
  std::uint64_t value = 0U;
  for (std::size_t index = 0; index < 8U; ++index) {
    value = (value << 8U) | input[offset + index];
  }
  return value;
}

[[nodiscard]] std::int64_t get_i64(const std::span<const std::uint8_t> input,
                                   const std::size_t offset) noexcept {
  return std::bit_cast<std::int64_t>(get_u64(input, offset));
}

[[nodiscard]] bool valid_type_raw(const std::uint8_t value) noexcept {
  return value >= static_cast<std::uint8_t>(NativeMessageType::add_order) &&
         value <= static_cast<std::uint8_t>(NativeMessageType::trading_status);
}

[[nodiscard]] bool valid_side_raw(const std::uint8_t value) noexcept {
  return value <= static_cast<std::uint8_t>(Side::ask);
}

[[nodiscard]] bool valid_action_raw(const std::uint8_t value) noexcept {
  return value <= static_cast<std::uint8_t>(BookAction::clear_book);
}

[[nodiscard]] bool valid_status_raw(const std::uint8_t value) noexcept {
  return value <= static_cast<std::uint8_t>(TradingStatus::closed);
}

} // namespace

EncodedPacket encode_packet(const SyntheticEvent& event) noexcept {
  EncodedPacket encoded{};
  std::ranges::copy(kPacketMagic, encoded.begin());
  encoded[4] = kProtocolMajor;
  encoded[5] = kProtocolMinor;
  put_u16(encoded, 6U, static_cast<std::uint16_t>(kPacketHeaderBytes));
  put_u16(encoded, 8U, static_cast<std::uint16_t>(kPacketBytes));
  put_u16(encoded, 10U, 1U);
  put_u32(encoded, 12U, event.venue_number);
  put_u32(encoded, 16U, event.channel_number);
  put_u64(encoded, 20U, event.channel_sequence);
  put_i64(encoded, 28U, event.exchange_event_time_ns);
  put_u32(encoded, 44U, event.packet_flags);

  const auto message = std::span<std::uint8_t>{encoded}.subspan(kPacketHeaderBytes);
  put_u16(message, 0U, static_cast<std::uint16_t>(kMessageBytes));
  message[2] = static_cast<std::uint8_t>(event.type);
  message[3] = static_cast<std::uint8_t>(event.side);
  message[4] = static_cast<std::uint8_t>(event.action);
  message[5] = static_cast<std::uint8_t>(event.status);
  put_u16(message, 6U, event.message_flags);
  put_u32(message, 8U, event.instrument_number);
  put_u64(message, 12U, event.channel_sequence);
  put_u64(message, 20U, event.global_ordinal);
  put_i64(message, 28U, event.exchange_event_time_ns);
  put_i64(message, 36U, event.nic_receive_time_ns);
  put_u64(message, 44U, event.process_monotonic_time_ns);
  put_u64(message, 52U, event.synthetic_order_id);
  put_i64(message, 60U, event.price_ticks);
  put_u64(message, 68U, event.quantity_units);
  put_u64(message, 76U, event.level_quantity_units);
  put_u32(message, 84U, event.order_count);
  put_u32(message, 88U, event.auxiliary_code);
  put_u64(message, 92U, event.auxiliary_value);
  put_u64(message, 100U, stable_hash(message.first(100U)));
  put_u64(encoded, 36U, stable_hash(message));
  return encoded;
}

DecodeError decode_packet(const std::span<const std::uint8_t> bytes,
                          SyntheticEvent& output) noexcept {
  if (bytes.size() < kPacketBytes) {
    return DecodeError::truncated;
  }
  if (bytes.size() > kPacketBytes) {
    return DecodeError::trailing_bytes;
  }
  if (!std::equal(kPacketMagic.begin(), kPacketMagic.end(), bytes.begin())) {
    return DecodeError::invalid_magic;
  }
  if (bytes[4] != kProtocolMajor || bytes[5] != kProtocolMinor) {
    return DecodeError::unsupported_version;
  }
  if (get_u16(bytes, 6U) != kPacketHeaderBytes) {
    return DecodeError::invalid_header_length;
  }
  if (get_u16(bytes, 8U) != kPacketBytes) {
    return DecodeError::invalid_packet_length;
  }
  if (get_u16(bytes, 10U) != 1U) {
    return DecodeError::invalid_message_count;
  }
  const auto venue = get_u32(bytes, 12U);
  const auto channel = get_u32(bytes, 16U);
  if (venue == 0U || channel == 0U) {
    return DecodeError::invalid_identifier;
  }
  const auto packet_sequence = get_u64(bytes, 20U);
  if (packet_sequence == 0U) {
    return DecodeError::invalid_sequence;
  }
  const auto send_time = get_i64(bytes, 28U);
  if (send_time <= 0) {
    return DecodeError::invalid_timestamp;
  }
  const auto packet_flags = get_u32(bytes, 44U);
  if ((packet_flags & ~kKnownPacketFlags) != 0U) {
    return DecodeError::invalid_flags;
  }
  const auto message = bytes.subspan(kPacketHeaderBytes, kMessageBytes);
  if (get_u64(bytes, 36U) != stable_hash(message)) {
    return DecodeError::checksum_mismatch;
  }
  if (get_u16(message, 0U) != kMessageBytes) {
    return DecodeError::invalid_message_length;
  }
  if (!valid_type_raw(message[2])) {
    return DecodeError::invalid_message_type;
  }
  if (!valid_side_raw(message[3])) {
    return DecodeError::invalid_side;
  }
  if (!valid_action_raw(message[4])) {
    return DecodeError::invalid_action;
  }
  if (!valid_status_raw(message[5])) {
    return DecodeError::invalid_status;
  }
  const auto message_flags = get_u16(message, 6U);
  if ((message_flags & ~kKnownMessageFlags) != 0U) {
    return DecodeError::invalid_flags;
  }
  const auto event_hash = get_u64(message, 100U);
  if (event_hash != stable_hash(message.first(100U))) {
    return DecodeError::event_hash_mismatch;
  }

  const SyntheticEvent decoded{
      .type = static_cast<NativeMessageType>(message[2]),
      .side = static_cast<Side>(message[3]),
      .action = static_cast<BookAction>(message[4]),
      .status = static_cast<TradingStatus>(message[5]),
      .data_quality = (packet_flags & kPacketFlagAfterStaleGap) != 0U
                          ? DataQuality::stale
                          : DataQuality::valid,
      .message_flags = message_flags,
      .packet_flags = packet_flags,
      .venue_number = venue,
      .channel_number = channel,
      .instrument_number = get_u32(message, 8U),
      .channel_sequence = get_u64(message, 12U),
      .global_ordinal = get_u64(message, 20U),
      .exchange_event_time_ns = get_i64(message, 28U),
      .nic_receive_time_ns = get_i64(message, 36U),
      .process_monotonic_time_ns = get_u64(message, 44U),
      .synthetic_order_id = get_u64(message, 52U),
      .price_ticks = get_i64(message, 60U),
      .quantity_units = get_u64(message, 68U),
      .level_quantity_units = get_u64(message, 76U),
      .order_count = get_u32(message, 84U),
      .auxiliary_code = get_u32(message, 88U),
      .auxiliary_value = get_u64(message, 92U),
      .event_hash = event_hash,
  };
  if (decoded.channel_sequence != packet_sequence ||
      decoded.exchange_event_time_ns != send_time) {
    return DecodeError::metadata_mismatch;
  }
  if (!valid_event_shape(decoded)) {
    return DecodeError::invalid_event_shape;
  }
  output = decoded;
  return DecodeError::none;
}

std::string_view decode_error_name(const DecodeError error) noexcept {
  switch (error) {
  case DecodeError::none:
    return "none";
  case DecodeError::truncated:
    return "truncated";
  case DecodeError::trailing_bytes:
    return "trailing-bytes";
  case DecodeError::invalid_magic:
    return "invalid-magic";
  case DecodeError::unsupported_version:
    return "unsupported-version";
  case DecodeError::invalid_header_length:
    return "invalid-header-length";
  case DecodeError::invalid_packet_length:
    return "invalid-packet-length";
  case DecodeError::invalid_message_count:
    return "invalid-message-count";
  case DecodeError::invalid_identifier:
    return "invalid-identifier";
  case DecodeError::invalid_sequence:
    return "invalid-sequence";
  case DecodeError::invalid_timestamp:
    return "invalid-timestamp";
  case DecodeError::invalid_flags:
    return "invalid-flags";
  case DecodeError::checksum_mismatch:
    return "checksum-mismatch";
  case DecodeError::invalid_message_length:
    return "invalid-message-length";
  case DecodeError::invalid_message_type:
    return "invalid-message-type";
  case DecodeError::invalid_side:
    return "invalid-side";
  case DecodeError::invalid_action:
    return "invalid-action";
  case DecodeError::invalid_status:
    return "invalid-status";
  case DecodeError::invalid_event_shape:
    return "invalid-event-shape";
  case DecodeError::event_hash_mismatch:
    return "event-hash-mismatch";
  case DecodeError::metadata_mismatch:
    return "metadata-mismatch";
  }
  return "invalid-error";
}

} // namespace aegis::market_data::synthetic
