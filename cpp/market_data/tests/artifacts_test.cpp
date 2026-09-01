#include "aegis/market_data/synthetic/artifacts.hpp"
#include "aegis/market_data/synthetic/canonical.hpp"

#include "aegis/common/audit_envelope.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>

namespace synthetic = aegis::market_data::synthetic;

namespace {

[[nodiscard]] synthetic::ArtifactPaths paths_for(const synthetic::Scenario scenario) {
  const auto directory = std::filesystem::temp_directory_path() / "aegis_smx_replay" /
                         std::filesystem::current_path().relative_path() /
                         std::string{synthetic::scenario_name(scenario)};
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  return {
      .capture = directory / "events.smxcap",
      .canonical = directory / "events.amae",
      .expected_book = directory / "events.book.txt",
  };
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::error_code error;
  const auto byte_count = std::filesystem::file_size(path, error);
  if (!stream.is_open() || error ||
      byte_count >
          static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
    return {};
  }
  std::string bytes(static_cast<std::size_t>(byte_count), '\0');
  stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
    return {};
  }
  return bytes;
}

} // namespace

// GoogleTest assertion macros expand into control flow that clang-tidy counts as
// test-body complexity and cannot use to prove subsequent optional access safe.
// NOLINTBEGIN(readability-function-cognitive-complexity,bugprone-unchecked-optional-access)
TEST(SyntheticArtifacts, CanonicalMarketEventPassesContractValidation) {
  auto config = synthetic::make_default_config();
  auto generator = synthetic::SyntheticExchangeGenerator::create(config);
  ASSERT_TRUE(generator.has_value());
  synthetic::SyntheticEvent event{};
  ASSERT_TRUE(generator->next(event).ok());
  const auto envelope = synthetic::build_market_event_envelope(event, config, {});
  EXPECT_TRUE(aegis::common::validate_size_prefixed_audit_envelope(
                  std::span<const std::uint8_t>{envelope.data(), envelope.size()})
                  .ok());
}

TEST(SyntheticArtifacts, ReplaysEveryScenarioAndChecksFaultExpectations) {
  constexpr std::uint64_t kReplaySeed = 0x5A17'B00BU;
  for (auto raw = static_cast<std::uint16_t>(synthetic::Scenario::normal);
       raw <=
       static_cast<std::uint16_t>(synthetic::Scenario::hidden_liquidity_replenishment);
       ++raw) {
    auto config = synthetic::make_default_config();
    config.seed = kReplaySeed;
    config.event_count = 96U;
    config.scenario = static_cast<synthetic::Scenario>(raw);
    const auto paths = paths_for(config.scenario);
    const auto generated = synthetic::generate_artifacts(config, paths, false);
    ASSERT_EQ(generated.error, synthetic::ArtifactError::none)
        << synthetic::scenario_name(config.scenario) << ' '
        << synthetic::generation_error_name(generated.generation_error) << " after "
        << generated.logical_events;
    ASSERT_TRUE(synthetic::expected_physical_packet_count(config).has_value());
    EXPECT_EQ(generated.physical_packets,
              *synthetic::expected_physical_packet_count(config));
    const auto verified = synthetic::verify_artifacts(
        {.capture = paths.capture, .expected_book = paths.expected_book});
    EXPECT_TRUE(verified.ok()) << synthetic::scenario_name(config.scenario) << ' '
                               << synthetic::artifact_error_name(verified.error);
    EXPECT_TRUE(verified.scenario_expectation_met);
    if (config.scenario == synthetic::Scenario::missing_sequence ||
        config.scenario == synthetic::Scenario::out_of_order_packet) {
      EXPECT_FALSE(verified.final_book_matches);
    } else {
      EXPECT_TRUE(verified.final_book_matches);
    }
  }
}

TEST(SyntheticArtifacts, RepeatedRunProducesByteIdenticalCaptureAndBook) {
  auto config = synthetic::make_default_config();
  config.seed = 0xB17E'1D3AULL;
  config.event_count = 256U;
  config.scenario = synthetic::Scenario::duplicate_packet;
  const auto first = paths_for(config.scenario);
  auto second = first;
  second.capture = first.capture.parent_path() / "second.smxcap";
  second.canonical = first.capture.parent_path() / "second.amae";
  second.expected_book = first.capture.parent_path() / "second.book.txt";
  ASSERT_EQ(synthetic::generate_artifacts(config, first, false).error,
            synthetic::ArtifactError::none);
  ASSERT_EQ(synthetic::generate_artifacts(config, second, false).error,
            synthetic::ArtifactError::none);
  EXPECT_EQ(read_file(first.capture), read_file(second.capture));
  EXPECT_EQ(read_file(first.expected_book), read_file(second.expected_book));
}

TEST(SyntheticArtifacts, FailsClosedOnCaptureOrOracleCorruption) {
  auto config = synthetic::make_default_config();
  config.event_count = 64U;
  const auto paths = paths_for(config.scenario);
  ASSERT_EQ(synthetic::generate_artifacts(config, paths, false).error,
            synthetic::ArtifactError::none);
  {
    std::ofstream stream(paths.expected_book, std::ios::app);
    stream << "untrusted trailing text\n";
  }
  EXPECT_FALSE(synthetic::verify_artifacts(
                   {.capture = paths.capture, .expected_book = paths.expected_book})
                   .ok());

  ASSERT_EQ(synthetic::generate_artifacts(config, paths, false).error,
            synthetic::ArtifactError::none);
  {
    std::fstream stream(paths.capture, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(stream.is_open());
    const auto corruption_offset = static_cast<std::streamoff>(
        synthetic::kCaptureHeaderBytes + synthetic::kCaptureRecordHeaderBytes +
        synthetic::kPacketHeaderBytes + 12U);
    stream.seekg(corruption_offset);
    char value{};
    stream.read(&value, 1);
    stream.seekp(corruption_offset);
    value = static_cast<char>(static_cast<unsigned char>(value) ^ 0x01U);
    stream.write(&value, 1);
  }
  EXPECT_FALSE(synthetic::verify_artifacts(
                   {.capture = paths.capture, .expected_book = paths.expected_book})
                   .ok());
}
// NOLINTEND(readability-function-cognitive-complexity,bugprone-unchecked-optional-access)
