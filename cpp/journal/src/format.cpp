#include "format.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <type_traits>

namespace aegis::journal::detail {
namespace {

constexpr std::array<std::uint8_t, 8> kSegmentMagic{'A', 'E', 'G', 'I',
                                                    'S', 'J', '1', 0};
constexpr std::array<std::uint8_t, 8> kRecordMagic{'A', 'E', 'G', 'R',
                                                   'E', 'C', '1', 0};
constexpr std::array<std::uint8_t, 8> kCommitMagic{'A', 'E', 'G', 'C',
                                                   'M', 'T', '1', 0};
constexpr std::array<std::uint8_t, 8> kFooterMagic{'A', 'E', 'G', 'E',
                                                   'N', 'D', '1', 0};
constexpr std::array<std::uint8_t, 8> kIndexMagic{'A', 'E', 'G', 'I', 'D', 'X', '1', 0};
constexpr std::array<std::uint8_t, 8> kIndexEntryMagic{'A', 'E', 'G', 'I',
                                                       'E', 'N', '1', 0};
constexpr std::uint32_t kCrc32cPolynomial = 0x82F63B78U;

[[nodiscard]] consteval std::array<std::uint32_t, 256> make_crc32c_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::size_t index = 0U; index < table.size(); ++index) {
    auto value = static_cast<std::uint32_t>(index);
    for (unsigned bit = 0U; bit < 8U; ++bit) {
      const auto mask = static_cast<std::uint32_t>(0U - (value & 1U));
      value = (value >> 1U) ^ (kCrc32cPolynomial & mask);
    }
    table[index] = value;
  }
  return table;
}

constexpr auto kCrc32cTable = make_crc32c_table();

template <typename Integer, std::size_t Extent>
void put(std::array<std::uint8_t, Extent>& output, const std::size_t offset,
         const Integer value) noexcept {
  static_assert(std::is_integral_v<Integer>);
  using Unsigned = std::make_unsigned_t<Integer>;
  const auto bits = static_cast<Unsigned>(value);
  for (std::size_t index = 0U; index < sizeof(Integer); ++index) {
    output[offset + index] =
        static_cast<std::uint8_t>(bits >> static_cast<unsigned>(index * 8U));
  }
}

template <typename Integer>
[[nodiscard]] Integer get(const std::span<const std::uint8_t> input,
                          const std::size_t offset) noexcept {
  static_assert(std::is_integral_v<Integer>);
  std::uint64_t value{};
  for (std::size_t index = 0U; index < sizeof(Integer); ++index) {
    value |= static_cast<std::uint64_t>(input[offset + index])
             << static_cast<unsigned>(index * 8U);
  }
  return static_cast<Integer>(value);
}

template <std::size_t Extent>
void put_digest(std::array<std::uint8_t, Extent>& output, const std::size_t offset,
                const common::Sha256Digest& digest) noexcept {
  std::copy(digest.begin(), digest.end(),
            output.begin() + static_cast<std::ptrdiff_t>(offset));
}

[[nodiscard]] common::Sha256Digest get_digest(const std::span<const std::uint8_t> input,
                                              const std::size_t offset) noexcept {
  common::Sha256Digest digest{};
  std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset), digest.size(),
              digest.begin());
  return digest;
}

template <std::size_t Extent>
void put_magic(std::array<std::uint8_t, Extent>& output,
               const std::array<std::uint8_t, 8>& magic) noexcept {
  std::copy(magic.begin(), magic.end(), output.begin());
}

[[nodiscard]] bool has_magic(const std::span<const std::uint8_t> input,
                             const std::array<std::uint8_t, 8>& magic) noexcept {
  return input.size() >= magic.size() &&
         std::equal(magic.begin(), magic.end(), input.begin());
}

[[nodiscard]] std::uint32_t extend_crc(std::uint32_t crc,
                                       const std::span<const std::uint8_t> input,
                                       const std::size_t zero_offset,
                                       const std::size_t zero_bytes) noexcept {
  for (std::size_t index = 0U; index < input.size(); ++index) {
    const auto byte =
        index >= zero_offset && index < zero_offset + zero_bytes ? 0U : input[index];
    const auto table_index = static_cast<std::uint8_t>(crc ^ byte);
    crc = (crc >> 8U) ^ kCrc32cTable[table_index];
  }
  return crc;
}

