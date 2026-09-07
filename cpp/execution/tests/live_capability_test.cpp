#include "aegis/execution/interfaces.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

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
          .configuration_stable_hash = configuration().stable_hash,
          .capability_key_id_sha256 = digest("live-capability-key-v1"),
          .capability_key = capability_key(),
          .exchange_session_epoch = kAuthority.exchange_session_epoch,
          .fencing_token = kAuthority.fencing_token,
          .maximum_clock_age_ns = 1'000'000U};
}

[[nodiscard]] GatewayRequest authorized_request() {
  auto value = request();
  value.safety.signed_configuration_valid = true;
  value.safety.operator_authorized = true;
  value.safety.activation_record_durable = true;
  value.safety.activation_record_hash = 77U;
  value.safety.operator_authorization_valid_until_ns =
      authority().operator_authorization_valid_until_ns;
  value.safety.stable_hash = stable_final_safety_state_hash(value.safety);
  value.stable_hash = stable_gateway_request_hash(value);
  return value;
}

[[nodiscard]] LiveTransmissionCapability capability() noexcept {
  const LiveTransmissionCapabilityIssuer issuer(trust());
  LiveTransmissionCapability value;
  const auto issued = issuer.issue(common::GlobalEventId{900U, 1U}, frame(),
                                   authorized_request(), authority(), value);
  return issued ? value : LiveTransmissionCapability{};
}

class RecordingLiveAdapter final : public ILiveTransmissionAdapter {
public:
  [[nodiscard]] std::uint32_t calls() const noexcept { return calls_; }

protected:
  [[nodiscard]] AdapterStatus
  transmit_verified(const OpaqueProtocolFrame& frame_value,
                    const LiveTransmissionCapability& evidence) noexcept override {
    static_cast<void>(frame_value);
    static_cast<void>(evidence);
    ++calls_;
    return AdapterStatus::encoded;
  }

private:
  std::uint32_t calls_{};
};

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
  const LiveTransmissionCapabilityIssuer issuer(trust());
  LiveTransmissionCapability output;
  EXPECT_FALSE(issuer.issue(common::GlobalEventId{900U, 2U}, frame(),
                            authorized_request(), evidence, output));
  EXPECT_FALSE(valid_live_transmission_capability(output));
}

TEST(LiveTransmissionCapabilityTest, UnsafeFinalStateCannotIssueCapability) {
  auto unsafe = authorized_request();
  unsafe.safety.kill_switch_engaged = true;
  unsafe.safety.stable_hash = stable_final_safety_state_hash(unsafe.safety);
  unsafe.stable_hash = stable_gateway_request_hash(unsafe);
  const LiveTransmissionCapabilityIssuer issuer(trust());
  LiveTransmissionCapability output;
  EXPECT_FALSE(issuer.issue(common::GlobalEventId{900U, 3U}, frame(), unsafe,
                            authority(), output));

  unsafe = authorized_request();
  unsafe.safety.clock_quality_snapshot.operation_mode =
      time::ClockOperationMode::blocked;
  unsafe.safety.stable_hash = stable_final_safety_state_hash(unsafe.safety);
  unsafe.stable_hash = stable_gateway_request_hash(unsafe);
  EXPECT_FALSE(issuer.issue(common::GlobalEventId{900U, 4U}, frame(), unsafe,
                            authority(), output));
}

TEST(LiveTransmissionCapabilityTest, VerifiedAuthorityIsMoveOnlyAndConsumedBySend) {
  static_assert(!std::is_copy_constructible_v<VerifiedLiveTransmissionCapability>);
  static_assert(!std::is_copy_assignable_v<VerifiedLiveTransmissionCapability>);

  auto verifier = LiveTransmissionCapabilityVerifier(trust());
  auto verification = verifier.verify_and_consume(frame(), capability(), kNow + 1U);
  ASSERT_TRUE(verification.accepted());
  // ASSERT_TRUE is fatal, but the optional-access checker does not model the
  // control flow embedded in GoogleTest's assertion macro.
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  auto verified = std::move(verification.capability.value());
  RecordingLiveAdapter adapter;
  EXPECT_EQ(adapter.transmit(frame(), verified), AdapterStatus::encoded);
  EXPECT_FALSE(verified.valid());
  EXPECT_EQ(adapter.transmit(frame(), verified), AdapterStatus::authorization_invalid);
  EXPECT_EQ(adapter.calls(), 1U);

  auto second_verifier = LiveTransmissionCapabilityVerifier(trust());
  auto second = second_verifier.verify_and_consume(frame(), capability(), kNow + 1U);
  ASSERT_TRUE(second.accepted());
  auto changed_frame = frame();
  changed_frame.bytes[0U] ^= 1U;
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  auto second_verified = std::move(second.capability.value());
  EXPECT_EQ(adapter.transmit(changed_frame, second_verified),
            AdapterStatus::authorization_invalid);
  EXPECT_EQ(adapter.calls(), 1U);
}

} // namespace
} // namespace aegis::execution::test
