#include "aegis/execution/live_capability.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aegis::execution {
namespace {

inline constexpr std::size_t kCanonicalCapabilityBytes = 512U;

void put_u16(std::array<std::uint8_t, kCanonicalCapabilityBytes>& output,
             std::size_t& offset, const std::uint16_t value) noexcept {
  output[offset++] = static_cast<std::uint8_t>(value >> 8U);
  output[offset++] = static_cast<std::uint8_t>(value);
}

void put_u64(std::array<std::uint8_t, kCanonicalCapabilityBytes>& output,
             std::size_t& offset, const std::uint64_t value) noexcept {
  for (std::size_t index = 0U; index < 8U; ++index) {
    output[offset++] = static_cast<std::uint8_t>(value >> ((7U - index) * 8U));
  }
}

template <typename Identifier>
void put_identifier(std::array<std::uint8_t, kCanonicalCapabilityBytes>& output,
                    std::size_t& offset, const Identifier& value) noexcept {
  put_u64(output, offset, value.high());
  put_u64(output, offset, value.low());
}

void put_digest(std::array<std::uint8_t, kCanonicalCapabilityBytes>& output,
                std::size_t& offset, const common::Sha256Digest& digest) noexcept {
  std::ranges::copy(digest, output.begin() + static_cast<std::ptrdiff_t>(offset));
  offset += digest.size();
}

[[nodiscard]] std::array<std::uint8_t, kCanonicalCapabilityBytes>
canonical_capability(const LiveTransmissionCapability& value,
                     std::size_t& size) noexcept {
  std::array<std::uint8_t, kCanonicalCapabilityBytes> output{};
  constexpr std::array<std::uint8_t, 18U> domain{'A', 'E', 'G', 'I', 'S', '-',
                                                 'L', 'I', 'V', 'E', '-', 'C',
                                                 'A', 'P', '-', 'V', '0', '1'};
  std::ranges::copy(domain, output.begin());
  size = domain.size();
  put_u16(output, size, value.schema_major);
  put_u16(output, size, value.schema_minor);
  put_identifier(output, size, value.capability_id);
  put_identifier(output, size, value.command_id);
  put_identifier(output, size, value.session_id);
  put_u64(output, size, value.command_hash);
  put_u64(output, size, value.risk_decision_hash);
  put_u64(output, size, value.safety_state_hash);
  put_digest(output, size, value.frame_sha256);
  put_digest(output, size, value.configuration_sha256);
  put_digest(output, size, value.operator_authorization_sha256);
  put_digest(output, size, value.activation_record_sha256);
  put_digest(output, size, value.fencing_grant_sha256);
  put_u64(output, size, value.activation_journal_sequence);
  put_u64(output, size, value.exchange_session_epoch);
  put_u64(output, size, value.fencing_token);
  put_u64(output, size, value.issued_process_monotonic_time_ns);
  put_u64(output, size, value.valid_until_process_monotonic_time_ns);
  put_digest(output, size, value.capability_key_id_sha256);
  return output;
}

[[nodiscard]] bool valid_frame(const OpaqueProtocolFrame& frame) noexcept {
  return (frame.boundary == ProtocolBoundaryKind::native_protocol ||
          frame.boundary == ProtocolBoundaryKind::fix_compatible) &&
         frame.byte_count != 0U && frame.byte_count <= frame.bytes.size();
}

[[nodiscard]] bool nonzero_authority(const LiveAuthorityEvidence& value) noexcept {
  return !common::is_zero_digest(value.configuration_sha256) &&
         !common::is_zero_digest(value.operator_authorization_sha256) &&
         !common::is_zero_digest(value.activation_record_sha256) &&
         !common::is_zero_digest(value.fencing_grant_sha256) &&
         value.activation_journal_sequence != 0U &&
         value.operator_authorization_valid_until_ns != 0U;
}

[[nodiscard]] bool authenticate(const common::HmacSha256Key& key,
                                const LiveTransmissionCapability& value,
                                common::Sha256Digest& tag) noexcept {
  std::size_t size{};
  const auto bytes = canonical_capability(value, size);
  return common::hmac_sha256(key, std::span<const std::uint8_t>(bytes.data(), size),
                             tag);
}

} // namespace

common::Sha256Digest
live_protocol_frame_sha256(const OpaqueProtocolFrame& frame) noexcept {
  if (!valid_frame(frame)) {
    return {};
  }
  std::array<std::uint8_t, kMaximumProtocolFrameBytes + 3U> bytes{};
  bytes[0U] = static_cast<std::uint8_t>(frame.boundary);
  bytes[1U] = static_cast<std::uint8_t>(frame.byte_count >> 8U);
  bytes[2U] = static_cast<std::uint8_t>(frame.byte_count);
  std::ranges::copy_n(frame.bytes.begin(), frame.byte_count, bytes.begin() + 3U);
  return common::sha256(
      std::span<const std::uint8_t>(bytes.data(), frame.byte_count + 3U));
}

bool valid_live_transmission_capability(
    const LiveTransmissionCapability& value) noexcept {
  return value.schema_major == 1U && value.schema_minor == 0U &&
         value.capability_id.valid() && value.command_id.valid() &&
         value.session_id.valid() && value.command_hash != 0U &&
         value.risk_decision_hash != 0U && value.safety_state_hash != 0U &&
         !common::is_zero_digest(value.frame_sha256) &&
         !common::is_zero_digest(value.configuration_sha256) &&
         !common::is_zero_digest(value.operator_authorization_sha256) &&
         !common::is_zero_digest(value.activation_record_sha256) &&
         !common::is_zero_digest(value.fencing_grant_sha256) &&
         value.activation_journal_sequence != 0U &&
         value.exchange_session_epoch != 0U && value.fencing_token != 0U &&
         value.issued_process_monotonic_time_ns != 0U &&
         value.valid_until_process_monotonic_time_ns >=
             value.issued_process_monotonic_time_ns &&
         !common::is_zero_digest(value.capability_key_id_sha256) &&
         !common::is_zero_digest(value.authentication_tag);
}