template <std::size_t Extent>
[[nodiscard]] bool validate_fixed_crc(const std::span<const std::uint8_t> bytes,
                                      const std::size_t crc_offset) noexcept {
  if (bytes.size() < Extent) {
    return false;
  }
  const auto expected = get<std::uint32_t>(bytes, crc_offset);
  auto crc = extend_crc(std::numeric_limits<std::uint32_t>::max(), bytes.first(Extent),
                        crc_offset, sizeof(std::uint32_t));
  return ~crc == expected;
}

template <std::size_t Extent>
void set_fixed_crc(std::array<std::uint8_t, Extent>& bytes,
                   const std::size_t crc_offset) noexcept {
  put<std::uint32_t>(bytes, crc_offset, 0U);
  auto crc = extend_crc(std::numeric_limits<std::uint32_t>::max(), bytes, crc_offset,
                        sizeof(std::uint32_t));
  put<std::uint32_t>(bytes, crc_offset, ~crc);
}

} // namespace

std::uint32_t crc32c(const std::span<const std::uint8_t> first,
                     const std::span<const std::uint8_t> second,
                     const std::span<const std::uint8_t> third) noexcept {
  auto crc = std::numeric_limits<std::uint32_t>::max();
  crc = extend_crc(crc, first, first.size(), 0U);
  crc = extend_crc(crc, second, second.size(), 0U);
  crc = extend_crc(crc, third, third.size(), 0U);
  return ~crc;
}

SegmentHeaderBytes encode_segment_header(const SegmentHeaderFields& fields) noexcept {
  SegmentHeaderBytes bytes{};
  put_magic(bytes, kSegmentMagic);
  put<std::uint16_t>(bytes, 8U, kJournalFormatMajor);
  put<std::uint16_t>(bytes, 10U, kJournalFormatMinor);
  put<std::uint32_t>(bytes, 12U, kSegmentHeaderBytes);
  put<std::uint64_t>(bytes, 16U, fields.segment_id);
  put<std::uint64_t>(bytes, 24U, fields.first_sequence);
  put<std::int64_t>(bytes, 32U, fields.created_wall_clock_utc_ns);
  put<std::uint64_t>(bytes, 40U, fields.created_process_monotonic_ns);
  put<std::uint64_t>(bytes, 48U, fields.writer_instance_id.high());
  put<std::uint64_t>(bytes, 56U, fields.writer_instance_id.low());
  put_digest(bytes, 64U, fields.configuration_sha256);
  put_digest(bytes, 96U, fields.build_sha256);
  put_digest(bytes, 128U, fields.previous_terminal_record_sha256);
  put<std::uint32_t>(bytes, 160U, fields.flags);
  set_fixed_crc(bytes, 164U);
  return bytes;
}

Status decode_segment_header(const std::span<const std::uint8_t> bytes,
                             SegmentHeaderFields& fields) noexcept {
  if (bytes.size() < kSegmentHeaderBytes) {
    return Status::truncated_tail;
  }
  if (!has_magic(bytes, kSegmentMagic)) {
    return Status::corrupt_header;
  }
  if (get<std::uint16_t>(bytes, 8U) != kJournalFormatMajor ||
      get<std::uint16_t>(bytes, 10U) > kJournalFormatMinor ||
      get<std::uint32_t>(bytes, 12U) != kSegmentHeaderBytes) {
    return Status::unsupported_format;
  }
  if (!validate_fixed_crc<kSegmentHeaderBytes>(bytes, 164U)) {
    return Status::corrupt_header;
  }
  fields.segment_id = get<std::uint64_t>(bytes, 16U);
  fields.first_sequence = get<std::uint64_t>(bytes, 24U);
  fields.created_wall_clock_utc_ns = get<std::int64_t>(bytes, 32U);
  fields.created_process_monotonic_ns = get<std::uint64_t>(bytes, 40U);
  fields.writer_instance_id = common::GlobalEventId(get<std::uint64_t>(bytes, 48U),
                                                    get<std::uint64_t>(bytes, 56U));
  fields.configuration_sha256 = get_digest(bytes, 64U);
  fields.build_sha256 = get_digest(bytes, 96U);
  fields.previous_terminal_record_sha256 = get_digest(bytes, 128U);
  fields.flags = get<std::uint32_t>(bytes, 160U);
  return fields.segment_id != 0U && fields.first_sequence != 0U &&
                 fields.created_wall_clock_utc_ns > 0 &&
                 fields.created_process_monotonic_ns != 0U &&
                 fields.flags >= static_cast<std::uint32_t>(SyncPolicy::none) &&
                 fields.flags <=
                     static_cast<std::uint32_t>(SyncPolicy::segment_close) &&
                 fields.writer_instance_id.valid() &&
                 !common::is_zero_digest(fields.configuration_sha256) &&
                 !common::is_zero_digest(fields.build_sha256)
             ? Status::ok
             : Status::corrupt_header;
}

