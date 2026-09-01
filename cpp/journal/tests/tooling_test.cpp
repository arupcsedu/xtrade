#include "test_support.hpp"

#include "../src/cli.hpp"

#include "aegis/journal/journal.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace aegis::journal {
namespace {

// GoogleTest assertion macros inflate this integration test's apparent branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(JournalTools, InspectVerifyRepairExportAndReplayUseVerifiedRecords) {
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

  auto source = configuration.directory.string();
  auto json = (temporary.path() / "export.jsonl").string();
  auto replay = (temporary.path() / "replay.ajrp").string();
  auto repaired = (temporary.path() / "repaired").string();
  std::string inspect_name = "journal-inspect";
  std::string verify_name = "journal-verify";
  std::string repair_name = "journal-repair-copy";
  std::string export_name = "journal-export";
  std::string replay_name = "journal-replay";
  std::array<char*, 2> inspect_args{inspect_name.data(), source.data()};
  std::array<char*, 2> verify_args{verify_name.data(), source.data()};
  std::array<char*, 3> repair_args{repair_name.data(), source.data(), repaired.data()};
  std::array<char*, 3> export_args{export_name.data(), source.data(), json.data()};
  std::array<char*, 3> replay_args{replay_name.data(), source.data(), replay.data()};
  EXPECT_EQ(cli::inspect_main(2, inspect_args.data()), 0);
  EXPECT_EQ(cli::verify_main(2, verify_args.data()), 0);
  EXPECT_EQ(cli::repair_main(3, repair_args.data()), 0);
  EXPECT_EQ(cli::export_main(3, export_args.data()), 0);
  EXPECT_EQ(cli::replay_main(3, replay_args.data()), 0);
  EXPECT_GT(std::filesystem::file_size(json), 0U);
  EXPECT_GT(std::filesystem::file_size(replay), 8U);

  std::ifstream exported(json);
  std::string first_line;
  ASSERT_TRUE(static_cast<bool>(std::getline(exported, first_line)));
  EXPECT_NE(first_line.find(R"("sequence":1)"), std::string::npos);
  EXPECT_NE(first_line.find(R"("payload_sha256")"), std::string::npos);
  const auto repaired_report = RecoveryScanner::scan_directory(repaired);
  EXPECT_EQ(repaired_report.status, Status::ok);
  EXPECT_EQ(repaired_report.valid_record_count, 2U);
}

} // namespace
} // namespace aegis::journal