LiveTransmissionCapabilityIssuer::LiveTransmissionCapabilityIssuer(
    const LiveCapabilitySigningKey signing_key) noexcept
    : capability_key_(signing_key.key),
      capability_key_id_sha256_(signing_key.key_id_sha256),
      initialized_(!common::is_zero_digest(capability_key_) &&
                   !common::is_zero_digest(capability_key_id_sha256_)) {}

bool LiveTransmissionCapabilityIssuer::issue(
    const common::GlobalEventId capability_id, const OpaqueProtocolFrame& frame,
    const GatewayRequest& request, const LiveAuthorityEvidence& authority,
    LiveTransmissionCapability& output) const noexcept {
  output = {};
  const auto now_ns = request.safety.observed_process_monotonic_time_ns;
  if (!initialized_ || !capability_id.valid() || !valid_frame(frame) ||
      !valid_gateway_request(request) || !nonzero_authority(authority) ||
      authority.operator_authorization_valid_until_ns < now_ns ||
      request.risk_decision.valid_until_process_monotonic_time_ns < now_ns) {
    return false;
  }
  LiveTransmissionCapability result{
      .capability_id = capability_id,
      .command_id = request.command.command_id,
      .session_id = request.command.session_id,
      .command_hash = request.command.stable_hash,
      .risk_decision_hash = request.risk_decision.stable_hash,
      .safety_state_hash = request.safety.stable_hash,
      .frame_sha256 = live_protocol_frame_sha256(frame),
      .configuration_sha256 = authority.configuration_sha256,
      .operator_authorization_sha256 = authority.operator_authorization_sha256,
      .activation_record_sha256 = authority.activation_record_sha256,
      .fencing_grant_sha256 = authority.fencing_grant_sha256,
      .activation_journal_sequence = authority.activation_journal_sequence,
      .exchange_session_epoch = request.command.authority.exchange_session_epoch,
      .fencing_token = request.command.authority.fencing_token,
      .issued_process_monotonic_time_ns = now_ns,
      .valid_until_process_monotonic_time_ns =
          std::min(request.risk_decision.valid_until_process_monotonic_time_ns,
                   authority.operator_authorization_valid_until_ns),
      .capability_key_id_sha256 = capability_key_id_sha256_};
  if (!authenticate(capability_key_, result, result.authentication_tag) ||
      !valid_live_transmission_capability(result)) {
    return false;
  }
  output = result;
  return true;
}

LiveTransmissionCapabilityVerifier::LiveTransmissionCapabilityVerifier(
    LiveTransmissionTrust trust) noexcept
    : trust_(trust),
      initialized_(trust_.session_id.valid() &&
                   !common::is_zero_digest(trust_.configuration_sha256) &&
                   !common::is_zero_digest(trust_.capability_key_id_sha256) &&
                   !common::is_zero_digest(trust_.capability_key) &&
                   trust_.exchange_session_epoch != 0U && trust_.fencing_token != 0U) {}

bool LiveTransmissionCapabilityVerifier::consumed(
    const common::GlobalEventId capability_id) const noexcept {
  for (std::size_t index = 0U; index < consumed_count_; ++index) {
    if (consumed_[index] == capability_id) {
      return true;
    }
  }
  return false;
}

LiveCapabilityVerification LiveTransmissionCapabilityVerifier::verify_and_consume(
    const OpaqueProtocolFrame& frame, const LiveTransmissionCapability& capability,
    const std::uint64_t now_ns) noexcept {
  if (!initialized_ || !valid_frame(frame) ||
      !valid_live_transmission_capability(capability)) {
    return {.status = LiveCapabilityStatus::invalid, .capability = std::nullopt};
  }
  if (now_ns < capability.issued_process_monotonic_time_ns ||
      now_ns > capability.valid_until_process_monotonic_time_ns) {
    return {.status = LiveCapabilityStatus::expired, .capability = std::nullopt};
  }
  if (capability.session_id != trust_.session_id ||
      capability.configuration_sha256 != trust_.configuration_sha256 ||
      capability.capability_key_id_sha256 != trust_.capability_key_id_sha256 ||
      capability.exchange_session_epoch != trust_.exchange_session_epoch ||
      capability.fencing_token != trust_.fencing_token ||
      capability.frame_sha256 != live_protocol_frame_sha256(frame)) {
    return {.status = LiveCapabilityStatus::scope_mismatch, .capability = std::nullopt};
  }
  common::Sha256Digest expected{};
  if (!authenticate(trust_.capability_key, capability, expected) ||
      !common::constant_time_equal(expected, capability.authentication_tag)) {
    return {.status = LiveCapabilityStatus::invalid, .capability = std::nullopt};
  }
  if (consumed(capability.capability_id)) {
    return {.status = LiveCapabilityStatus::replayed, .capability = std::nullopt};
  }
  if (consumed_count_ == consumed_.size()) {
    return {.status = LiveCapabilityStatus::capacity_exhausted,
            .capability = std::nullopt};
  }
  consumed_[consumed_count_++] = capability.capability_id;
  return {.status = LiveCapabilityStatus::accepted,
          .capability = VerifiedLiveTransmissionCapability{capability}};
}

} // namespace aegis::execution
