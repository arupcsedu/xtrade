#include "test_support.hpp"

#include "../src/format.hpp"

#include "aegis/common/audit_envelope.hpp"
#include "aegis/journal/journal.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace aegis::journal {
namespace {

TEST(JournalFormat, Crc32cMatchesCastagnoliCheckValue) {
  constexpr std::string_view input = "123456789";
  const auto bytes = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
  EXPECT_EQ(detail::crc32c(bytes), 0xE3069283U);
}

// GoogleTest assertion macros inflate integration tests' apparent branch counts.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(JournalWriter, RotatesChainsAndSeeksThroughSparseIndex) {
  const test::TemporaryDirectory temporary;
  auto configuration = test::config(temporary.path() / "journal");
  configuration.maximum_records_per_segment = 2U;
  configuration.index_stride_records = 1U;
  SegmentWriter writer;
  ASSERT_EQ(writer.open(configuration), Status::ok);
  for (std::uint64_t index = 0U; index < 5U; ++index) {
    const auto bytes = test::payload(index);
    const auto result =
        writer.append({.metadata = test::metadata(index), .payload = bytes});
    ASSERT_EQ(result.status, Status::ok);
    EXPECT_EQ(result.journal_sequence, index + 1U);
    EXPECT_TRUE(result.durable_at_return);
  }
  ASSERT_EQ(writer.close(), Status::ok);

  const auto report = RecoveryScanner::scan_directory(configuration.directory);
  ASSERT_EQ(report.status, Status::ok);
  EXPECT_EQ(report.segments.size(), 3U);
  EXPECT_EQ(report.valid_record_count, 5U);
  EXPECT_EQ(report.first_sequence, 1U);
  EXPECT_EQ(report.last_sequence, 5U);

