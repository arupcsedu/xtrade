#ifndef AEGIS_MARKET_DATA_SYNTHETIC_MOCK_PROTOCOL_HPP
#define AEGIS_MARKET_DATA_SYNTHETIC_MOCK_PROTOCOL_HPP

#include "aegis/market_data/synthetic/event.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace aegis::market_data::synthetic {

inline constexpr std::size_t kMessageBytes = 108U;
inline constexpr std::size_t kPacketHeaderBytes = 48U;
inline constexpr std::size_t kPacketBytes = kPacketHeaderBytes + kMessageBytes;
inline constexpr std::uint8_t kProtocolMajor = 1U;
inline constexpr std::uint8_t kProtocolMinor = 0U;

using EncodedPacket = std::array<std::uint8_t, kPacketBytes>;

enum class DecodeError : std::uint8_t {
  none = 0,
  truncated,
  trailing_bytes,
  invalid_magic,
  unsupported_version,
  invalid_header_length,
  invalid_packet_length,
  invalid_message_count,
  invalid_identifier,
  invalid_sequence,
  invalid_timestamp,
  invalid_flags,
  checksum_mismatch,
  invalid_message_length,
  invalid_message_type,
  invalid_side,
  invalid_action,
  invalid_status,
  invalid_event_shape,
  event_hash_mismatch,
  metadata_mismatch,
};

[[nodiscard]] EncodedPacket encode_packet(const SyntheticEvent& event) noexcept;
[[nodiscard]] DecodeError decode_packet(std::span<const std::uint8_t> bytes,
                                        SyntheticEvent& output) noexcept;
[[nodiscard]] std::string_view decode_error_name(DecodeError error) noexcept;

} // namespace aegis::market_data::synthetic

#endif // AEGIS_MARKET_DATA_SYNTHETIC_MOCK_PROTOCOL_HPP