RecordHeaderBytes encode_record_header(const RecordHeaderFields& fields) noexcept {
  RecordHeaderBytes bytes{};
  put_magic(bytes, kRecordMagic);
  put<std::uint16_t>(bytes, 8U, kJournalFormatMajor);
  put<std::uint16_t>(bytes, 10U, kJournalFormatMinor);
  put<std::uint32_t>(bytes, 12U, kRecordHeaderBytes);
  put<std::uint32_t>(bytes, 16U, fields.total_frame_bytes);
  put<std::uint16_t>(bytes, 20U, static_cast<std::uint16_t>(fields.metadata.kind));
  put<std::uint8_t>(bytes, 22U, static_cast<std::uint8_t>(fields.metadata.encoding));
  put<std::uint8_t>(bytes, 23U, static_cast<std::uint8_t>(fields.metadata.priority));
  put<std::uint16_t>(bytes, 24U, fields.metadata.schema_version.major);
  put<std::uint16_t>(bytes, 26U, fields.metadata.schema_version.minor);
  put<std::uint32_t>(bytes, 28U, fields.metadata.schema_version.patch);
  put<std::uint32_t>(bytes, 32U, fields.payload_bytes);
  put<std::uint64_t>(bytes, 40U, fields.sequence);
  put<std::uint64_t>(bytes, 48U, fields.metadata.created_process_monotonic_time_ns);
  put<std::int64_t>(bytes, 56U, fields.metadata.recorded_wall_clock_utc_time_ns);
  put<std::uint64_t>(bytes, 64U, fields.metadata.source_event_sequence);
  put<std::uint64_t>(bytes, 72U, fields.metadata.global_event_id.high());
  put<std::uint64_t>(bytes, 80U, fields.metadata.global_event_id.low());
  put_digest(bytes, 88U, fields.previous_record_sha256);
  put_digest(bytes, 120U, fields.payload_sha256);
  set_fixed_crc(bytes, 152U);
  return bytes;
}

Status decode_record_header(const std::span<const std::uint8_t> bytes,
                            RecordHeaderFields& fields) noexcept {
  if (bytes.size() < kRecordHeaderBytes) {
    return Status::truncated_tail;
  }
  if (!has_magic(bytes, kRecordMagic)) {
    return Status::corrupt_record;
  }
  if (get<std::uint16_t>(bytes, 8U) != kJournalFormatMajor ||
      get<std::uint16_t>(bytes, 10U) > kJournalFormatMinor ||
      get<std::uint32_t>(bytes, 12U) != kRecordHeaderBytes) {
    return Status::unsupported_format;
  }
  if (!validate_fixed_crc<kRecordHeaderBytes>(bytes, 152U)) {
    return Status::corrupt_record;
  }
  fields.total_frame_bytes = get<std::uint32_t>(bytes, 16U);
  fields.metadata.kind = static_cast<RecordKind>(get<std::uint16_t>(bytes, 20U));
  fields.metadata.encoding =
      static_cast<PayloadEncoding>(get<std::uint8_t>(bytes, 22U));
  fields.metadata.priority = static_cast<RecordPriority>(get<std::uint8_t>(bytes, 23U));
  fields.metadata.schema_version.major = get<std::uint16_t>(bytes, 24U);
  fields.metadata.schema_version.minor = get<std::uint16_t>(bytes, 26U);
  fields.metadata.schema_version.patch = get<std::uint32_t>(bytes, 28U);
  fields.payload_bytes = get<std::uint32_t>(bytes, 32U);
  fields.sequence = get<std::uint64_t>(bytes, 40U);
  fields.metadata.created_process_monotonic_time_ns = get<std::uint64_t>(bytes, 48U);
  fields.metadata.recorded_wall_clock_utc_time_ns = get<std::int64_t>(bytes, 56U);
  fields.metadata.source_event_sequence = get<std::uint64_t>(bytes, 64U);
  fields.metadata.global_event_id = common::GlobalEventId(
      get<std::uint64_t>(bytes, 72U), get<std::uint64_t>(bytes, 80U));
  fields.previous_record_sha256 = get_digest(bytes, 88U);
  fields.payload_sha256 = get_digest(bytes, 120U);
  const auto expected_total = kRecordHeaderBytes +
                              static_cast<std::size_t>(fields.payload_bytes) +
                              kRecordTrailerBytes;
  if (fields.sequence == 0U || fields.payload_bytes == 0U ||
      fields.payload_bytes > kMaximumPersistentPayloadBytes ||
      fields.total_frame_bytes != expected_total ||
      !fields.metadata.global_event_id.valid() ||
      !is_known_record_kind(fields.metadata.kind) ||
      fields.metadata.encoding == PayloadEncoding::unknown ||
      fields.metadata.encoding > PayloadEncoding::opaque_binary ||
      fields.metadata.priority < RecordPriority::advisory ||
      fields.metadata.priority > RecordPriority::mandatory ||
      fields.metadata.schema_version.major == 0U ||
      fields.metadata.created_process_monotonic_time_ns == 0U ||
      fields.metadata.recorded_wall_clock_utc_time_ns <= 0) {
    return Status::corrupt_record;
  }
  return Status::ok;
}