  PersistedRecord record{};
  ASSERT_EQ(RecoveryScanner::read_record(configuration.directory, 4U, record),
            Status::ok);
  EXPECT_EQ(record.journal_sequence, 4U);
  EXPECT_EQ(record.payload, test::payload(3U));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(JournalWriter, RestartContinuesMonotonicSequenceAndChain) {
  const test::TemporaryDirectory temporary;
  const auto configuration = test::config(temporary.path() / "journal");
  {
    SegmentWriter writer;
    ASSERT_EQ(writer.open(configuration), Status::ok);
    const auto bytes = test::payload(1U);
    ASSERT_EQ(writer.append({.metadata = test::metadata(1U), .payload = bytes}).status,
              Status::ok);
    ASSERT_EQ(writer.close(), Status::ok);
  }
  {
    SegmentWriter writer;
    ASSERT_EQ(writer.open(configuration), Status::ok);
    const auto bytes = test::payload(2U);
    const auto result =
        writer.append({.metadata = test::metadata(2U), .payload = bytes});
    EXPECT_EQ(result.status, Status::ok);
    EXPECT_EQ(result.journal_sequence, 2U);
    ASSERT_EQ(writer.close(), Status::ok);
  }
  const auto report = RecoveryScanner::scan_directory(configuration.directory);
  ASSERT_EQ(report.status, Status::ok);
  EXPECT_EQ(report.segments.size(), 2U);
  EXPECT_EQ(report.valid_record_count, 2U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(JournalWriter, PreservesAdditivePayloadSchemaVersion) {
  const test::TemporaryDirectory temporary;
  const auto configuration = test::config(temporary.path() / "journal");
  SegmentWriter writer;
  ASSERT_EQ(writer.open(configuration), Status::ok);
  const auto bytes = test::payload(7U);
  const auto schema = SchemaVersion{.major = 1U, .minor = 99U, .patch = 4U};
  ASSERT_EQ(
      writer
          .append({.metadata = test::metadata(7U, RecordKind::configuration_change,
                                              PayloadEncoding::opaque_binary,
                                              RecordPriority::mandatory, schema),
                   .payload = bytes})
          .status,
      Status::ok);
  ASSERT_EQ(writer.close(), Status::ok);
  const auto report = RecoveryScanner::scan_directory(configuration.directory);
  ASSERT_EQ(report.status, Status::ok);
  ASSERT_EQ(report.segments.front().records.size(), 1U);
  EXPECT_EQ(report.segments.front().records.front().metadata.schema_version.minor, 99U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(JournalWriter, PeriodicSyncReportsOnlyConfiguredDurabilityBoundaries) {
  const test::TemporaryDirectory temporary;
  auto configuration = test::config(temporary.path() / "journal");
  configuration.sync_policy = SyncPolicy::periodic_records;
  configuration.periodic_sync_records = 2U;
  SegmentWriter writer;
  ASSERT_EQ(writer.open(configuration), Status::ok);
  const auto first_bytes = test::payload(1U);
  const auto first =
      writer.append({.metadata = test::metadata(1U), .payload = first_bytes});
  ASSERT_EQ(first.status, Status::ok);
  EXPECT_FALSE(first.durable_at_return);
  const auto second_bytes = test::payload(2U);
  const auto second =
      writer.append({.metadata = test::metadata(2U), .payload = second_bytes});
  ASSERT_EQ(second.status, Status::ok);
  EXPECT_TRUE(second.durable_at_return);
  EXPECT_EQ(writer.metrics().sync_calls, 1U);
  EXPECT_EQ(writer.close(), Status::ok);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(JournalWriter, CorruptSparseIndexFailsSeekWithoutInvalidatingJournalBytes) {
  const test::TemporaryDirectory temporary;
  auto configuration = test::config(temporary.path() / "journal");
  configuration.index_stride_records = 1U;
  SegmentWriter writer;
  ASSERT_EQ(writer.open(configuration), Status::ok);
  const auto bytes = test::payload(1U);
  ASSERT_EQ(writer.append({.metadata = test::metadata(1U), .payload = bytes}).status,
            Status::ok);
  ASSERT_EQ(writer.close(), Status::ok);
  const auto index = test::first_with_extension(configuration.directory, ".idx");
  ASSERT_FALSE(index.empty());
  std::fstream changed(index, std::ios::binary | std::ios::in | std::ios::out);
  ASSERT_TRUE(changed.good());
  changed.seekp(0);
  changed.put('X');
  changed.flush();
  changed.close();

  const auto report = RecoveryScanner::scan_directory(configuration.directory);
  EXPECT_EQ(report.status, Status::ok);
  EXPECT_EQ(report.valid_record_count, 1U);
  PersistedRecord record{};
  EXPECT_EQ(RecoveryScanner::read_record(configuration.directory, 1U, record),
            Status::corrupt_index);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(JournalWriter, ValidatesCanonicalAuditEnvelopeBeforeAppend) {
  const test::TemporaryDirectory temporary;
  const auto configuration = test::config(temporary.path() / "journal");
  const auto fixture = std::filesystem::path(AEGIS_SOURCE_DIR) /
                       "schemas/golden/model_forecast_v1_3.amae";
  std::ifstream input(fixture, std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(input), {});
  ASSERT_FALSE(bytes.empty());
  SegmentWriter writer;
  ASSERT_EQ(writer.open(configuration), Status::ok);
  common::ValidatedAuditEnvelopeView envelope{};
  ASSERT_TRUE(common::validate_size_prefixed_audit_envelope(bytes, &envelope).ok());
  ASSERT_NE(envelope.envelope, nullptr);
  const auto* const envelope_id = envelope.envelope->envelope_id();
  const auto* const created_time = envelope.envelope->created_process_monotonic_time();
  const auto* const recorded_time = envelope.envelope->recorded_wall_clock_utc_time();
  ASSERT_NE(envelope_id, nullptr);
  ASSERT_NE(created_time, nullptr);
  ASSERT_NE(recorded_time, nullptr);
  auto meta = test::metadata(
      9U, RecordKind::model_forecast, PayloadEncoding::canonical_audit_envelope,
      RecordPriority::mandatory, {.major = 1U, .minor = 8U, .patch = 0U});
  meta.global_event_id = common::GlobalEventId(envelope_id->high(), envelope_id->low());
  meta.created_process_monotonic_time_ns = created_time->value();
  meta.recorded_wall_clock_utc_time_ns = recorded_time->value();
  EXPECT_EQ(writer.append({.metadata = meta, .payload = bytes}).status, Status::ok);
  auto malformed = bytes;
  malformed.back() ^= 0x1U;
  EXPECT_EQ(writer.append({.metadata = meta, .payload = malformed}).status,
            Status::invalid_payload);
  EXPECT_EQ(writer.close(), Status::ok);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(JournalWriter, RetentionIsDisabledByDefaultAndRemovesOnlySealedSegments) {
  const test::TemporaryDirectory temporary;
  auto configuration = test::config(temporary.path() / "journal");
  configuration.maximum_records_per_segment = 1U;
  SegmentWriter writer;
  ASSERT_EQ(writer.open(configuration), Status::ok);
  for (std::uint64_t index = 0U; index < 4U; ++index) {
    const auto bytes = test::payload(index, 256U);
    ASSERT_EQ(
        writer.append({.metadata = test::metadata(index), .payload = bytes}).status,
        Status::ok);
  }
  ASSERT_EQ(writer.close(), Status::ok);
  EXPECT_EQ(apply_retention(configuration.directory, {},
                            std::numeric_limits<std::int64_t>::max())
                .removed_segments,
            0U);

  const auto before = RecoveryScanner::scan_directory(configuration.directory);
  ASSERT_EQ(before.status, Status::ok);
  const RetentionPolicy policy{
      .enabled = true,
      .retain_at_least_segments = 1U,
      .retain_at_least_records = 1U,
      .minimum_age_ns = 1U,
      .maximum_total_bytes = before.segments.back().file_bytes,
  };
  const auto retained = apply_retention(configuration.directory, policy,
                                        std::numeric_limits<std::int64_t>::max());
  ASSERT_EQ(retained.status, Status::ok);
  EXPECT_GT(retained.removed_segments, 0U);
  const auto after = RecoveryScanner::scan_directory(configuration.directory);
  EXPECT_EQ(after.status, Status::ok);
  EXPECT_GE(after.valid_record_count, 1U);
}

} // namespace
} // namespace aegis::journal
