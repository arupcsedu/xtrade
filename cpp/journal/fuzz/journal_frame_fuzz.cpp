#include "../src/format.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

template <std::size_t Size>
void xor_mutation(std::array<std::uint8_t, Size>& bytes,
                  const std::span<const std::uint8_t> mutation) noexcept {
  const auto count = std::min(bytes.size(), mutation.size());
  for (std::size_t index = 0U; index < count; ++index) {
    bytes[index] ^= mutation[index];
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      const std::size_t size) {
  const std::span<const std::uint8_t> input(data, size);
  aegis::journal::detail::SegmentHeaderFields segment{};
  (void)aegis::journal::detail::decode_segment_header(input, segment);
  aegis::journal::detail::RecordHeaderFields record{};
  (void)aegis::journal::detail::decode_record_header(input, record);
  aegis::journal::detail::SegmentFooterFields footer{};
  (void)aegis::journal::detail::decode_segment_footer(input, footer);
  aegis::journal::detail::IndexHeaderFields index{};
  (void)aegis::journal::detail::decode_index_header(input, index);
  aegis::journal::detail::IndexEntryFields entry{};
  (void)aegis::journal::detail::decode_index_entry(input, entry);

  aegis::common::Sha256Digest digest{};
  digest.fill(0x5AU);
  auto valid_segment = aegis::journal::detail::encode_segment_header(
      {.segment_id = 1U,
       .first_sequence = 1U,
       .created_wall_clock_utc_ns = 1'800'000'000'000'000'000LL,
       .created_process_monotonic_ns = 1U,
       .writer_instance_id = aegis::common::GlobalEventId(1U, 1U),
       .configuration_sha256 = digest,
       .build_sha256 = digest,
       .previous_terminal_record_sha256 = {},
       .flags = 1U});
  xor_mutation(valid_segment, input);
  (void)aegis::journal::detail::decode_segment_header(valid_segment, segment);

  const aegis::journal::detail::RecordHeaderFields valid_record_fields{
      .metadata = {.kind = aegis::journal::RecordKind::normalized_market_event,
                   .encoding = aegis::journal::PayloadEncoding::opaque_binary,
                   .priority = aegis::journal::RecordPriority::mandatory,
                   .schema_version = {.major = 1U, .minor = 8U, .patch = 0U},
                   .created_process_monotonic_time_ns = 1U,
                   .recorded_wall_clock_utc_time_ns = 1'800'000'000'000'000'000LL,
                   .source_event_sequence = 1U,
                   .global_event_id = aegis::common::GlobalEventId(1U, 2U)},
      .sequence = 1U,
      .payload_bytes = 1U,
      .total_frame_bytes = aegis::journal::detail::kRecordHeaderBytes + 1U +
                           aegis::journal::detail::kRecordTrailerBytes,
      .previous_record_sha256 = {},
      .payload_sha256 = digest};
  auto valid_record = aegis::journal::detail::encode_record_header(valid_record_fields);
  const std::array<std::uint8_t, 1> payload{0x42U};
  const auto record_digest = aegis::journal::detail::record_digest(valid_record);
  auto valid_trailer = aegis::journal::detail::encode_record_trailer(
      valid_record_fields, record_digest, valid_record, payload);
  xor_mutation(valid_trailer, input);
  (void)aegis::journal::detail::decode_record_trailer(
      valid_trailer, valid_record_fields, record_digest, valid_record, payload);
  xor_mutation(valid_record, input);
  (void)aegis::journal::detail::decode_record_header(valid_record, record);

  auto valid_footer = aegis::journal::detail::encode_segment_footer(
      {.segment_id = 1U,
       .first_sequence = 1U,
       .last_sequence = 1U,
       .record_count = 1U,
       .data_bytes = aegis::journal::detail::kSegmentHeaderBytes +
                     aegis::journal::detail::kRecordHeaderBytes + 1U +
                     aegis::journal::detail::kRecordTrailerBytes,
       .terminal_record_sha256 = digest,
       .segment_header_sha256 = digest});
  xor_mutation(valid_footer, input);
  (void)aegis::journal::detail::decode_segment_footer(valid_footer, footer);

  auto valid_index =
      aegis::journal::detail::encode_index_header({.segment_id = 1U,
                                                   .first_sequence = 1U,
                                                   .last_sequence = 1U,
                                                   .entry_count = 1U,
                                                   .stride = 1U,
                                                   .terminal_record_sha256 = digest});
  xor_mutation(valid_index, input);
  (void)aegis::journal::detail::decode_index_header(valid_index, index);

  auto valid_entry = aegis::journal::detail::encode_index_entry(
      {.sequence = 1U,
       .file_offset = aegis::journal::detail::kSegmentHeaderBytes,
       .record_sha256 = digest});
  xor_mutation(valid_entry, input);
  (void)aegis::journal::detail::decode_index_entry(valid_entry, entry);
  return 0;
}
