#include "aegis/execution/live_capability.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <span>
#include <string_view>

namespace aegis::execution::test {
namespace {

[[nodiscard]] common::Sha256Digest digest(const std::string_view value) noexcept {
  return common::sha256(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

[[nodiscard]] OpaqueProtocolFrame frame() noexcept {
  OpaqueProtocolFrame value{.boundary = ProtocolBoundaryKind::native_protocol,
                            .byte_count = 4U};
  value.bytes[0U] = 0x10U;
  value.bytes[1U] = 0x20U;
  value.bytes[2U] = 0x30U;
  value.bytes[3U] = 0x40U;
  return value;
}

[[nodiscard]] common::HmacSha256Key capability_key() noexcept {
  common::HmacSha256Key value{};
  for (std::size_t index = 0U; index < value.size(); ++index) {
    value[index] = static_cast<std::uint8_t>(index + 1U);
  }
  return value;
}

[[nodiscard]] LiveAuthorityEvidence authority() noexcept {
  return {.configuration_sha256 = digest("signed-config-v7"),
          .operator_authorization_sha256 = digest("operator-grant-v3"),
          .activation_record_sha256 = digest("durable-activation-record-v2"),
          .fencing_grant_sha256 = digest("signed-fencing-grant-v5"),
          .activation_journal_sequence = 44U,
          .operator_authorization_valid_until_ns = kNow + 10'000U};
}

[[nodiscard]] LiveTransmissionTrust trust() noexcept {
  return {.session_id = risk::test::kSession,
          .configuration_sha256 = authority().configuration_sha256,
          .capability_key_id_sha256 = digest("live-capability-key-v1"),
          .capability_key = capability_key(),
          .exchange_session_epoch = kAuthority.exchange_session_epoch,
          .fencing_token = kAuthority.fencing_token};
}

[[nodiscard]] LiveTransmissionCapability capability() noexcept {
  const LiveTransmissionCapabilityIssuer issuer(
      {.key = capability_key(), .key_id_sha256 = trust().capability_key_id_sha256});
  LiveTransmissionCapability value;
  const auto issued = issuer.issue(common::GlobalEventId{900U, 1U}, frame(), request(),
                                   authority(), value);
  return issued ? value : LiveTransmissionCapability{};
}

TEST(LiveTransmissionCapabilityTest, BindsExactFrameAndConsumesOnlyOnce) {
  auto verifier = LiveTransmissionCapabilityVerifier(trust());
  const auto value = capability();
  ASSERT_TRUE(valid_live_transmission_capability(value));
  const auto accepted = verifier.verify_and_consume(frame(), value, kNow + 1U);
  ASSERT_TRUE(accepted.accepted());
  ASSERT_TRUE(accepted.capability.has_value());
  // ASSERT_TRUE is fatal, but the optional-access checker does not model the
  // control flow embedded in GoogleTest's assertion macro.
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(accepted.capability->evidence().capability_id, value.capability_id);
  EXPECT_EQ(verifier.verify_and_consume(frame(), value, kNow + 2U).status,
            LiveCapabilityStatus::replayed);
}

TEST(LiveTransmissionCapabilityTest, RejectsTamperingWrongTrustAndExpiry) {
  {
    auto changed_frame = frame();
    changed_frame.bytes[0U] ^= 1U;
    auto verifier = LiveTransmissionCapabilityVerifier(trust());
    EXPECT_EQ(
        verifier.verify_and_consume(changed_frame, capability(), kNow + 1U).status,
        LiveCapabilityStatus::scope_mismatch);
  }
  {
    auto changed = capability();
    changed.command_hash ^= 1U;
    auto verifier = LiveTransmissionCapabilityVerifier(trust());
    EXPECT_EQ(verifier.verify_and_consume(frame(), changed, kNow + 1U).status,
              LiveCapabilityStatus::invalid);
  }
  {
    auto wrong_trust = trust();
    wrong_trust.configuration_sha256 = digest("different-config");
    auto verifier = LiveTransmissionCapabilityVerifier(wrong_trust);
    EXPECT_EQ(verifier.verify_and_consume(frame(), capability(), kNow + 1U).status,
              LiveCapabilityStatus::scope_mismatch);
  }
  {
    auto verifier = LiveTransmissionCapabilityVerifier(trust());
    EXPECT_EQ(verifier.verify_and_consume(frame(), capability(), kNow + 20'000U).status,
              LiveCapabilityStatus::expired);
  }
}

TEST(LiveTransmissionCapabilityTest, FailsClosedWithoutEveryAuthorityDigest) {
  auto evidence = authority();
  evidence.activation_record_sha256.fill(0U);
  const LiveTransmissionCapabilityIssuer issuer(
      {.key = capability_key(), .key_id_sha256 = trust().capability_key_id_sha256});
  LiveTransmissionCapability output;
  EXPECT_FALSE(issuer.issue(common::GlobalEventId{900U, 2U}, frame(), request(),
                            evidence, output));
  EXPECT_FALSE(valid_live_transmission_capability(output));
}

} // namespace
} // namespace aegis::execution::test