RecordTrailerBytes
encode_record_trailer(const RecordHeaderFields& fields,
                      const common::Sha256Digest& record_sha256,
                      const std::span<const std::uint8_t> header,
                      const std::span<const std::uint8_t> payload) noexcept {
  RecordTrailerBytes bytes{};
  put_magic(bytes, kCommitMagic);
  put<std::uint64_t>(bytes, 8U, fields.sequence);
  put<std::uint32_t>(bytes, 16U, fields.total_frame_bytes);
  put_digest(bytes, 24U, record_sha256);
  put<std::uint32_t>(bytes, 56U, 0U);
  const auto checksum = crc32c(header, payload, bytes);
  put<std::uint32_t>(bytes, 56U, checksum);
  return bytes;
}

Status decode_record_trailer(const std::span<const std::uint8_t> bytes,
                             const RecordHeaderFields& expected,
                             const common::Sha256Digest& expected_digest,
                             const std::span<const std::uint8_t> header,
                             const std::span<const std::uint8_t> payload) noexcept {
  if (bytes.size() < kRecordTrailerBytes) {
    return Status::truncated_tail;
  }
  if (!has_magic(bytes, kCommitMagic) ||
      get<std::uint64_t>(bytes, 8U) != expected.sequence ||
      get<std::uint32_t>(bytes, 16U) != expected.total_frame_bytes ||
      get_digest(bytes, 24U) != expected_digest) {
    return Status::corrupt_record;
  }
  RecordTrailerBytes normalized{};
  std::copy_n(bytes.begin(), normalized.size(), normalized.begin());
  const auto expected_crc = get<std::uint32_t>(bytes, 56U);
  put<std::uint32_t>(normalized, 56U, 0U);
  return crc32c(header, payload, normalized) == expected_crc ? Status::ok
                                                             : Status::corrupt_record;
}

SegmentFooterBytes encode_segment_footer(const SegmentFooterFields& fields) noexcept {
  SegmentFooterBytes bytes{};
  put_magic(bytes, kFooterMagic);
  put<std::uint16_t>(bytes, 8U, kJournalFormatMajor);
  put<std::uint16_t>(bytes, 10U, kJournalFormatMinor);
  put<std::uint32_t>(bytes, 12U, kSegmentFooterBytes);
  put<std::uint64_t>(bytes, 16U, fields.segment_id);
  put<std::uint64_t>(bytes, 24U, fields.first_sequence);
  put<std::uint64_t>(bytes, 32U, fields.last_sequence);
  put<std::uint64_t>(bytes, 40U, fields.record_count);
  put<std::uint64_t>(bytes, 48U, fields.data_bytes);
  put_digest(bytes, 56U, fields.terminal_record_sha256);
  put_digest(bytes, 88U, fields.segment_header_sha256);
  set_fixed_crc(bytes, 120U);
  return bytes;
}

Status decode_segment_footer(const std::span<const std::uint8_t> bytes,
                             SegmentFooterFields& fields) noexcept {
  if (bytes.size() < kSegmentFooterBytes) {
    return Status::truncated_tail;
  }
  if (!has_magic(bytes, kFooterMagic)) {
    return Status::corrupt_record;
  }
  if (get<std::uint16_t>(bytes, 8U) != kJournalFormatMajor ||
      get<std::uint16_t>(bytes, 10U) > kJournalFormatMinor ||
      get<std::uint32_t>(bytes, 12U) != kSegmentFooterBytes) {
    return Status::unsupported_format;
  }
  if (!validate_fixed_crc<kSegmentFooterBytes>(bytes, 120U)) {
    return Status::corrupt_record;
  }
  fields.segment_id = get<std::uint64_t>(bytes, 16U);
  fields.first_sequence = get<std::uint64_t>(bytes, 24U);
  fields.last_sequence = get<std::uint64_t>(bytes, 32U);
  fields.record_count = get<std::uint64_t>(bytes, 40U);
  fields.data_bytes = get<std::uint64_t>(bytes, 48U);
  fields.terminal_record_sha256 = get_digest(bytes, 56U);
  fields.segment_header_sha256 = get_digest(bytes, 88U);
  return Status::ok;
}

