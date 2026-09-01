#include "aegis/common/audit_envelope.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

namespace {

using aegis::common::AuditMetadata;
using aegis::common::ChannelId;
using aegis::common::ConfigurationVersion;
using aegis::common::DataQualityContractInput;
using aegis::common::GlobalEventId;
using aegis::common::SessionId;
using aegis::common::ValidationError;
using aegis::common::VenueId;
namespace wire = aegis::mx::contracts::v1;

constexpr DataQualityContractInput kContractInput{
    .record_id = GlobalEventId{0x101U, 0x102U},
    .venue_id = VenueId{0x201U, 0x202U},
    .channel_id = ChannelId{0x301U, 0x302U},
    .state = wire::DataQualityCode::VALID,
    .observed_process_monotonic_time_ns = 5'000'000U,
    .last_good_exchange_event_time_ns = 1'800'000'000'000'000'000LL,
    .last_good_nic_receive_time_ns = 1'800'000'000'000'000'500LL,
    .missing_sequence_count = 0,
    .malformed_record_count = 0,
    .stale_after_ns = 250'000'000U,
};

constexpr AuditMetadata kAuditMetadata{
    .envelope_id = GlobalEventId{0x401U, 0x402U},
    .session_id = SessionId{0x501U, 0x502U},
    .configuration_version = ConfigurationVersion{0x601U, 0x602U},
    .created_process_monotonic_time_ns = 5'000'100U,
    .recorded_wall_clock_utc_time_ns = 1'800'000'000'100'000'000LL,
    .previous_envelope_sha256 = {},
};

[[nodiscard]] std::vector<std::uint8_t> canonical_envelope() {
  const auto contract = aegis::common::build_data_quality_contract(kContractInput);
  const auto envelope = aegis::common::build_size_prefixed_audit_envelope(
      {contract.data(), contract.size()}, wire::RecordType::DATA_QUALITY_STATE,
      kAuditMetadata);
  return {envelope.data(), envelope.data() + envelope.size()};
}

// GoogleTest assertions expand to branching macros that inflate this metric.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(AuditEnvelopeTest, VerifiesAndReturnsZeroCopyView) {
  const auto encoded = canonical_envelope();
  aegis::common::ValidatedAuditEnvelopeView view{};
  const auto result =
      aegis::common::validate_size_prefixed_audit_envelope(encoded, &view);

  ASSERT_TRUE(result.ok());
  ASSERT_NE(view.envelope, nullptr);
  ASSERT_NE(view.record, nullptr);
  EXPECT_EQ(view.record->record_type(), wire::RecordType::DATA_QUALITY_STATE);
  const auto* data_quality = view.record->payload_as_DataQualityState();
  if (data_quality == nullptr) {
    FAIL() << "validated payload type did not match the record type";
    return;
  }
  EXPECT_EQ(data_quality->state(), wire::DataQualityCode::VALID);
  EXPECT_GE(view.payload.data(), encoded.data());
  EXPECT_LT(view.payload.data(), encoded.data() + encoded.size());
}

TEST(AuditEnvelopeTest, RejectsTruncationBeforeReading) {
  auto encoded = canonical_envelope();
  encoded.pop_back();
  const auto result = aegis::common::validate_size_prefixed_audit_envelope(encoded);
  EXPECT_EQ(result.error(), ValidationError::invalid_size_prefix);
}

TEST(AuditEnvelopeTest, RejectsPayloadCorruptionByChecksum) {
  auto encoded = canonical_envelope();
  const auto* envelope = wire::GetSizePrefixedAuditEnvelope(encoded.data());
  const auto* payload = envelope->payload();
  if (payload == nullptr || payload->empty()) {
    FAIL() << "test fixture has no payload";
    return;
  }
  const auto payload_offset =
      static_cast<std::size_t>(payload->Data() - encoded.data());
  encoded[payload_offset + payload->size() - 1U] ^= 0x01U;

  const auto result = aegis::common::validate_size_prefixed_audit_envelope(encoded);
  EXPECT_EQ(result.error(), ValidationError::checksum_mismatch);
}

TEST(AuditEnvelopeTest, RejectsUnknownEnumValue) {
  auto invalid_input = kContractInput;
  // Deliberately exercise semantic rejection beyond the generated enum range.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  invalid_input.state = static_cast<wire::DataQualityCode>(255U);
  const auto contract = aegis::common::build_data_quality_contract(invalid_input);
  const auto envelope = aegis::common::build_size_prefixed_audit_envelope(
      {contract.data(), contract.size()}, wire::RecordType::DATA_QUALITY_STATE,
      kAuditMetadata);

  const auto result = aegis::common::validate_size_prefixed_audit_envelope(
      {envelope.data(), envelope.size()});
  EXPECT_EQ(result.error(), ValidationError::invalid_enum);
}

TEST(AuditEnvelopeTest, RejectsDiscriminatorMismatch) {
  const auto contract = aegis::common::build_data_quality_contract(kContractInput);
  const auto envelope = aegis::common::build_size_prefixed_audit_envelope(
      {contract.data(), contract.size()}, wire::RecordType::CLOCK_QUALITY_STATE,
      kAuditMetadata);

  const auto result = aegis::common::validate_size_prefixed_audit_envelope(
      {envelope.data(), envelope.size()});
  EXPECT_EQ(result.error(), ValidationError::record_type_mismatch);
}

TEST(AuditEnvelopeTest, MatchesCrossLanguageGoldenFile) {
  const auto golden_path =
      std::filesystem::path{AEGIS_SOURCE_DIR} / "schemas/golden/data_quality_v1.amae";
  std::ifstream stream{golden_path, std::ios::binary};
  ASSERT_TRUE(stream.good()) << golden_path;
  const std::vector<std::uint8_t> golden{std::istreambuf_iterator<char>{stream},
                                         std::istreambuf_iterator<char>{}};
  const auto actual = canonical_envelope();
  EXPECT_EQ(actual, golden);
}

// GoogleTest assertions expand to branching macros that inflate this metric.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(AuditEnvelopeTest, ValidatesV14EventIntelligenceGoldenFile) {
  const auto golden_path = std::filesystem::path{AEGIS_SOURCE_DIR} /
                           "schemas/golden/event_intelligence_v1_4.amae";
  std::ifstream stream{golden_path, std::ios::binary};
  ASSERT_TRUE(stream.good()) << golden_path;
  const std::vector<std::uint8_t> golden{std::istreambuf_iterator<char>{stream},
                                         std::istreambuf_iterator<char>{}};
  aegis::common::ValidatedAuditEnvelopeView view{};
  const auto result =
      aegis::common::validate_size_prefixed_audit_envelope(golden, &view);
  ASSERT_TRUE(result.ok());
  ASSERT_NE(view.record, nullptr);
  EXPECT_EQ(view.record->record_type(), wire::RecordType::EVENT_INTELLIGENCE_RECORD);
  const auto* intelligence = view.record->payload_as_EventIntelligenceRecord();
  ASSERT_NE(intelligence, nullptr);
  EXPECT_EQ(intelligence->event_type(), wire::IntelligenceEventType::EARNINGS_RELEASE);
  EXPECT_EQ(intelligence->stage(), wire::IntelligenceStage::FAST);
  const auto* facts = intelligence->facts();
  const auto* entities = intelligence->entities();
  if (facts == nullptr || entities == nullptr) {
    FAIL() << "validated v1.4 intelligence collections are absent";
    return;
  }
  EXPECT_EQ(facts->size(), 2U);
  EXPECT_EQ(entities->size(), 1U);
}

} // namespace
