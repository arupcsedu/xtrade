#ifndef AEGIS_JOURNAL_FORMAT_HPP
#define AEGIS_JOURNAL_FORMAT_HPP

#include "aegis/journal/journal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aegis::journal::detail {

inline constexpr std::size_t kSegmentHeaderBytes = 192U;
inline constexpr std::size_t kRecordHeaderBytes = 160U;
inline constexpr std::size_t kRecordTrailerBytes = 64U;
inline constexpr std::size_t kSegmentFooterBytes = 128U;
inline constexpr std::size_t kIndexHeaderBytes = 128U;
inline constexpr std::size_t kIndexEntryBytes = 64U;

using SegmentHeaderBytes = std::array<std::uint8_t, kSegmentHeaderBytes>;
using RecordHeaderBytes = std::array<std::uint8_t, kRecordHeaderBytes>;
using RecordTrailerBytes = std::array<std::uint8_t, kRecordTrailerBytes>;
using SegmentFooterBytes = std::array<std::uint8_t, kSegmentFooterBytes>;
using IndexHeaderBytes = std::array<std::uint8_t, kIndexHeaderBytes>;
using IndexEntryBytes = std::array<std::uint8_t, kIndexEntryBytes>;

struct SegmentHeaderFields {
  std::uint64_t segment_id{};
  std::uint64_t first_sequence{};
  std::int64_t created_wall_clock_utc_ns{};
  std::uint64_t created_process_monotonic_ns{};
  common::GlobalEventId writer_instance_id{};
  common::Sha256Digest configuration_sha256{};
  common::Sha256Digest build_sha256{};
  common::Sha256Digest previous_terminal_record_sha256{};
  std::uint32_t flags{};
};

struct RecordHeaderFields {
  RecordMetadata metadata;
  std::uint64_t sequence{};
  std::uint32_t payload_bytes{};
  std::uint32_t total_frame_bytes{};
  common::Sha256Digest previous_record_sha256{};
  common::Sha256Digest payload_sha256{};
};

struct SegmentFooterFields {
  std::uint64_t segment_id{};
  std::uint64_t first_sequence{};
  std::uint64_t last_sequence{};
  std::uint64_t record_count{};
  std::uint64_t data_bytes{};
  common::Sha256Digest terminal_record_sha256{};
  common::Sha256Digest segment_header_sha256{};
};

struct IndexHeaderFields {
  std::uint64_t segment_id{};
  std::uint64_t first_sequence{};
  std::uint64_t last_sequence{};
  std::uint64_t entry_count{};
  std::uint32_t stride{};
  common::Sha256Digest terminal_record_sha256{};
};

struct IndexEntryFields {
  std::uint64_t sequence{};
  std::uint64_t file_offset{};
  common::Sha256Digest record_sha256{};
};

[[nodiscard]] std::uint32_t crc32c(std::span<const std::uint8_t> first,
                                   std::span<const std::uint8_t> second = {},
                                   std::span<const std::uint8_t> third = {}) noexcept;

[[nodiscard]] SegmentHeaderBytes
encode_segment_header(const SegmentHeaderFields& fields) noexcept;
[[nodiscard]] Status decode_segment_header(std::span<const std::uint8_t> bytes,
                                           SegmentHeaderFields& fields) noexcept;

[[nodiscard]] RecordHeaderBytes
encode_record_header(const RecordHeaderFields& fields) noexcept;
[[nodiscard]] Status decode_record_header(std::span<const std::uint8_t> bytes,
                                          RecordHeaderFields& fields) noexcept;

[[nodiscard]] RecordTrailerBytes
encode_record_trailer(const RecordHeaderFields& fields,
                      const common::Sha256Digest& record_sha256,
                      std::span<const std::uint8_t> header,
                      std::span<const std::uint8_t> payload) noexcept;
[[nodiscard]] Status decode_record_trailer(
    std::span<const std::uint8_t> bytes, const RecordHeaderFields& expected,
    const common::Sha256Digest& expected_digest, std::span<const std::uint8_t> header,
    std::span<const std::uint8_t> payload) noexcept;

[[nodiscard]] SegmentFooterBytes
encode_segment_footer(const SegmentFooterFields& fields) noexcept;
[[nodiscard]] Status decode_segment_footer(std::span<const std::uint8_t> bytes,
                                           SegmentFooterFields& fields) noexcept;

[[nodiscard]] IndexHeaderBytes
encode_index_header(const IndexHeaderFields& fields) noexcept;
[[nodiscard]] Status decode_index_header(std::span<const std::uint8_t> bytes,
                                         IndexHeaderFields& fields) noexcept;
[[nodiscard]] IndexEntryBytes
encode_index_entry(const IndexEntryFields& fields) noexcept;
[[nodiscard]] Status decode_index_entry(std::span<const std::uint8_t> bytes,
                                        IndexEntryFields& fields) noexcept;

[[nodiscard]] common::Sha256Digest
record_digest(std::span<const std::uint8_t> encoded_header) noexcept;

[[nodiscard]] bool
is_segment_footer_magic(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] bool all_zero(std::span<const std::uint8_t> bytes) noexcept;

} // namespace aegis::journal::detail

#endif // AEGIS_JOURNAL_FORMAT_HPP