IndexHeaderBytes encode_index_header(const IndexHeaderFields& fields) noexcept {
  IndexHeaderBytes bytes{};
  put_magic(bytes, kIndexMagic);
  put<std::uint16_t>(bytes, 8U, kJournalFormatMajor);
  put<std::uint16_t>(bytes, 10U, kJournalFormatMinor);
  put<std::uint32_t>(bytes, 12U, kIndexHeaderBytes);
  put<std::uint64_t>(bytes, 16U, fields.segment_id);
  put<std::uint64_t>(bytes, 24U, fields.first_sequence);
  put<std::uint64_t>(bytes, 32U, fields.last_sequence);
  put<std::uint64_t>(bytes, 40U, fields.entry_count);
  put<std::uint32_t>(bytes, 48U, fields.stride);
  put_digest(bytes, 56U, fields.terminal_record_sha256);
  set_fixed_crc(bytes, 120U);
  return bytes;
}

Status decode_index_header(const std::span<const std::uint8_t> bytes,
                           IndexHeaderFields& fields) noexcept {
  if (bytes.size() < kIndexHeaderBytes) {
    return Status::truncated_tail;
  }
  if (!has_magic(bytes, kIndexMagic) ||
      !validate_fixed_crc<kIndexHeaderBytes>(bytes, 120U)) {
    return Status::corrupt_index;
  }
  if (get<std::uint16_t>(bytes, 8U) != kJournalFormatMajor ||
      get<std::uint16_t>(bytes, 10U) > kJournalFormatMinor ||
      get<std::uint32_t>(bytes, 12U) != kIndexHeaderBytes) {
    return Status::unsupported_format;
  }
  fields.segment_id = get<std::uint64_t>(bytes, 16U);
  fields.first_sequence = get<std::uint64_t>(bytes, 24U);
  fields.last_sequence = get<std::uint64_t>(bytes, 32U);
  fields.entry_count = get<std::uint64_t>(bytes, 40U);
  fields.stride = get<std::uint32_t>(bytes, 48U);
  fields.terminal_record_sha256 = get_digest(bytes, 56U);
  return fields.segment_id != 0U && fields.first_sequence != 0U && fields.stride != 0U
             ? Status::ok
             : Status::corrupt_index;
}

IndexEntryBytes encode_index_entry(const IndexEntryFields& fields) noexcept {
  IndexEntryBytes bytes{};
  put_magic(bytes, kIndexEntryMagic);
  put<std::uint64_t>(bytes, 8U, fields.sequence);
  put<std::uint64_t>(bytes, 16U, fields.file_offset);
  put_digest(bytes, 24U, fields.record_sha256);
  set_fixed_crc(bytes, 56U);
  return bytes;
}

Status decode_index_entry(const std::span<const std::uint8_t> bytes,
                          IndexEntryFields& fields) noexcept {
  if (bytes.size() < kIndexEntryBytes) {
    return Status::truncated_tail;
  }
  if (!has_magic(bytes, kIndexEntryMagic) ||
      !validate_fixed_crc<kIndexEntryBytes>(bytes, 56U)) {
    return Status::corrupt_index;
  }
  fields.sequence = get<std::uint64_t>(bytes, 8U);
  fields.file_offset = get<std::uint64_t>(bytes, 16U);
  fields.record_sha256 = get_digest(bytes, 24U);
  return fields.sequence != 0U && fields.file_offset >= kSegmentHeaderBytes
             ? Status::ok
             : Status::corrupt_index;
}

common::Sha256Digest
record_digest(const std::span<const std::uint8_t> encoded_header) noexcept {
  return common::sha256(encoded_header);
}

bool is_segment_footer_magic(const std::span<const std::uint8_t> bytes) noexcept {
  return has_magic(bytes, kFooterMagic);
}

bool all_zero(const std::span<const std::uint8_t> bytes) noexcept {
  return std::ranges::all_of(bytes,
                             [](const std::uint8_t value) { return value == 0U; });
}

} // namespace aegis::journal::detail
