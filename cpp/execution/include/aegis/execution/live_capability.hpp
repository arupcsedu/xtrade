#ifndef AEGIS_EXECUTION_LIVE_CAPABILITY_HPP
#define AEGIS_EXECUTION_LIVE_CAPABILITY_HPP

#include "aegis/common/authentication.hpp"
#include "aegis/execution/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace aegis::execution {

inline constexpr std::size_t kMaximumConsumedLiveCapabilities = 4'096U;

enum class LiveCapabilityStatus : std::uint8_t {
  accepted = 1,
  invalid = 2,
  expired = 3,
  scope_mismatch = 4,
  replayed = 5,
  capacity_exhausted = 6,
};

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct LiveAuthorityEvidence {
  common::Sha256Digest configuration_sha256{};
  common::Sha256Digest operator_authorization_sha256{};
  common::Sha256Digest activation_record_sha256{};
  common::Sha256Digest fencing_grant_sha256{};
  std::uint64_t activation_journal_sequence{};
  std::uint64_t operator_authorization_valid_until_ns{};
};

struct LiveTransmissionCapability {
  std::uint16_t schema_major{1U};
  std::uint16_t schema_minor{0U};
  common::GlobalEventId capability_id;
  common::GlobalEventId command_id;
  common::SessionId session_id;
  std::uint64_t command_hash{};
  std::uint64_t risk_decision_hash{};
  std::uint64_t safety_state_hash{};
  common::Sha256Digest frame_sha256{};
  common::Sha256Digest configuration_sha256{};
  common::Sha256Digest operator_authorization_sha256{};
  common::Sha256Digest activation_record_sha256{};
  common::Sha256Digest fencing_grant_sha256{};
  std::uint64_t activation_journal_sequence{};
  std::uint64_t exchange_session_epoch{};
  std::uint64_t fencing_token{};
  std::uint64_t issued_process_monotonic_time_ns{};
  std::uint64_t valid_until_process_monotonic_time_ns{};
  common::Sha256Digest capability_key_id_sha256{};
  common::Sha256Digest authentication_tag{};
};

struct LiveTransmissionTrust {
  common::SessionId session_id;
  common::Sha256Digest configuration_sha256{};
  common::Sha256Digest capability_key_id_sha256{};
  common::HmacSha256Key capability_key{};
  std::uint64_t exchange_session_epoch{};
  std::uint64_t fencing_token{};
};

struct LiveCapabilitySigningKey {
  common::HmacSha256Key key{};
  common::Sha256Digest key_id_sha256{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] common::Sha256Digest
live_protocol_frame_sha256(const OpaqueProtocolFrame& frame) noexcept;
[[nodiscard]] bool
valid_live_transmission_capability(const LiveTransmissionCapability& value) noexcept;

class LiveTransmissionCapabilityIssuer final {
public:
  explicit LiveTransmissionCapabilityIssuer(
      LiveCapabilitySigningKey signing_key) noexcept;

  [[nodiscard]] bool issue(common::GlobalEventId capability_id,
                           const OpaqueProtocolFrame& frame,
                           const GatewayRequest& request,
                           const LiveAuthorityEvidence& authority,
                           LiveTransmissionCapability& output) const noexcept;

private:
  common::HmacSha256Key capability_key_{};
  common::Sha256Digest capability_key_id_sha256_{};
  bool initialized_{false};
};

class VerifiedLiveTransmissionCapability final {
public:
  [[nodiscard]] const LiveTransmissionCapability& evidence() const noexcept {
    return evidence_;
  }

private:
  friend class LiveTransmissionCapabilityVerifier;
  explicit VerifiedLiveTransmissionCapability(
      const LiveTransmissionCapability& evidence) noexcept
      : evidence_(evidence) {}

  LiveTransmissionCapability evidence_;
};

struct LiveCapabilityVerification {
  LiveCapabilityStatus status{LiveCapabilityStatus::invalid};
  std::optional<VerifiedLiveTransmissionCapability> capability;

  [[nodiscard]] bool accepted() const noexcept {
    return status == LiveCapabilityStatus::accepted && capability.has_value();
  }
};

class LiveTransmissionCapabilityVerifier final {
public:
  explicit LiveTransmissionCapabilityVerifier(LiveTransmissionTrust trust) noexcept;

  // This is the final per-frame authorization check. It is bounded,
  // allocation-free, consumes a capability at most once, and must precede the
  // licensed socket/session implementation.
  [[nodiscard]] LiveCapabilityVerification
  verify_and_consume(const OpaqueProtocolFrame& frame,
                     const LiveTransmissionCapability& capability,
                     std::uint64_t now_ns) noexcept;

private:
  [[nodiscard]] bool consumed(common::GlobalEventId capability_id) const noexcept;

  LiveTransmissionTrust trust_{};
  std::array<common::GlobalEventId, kMaximumConsumedLiveCapabilities> consumed_{};
  std::size_t consumed_count_{};
  bool initialized_{false};
};

} // namespace aegis::execution

#endif // AEGIS_EXECUTION_LIVE_CAPABILITY_HPP
