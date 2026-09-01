#include "test_support.hpp"

#include "aegis/common/sha256.hpp"
#include "aegis/journal/journal.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace aegis::journal {
namespace {

[[nodiscard]] std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}

// GoogleTest assertion macros inflate integration tests' apparent branch counts.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(JournalRecovery, TruncatedPowerLossTailKeepsOnlyCommittedPrefix) {
  const test::TemporaryDirectory temporary;
  auto configuration = test::config(temporary.path() / "journal");
  configuration.preallocate_bytes = 1024ULL * 1024U;
  const auto process = ::fork();
  ASSERT_GE(process, 0);
  if (process == 0) {
    SegmentWriter writer;
    if (writer.open(configuration) != Status::ok) {
      ::_exit(2);
    }
    const auto bytes = test::payload(1U);
    if (writer.append({.metadata = test::metadata(1U), .payload = bytes}).status !=
        Status::ok) {
      ::_exit(3);
    }
    ::_exit(0);
  }
  int child_status{};
  ASSERT_EQ(::waitpid(process, &child_status, 0), process);
  ASSERT_TRUE(WIFEXITED(child_status));
  ASSERT_EQ(WEXITSTATUS(child_status), 0);

  const auto report = RecoveryScanner::scan_directory(configuration.directory);
  EXPECT_EQ(report.status, Status::truncated_tail);
  ASSERT_EQ(report.segments.size(), 1U);
  EXPECT_TRUE(report.segments.front().recoverable_tail);
  EXPECT_EQ(report.valid_record_count, 1U);
  EXPECT_EQ(report.last_sequence, 1U);

  SegmentWriter replacement;
  ASSERT_EQ(replacement.open(configuration), Status::ok);
  const auto replacement_bytes = test::payload(2U);
  const auto replacement_result = replacement.append(
      {.metadata = test::metadata(2U), .payload = replacement_bytes});
  EXPECT_EQ(replacement_result.status, Status::ok);
  EXPECT_EQ(replacement_result.journal_sequence, 2U);
  ASSERT_EQ(replacement.close(), Status::ok);
  const auto continued = RecoveryScanner::scan_directory(configuration.directory);
  EXPECT_EQ(continued.status, Status::truncated_tail);
  EXPECT_EQ(continued.valid_record_count, 2U);
  EXPECT_EQ(continued.last_sequence, 2U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(JournalRecovery, PayloadCorruptionIsNeverTreatedAsRecoverableTail) {
  const test::TemporaryDirectory temporary;
  const auto configuration = test::config(temporary.path() / "journal");
  SegmentWriter writer;
  ASSERT_EQ(writer.open(configuration), Status::ok);
  const auto bytes = test::payload(1U, 64U);
  ASSERT_EQ(writer.append({.metadata = test::metadata(1U), .payload = bytes}).status,
            Status::ok);
  ASSERT_EQ(writer.close(), Status::ok);
  const auto segment = test::first_with_extension(configuration.directory, ".ajl");
  ASSERT_FALSE(segment.empty());
  std::fstream file(segment, std::ios::binary | std::ios::in | std::ios::out);
  ASSERT_TRUE(file.good());
  constexpr std::streamoff kPayloadOffset = 192 + 160 + 7;
  file.seekg(kPayloadOffset);
  char value{};
  file.get(value);
  file.seekp(kPayloadOffset);
  value ^= 0x5A;
  file.put(value);
  file.flush();
  file.close();

  const auto report = RecoveryScanner::scan_directory(configuration.directory);
  EXPECT_EQ(report.status, Status::corrupt_record);
  EXPECT_EQ(report.valid_record_count, 0U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(JournalRecovery, PartialRecordAfterPowerLossNeverCommits) {
  const test::TemporaryDirectory temporary;
  const auto source = temporary.path() / "source";
  const auto target = temporary.path() / "target";
  const auto configuration = test::config(source);
  SegmentWriter writer;
  ASSERT_EQ(writer.open(configuration), Status::ok);
  for (std::uint64_t index = 0U; index < 2U; ++index) {
    const auto bytes = test::payload(index);
    ASSERT_EQ(
        writer.append({.metadata = test::metadata(index), .payload = bytes}).status,
        Status::ok);
  }
  ASSERT_EQ(writer.close(), Status::ok);
  const auto sealed = source / "journal-00000000000000000001.ajl";
  const auto interrupted = source / "journal-00000000000000000001.open";
  ASSERT_TRUE(std::filesystem::exists(sealed));
  std::filesystem::rename(sealed, interrupted);
  constexpr std::uintmax_t kFirstFrameBytes = 160U + 32U + 64U;
  constexpr std::uintmax_t kPartialSecondFrameBytes = 160U + 8U;
  std::filesystem::resize_file(interrupted,
                               192U + kFirstFrameBytes + kPartialSecondFrameBytes);

  const auto report = RecoveryScanner::scan_directory(source);
  ASSERT_EQ(report.status, Status::truncated_tail);
  ASSERT_EQ(report.segments.size(), 1U);
  EXPECT_TRUE(report.segments.front().recoverable_tail);
  EXPECT_EQ(report.valid_record_count, 1U);
  EXPECT_EQ(report.last_sequence, 1U);

  RepairReport repair{};
  ASSERT_EQ(repair_copy(source, test::config(target), repair), Status::ok);
  EXPECT_EQ(repair.source_status, Status::truncated_tail);
  EXPECT_EQ(repair.copied_records, 1U);
  const auto repaired = RecoveryScanner::scan_directory(target);
  EXPECT_EQ(repaired.status, Status::ok);
  EXPECT_EQ(repaired.valid_record_count, 1U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(JournalRecovery, RepairCopiesVerifiedPrefixWithoutChangingOriginal) {
  const test::TemporaryDirectory temporary;
  const auto source = temporary.path() / "source";
  const auto target = temporary.path() / "target";
  const auto configuration = test::config(source);
  SegmentWriter writer;
  ASSERT_EQ(writer.open(configuration), Status::ok);
  for (std::uint64_t index = 0U; index < 3U; ++index) {
    const auto bytes = test::payload(index);
    ASSERT_EQ(
        writer.append({.metadata = test::metadata(index), .payload = bytes}).status,
        Status::ok);
  }
  ASSERT_EQ(writer.close(), Status::ok);
  const auto segment = source / "journal-00000000000000000001.ajl";
  ASSERT_TRUE(std::filesystem::exists(segment));
  ASSERT_FALSE(segment.empty());
  const auto full_size = std::filesystem::file_size(segment);
  ASSERT_GT(full_size, 64U);
  std::filesystem::resize_file(segment, full_size - 32U);
  const auto before = read_file(segment);
  const auto before_digest = common::sha256(before);

  SegmentWriter refused;
  EXPECT_EQ(refused.open(configuration), Status::truncated_tail);
  PersistedRecord rejected_record{};
  EXPECT_EQ(RecoveryScanner::read_record(source, 1U, rejected_record),
            Status::truncated_tail);
  std::uint64_t replayed{};
  EXPECT_EQ(
      replay_verified(
          source, 0U, 0U, [](const PersistedRecord&) { return Status::ok; }, replayed),
      Status::truncated_tail);
  EXPECT_EQ(replayed, 0U);
  const RetentionPolicy retention{
      .enabled = true,
      .retain_at_least_segments = 1U,
      .retain_at_least_records = 1U,
      .minimum_age_ns = 0U,
      .maximum_total_bytes = 1U,
  };
  const auto retention_result =
      apply_retention(source, retention, std::numeric_limits<std::int64_t>::max());
  EXPECT_EQ(retention_result.status, Status::truncated_tail);
  EXPECT_EQ(retention_result.removed_segments, 0U);

  auto target_configuration = test::config(target);
  RepairReport repair{};
  ASSERT_EQ(repair_copy(source, target_configuration, repair), Status::ok);
  EXPECT_EQ(repair.source_status, Status::truncated_tail);
  EXPECT_EQ(repair.target_status, Status::ok);
  EXPECT_EQ(repair.copied_records, 3U);
  const auto after = read_file(segment);
  EXPECT_EQ(after.size(), before.size());
  EXPECT_EQ(common::sha256(after), before_digest);

  const auto target_report = RecoveryScanner::scan_directory(target);
  EXPECT_EQ(target_report.status, Status::ok);
  EXPECT_EQ(target_report.valid_record_count, 3U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(JournalRecovery, ReplayStopsBeforeAnyInternalCorruption) {
  const test::TemporaryDirectory temporary;
  const auto configuration = test::config(temporary.path() / "journal");
  SegmentWriter writer;
  ASSERT_EQ(writer.open(configuration), Status::ok);
  for (std::uint64_t index = 0U; index < 2U; ++index) {
    const auto bytes = test::payload(index);
    ASSERT_EQ(
        writer.append({.metadata = test::metadata(index), .payload = bytes}).status,
        Status::ok);
  }
  ASSERT_EQ(writer.close(), Status::ok);
  const auto segment = test::first_with_extension(configuration.directory, ".ajl");
  auto contents = read_file(segment);
  ASSERT_GT(contents.size(), 400U);
  contents[192U + 160U + 3U] ^= 0xFFU;
  std::ofstream output(segment, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(contents.data()),
               static_cast<std::streamsize>(contents.size()));
  output.close();

  std::uint64_t replayed{};
  const auto status = replay_verified(
      configuration.directory, 0U, 0U,
      [](const PersistedRecord&) { return Status::ok; }, replayed);
  EXPECT_EQ(status, Status::corrupt_record);
  EXPECT_EQ(replayed, 0U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(JournalRecovery, RepairCanSalvagePrefixBeforeInternalCorruption) {
  const test::TemporaryDirectory temporary;
  const auto source = temporary.path() / "source";
  const auto target = temporary.path() / "target";
  auto configuration = test::config(source);
  configuration.maximum_records_per_segment = 2U;
  SegmentWriter writer;
  ASSERT_EQ(writer.open(configuration), Status::ok);
  for (std::uint64_t index = 0U; index < 3U; ++index) {
    const auto bytes = test::payload(index);
    ASSERT_EQ(
        writer.append({.metadata = test::metadata(index), .payload = bytes}).status,
        Status::ok);
  }
  ASSERT_EQ(writer.close(), Status::ok);
  const auto segment = source / "journal-00000000000000000001.ajl";
  ASSERT_TRUE(std::filesystem::exists(segment));
  auto contents = read_file(segment);
  constexpr std::size_t kFirstFrameBytes = 160U + 32U + 64U;
  const auto second_payload_offset = 192U + kFirstFrameBytes + 160U + 4U;
  ASSERT_LT(second_payload_offset, contents.size());
  contents[second_payload_offset] ^= 0x7FU;
  std::ofstream changed(segment, std::ios::binary | std::ios::trunc);
  changed.write(reinterpret_cast<const char*>(contents.data()),
                static_cast<std::streamsize>(contents.size()));
  changed.close();
  const auto source_digest = common::sha256(read_file(segment));

  RepairReport repair{};
  ASSERT_EQ(repair_copy(source, test::config(target), repair), Status::ok);
  EXPECT_EQ(repair.source_status, Status::corrupt_record);
  EXPECT_EQ(repair.copied_records, 1U);
  EXPECT_EQ(common::sha256(read_file(segment)), source_digest);
  const auto recovered = RecoveryScanner::scan_directory(target);
  EXPECT_EQ(recovered.status, Status::ok);
  EXPECT_EQ(recovered.valid_record_count, 1U);
}

} // namespace
} // namespace aegis::journal
