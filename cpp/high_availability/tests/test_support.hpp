#ifndef AEGIS_HIGH_AVAILABILITY_TEST_SUPPORT_HPP
#define AEGIS_HIGH_AVAILABILITY_TEST_SUPPORT_HPP

#include "aegis/high_availability/coordinator.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace aegis::high_availability::test {

inline constexpr common::SessionId kSession{1U, 1U};
inline constexpr common::ConfigurationVersion kConfiguration{2U, 1U};
inline constexpr std::uint64_t kExchangeEpoch = 77U;
inline constexpr std::uint64_t kStart = 1'000U;
inline constexpr common::Ed25519PrivateKey kWitnessPrivateKey{
    1U,  2U,  3U,  4U,  5U,  6U,  7U,  8U,  9U,  10U, 11U, 12U, 13U, 14U, 15U, 16U,
    17U, 18U, 19U, 20U, 21U, 22U, 23U, 24U, 25U, 26U, 27U, 28U, 29U, 30U, 31U, 32U};

[[nodiscard]] inline common::Sha256Digest
digest(const std::string_view value) noexcept {
  return common::sha256(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

[[nodiscard]] inline HaConfiguration configuration(
    const std::uint64_t node_id = 1U, const std::uint64_t process_id = 101U,
    const std::uint64_t process_epoch = 1U,
    const DeploymentLocality locality = DeploymentLocality::colocated_edge) noexcept {
  HaConfiguration result{.session_id = kSession,
                         .configuration_version = kConfiguration,
                         .node_id = node_id,
                         .process_id = process_id,
                         .process_epoch = process_epoch,
                         .exchange_session_epoch = kExchangeEpoch,
                         .heartbeat_timeout_ns = 500U,
                         .peer_timeout_ns = 100U,
                         .locality = locality};
  static_cast<void>(
      common::ed25519_derive_public_key(kWitnessPrivateKey, result.witness_public_key));
  result.witness_key_id_sha256 = digest("aegis-test-witness-key-v1");
  result.witness_trust_root_sha256 = digest("aegis-test-trust-root-v1");
  result.stable_hash = stable_ha_configuration_hash(result);
  return result;
}

[[nodiscard]] inline FencingGrant
signed_grant(const HaConfiguration& config, const std::uint64_t token,
             const std::uint64_t previous_token, const std::uint64_t sequence,
             const std::uint64_t issued_ns, const std::uint64_t expires_ns) noexcept {
  FencingGrant result{.session_id = config.session_id,
                      .node_id = config.node_id,
                      .process_epoch = config.process_epoch,
                      .exchange_session_epoch = config.exchange_session_epoch,
                      .fencing_token = token,
                      .previous_fencing_token = previous_token,
                      .grant_sequence = sequence,
                      .issued_process_monotonic_time_ns = issued_ns,
                      .valid_until_process_monotonic_time_ns = expires_ns,
                      .quorum_confirmed = true,
                      .previous_owner_fenced = true,
                      .witness_key_id_sha256 = config.witness_key_id_sha256,
                      .witness_trust_root_sha256 = config.witness_trust_root_sha256};
  static_cast<void>(sign_fencing_grant(result, kWitnessPrivateKey));
  return result;
}

[[nodiscard]] inline VerifiedFencingGrant
grant(const HaConfiguration& config, const std::uint64_t token,
      const std::uint64_t previous_token, const std::uint64_t sequence,
      const std::uint64_t issued_ns, const std::uint64_t expires_ns) noexcept {
  const FencingGrantVerifier verifier(
      {.witness_public_key = config.witness_public_key,
       .witness_key_id_sha256 = config.witness_key_id_sha256,
       .witness_trust_root_sha256 = config.witness_trust_root_sha256});
  VerifiedFencingGrant result;
  static_cast<void>(verifier.verify(
      signed_grant(config, token, previous_token, sequence, issued_ns, expires_ns),
      issued_ns, result));
  return result;
}

[[nodiscard]] inline DependencyHealth
healthy(const std::uint64_t now_ns, const std::uint64_t risk_epoch = 7U,
        const bool peer_available = true) noexcept {
  return {.observed_process_monotonic_time_ns = now_ns,
          .gateway_session_epoch = kExchangeEpoch,
          .risk_service_epoch = risk_epoch,
          .gateway_connected = true,
          .risk_ready = true,
          .journal_ready = true,
          .shared_memory_current = true,
          .partial_colocation_outage = false,
          .standby_heartbeat_expected = true,
          .peer = {.node_id = 2U,
                   .process_epoch = 1U,
                   .fencing_token = 0U,
                   .heartbeat_process_monotonic_time_ns = now_ns,
                   .state = LeadershipState::hot_standby,
                   .observed = peer_available}};
}

[[nodiscard]] inline ReconciliationEvidence
evidence(const std::uint64_t token, const std::uint64_t now_ns,
         const RecoveryStreamSnapshot& recovery,
         const std::uint64_t risk_epoch = 7U) noexcept {
  ReconciliationEvidence result{
      .fencing_token = token,
      .exchange_session_epoch = kExchangeEpoch,
      .risk_service_epoch = risk_epoch,
      .source_journal_sequence = recovery.latest_source_journal_sequence,
      .source_journal_hash = recovery.latest_source_journal_hash,
      .observed_process_monotonic_time_ns = now_ns,
      .oms_reconciled = true,
      .gateway_reconciled = true,
      .positions_reconciled = true,
      .risk_reconciled = true,
      .journal_verified = true};
  result.stable_hash = stable_reconciliation_evidence_hash(result);
  return result;
}

[[nodiscard]] inline EmissionRequest
emission(const HaConfiguration& config, const std::uint64_t token,
         const std::uint64_t ordinal, const std::uint64_t now_ns,
         const std::uint64_t command_hash = 9'000U) noexcept {
  EmissionRequest result{.session_id = config.session_id,
                         .command_id = common::GlobalEventId{8U, ordinal},
                         .command_hash = command_hash,
                         .exchange_session_epoch = config.exchange_session_epoch,
                         .fencing_token = token,
                         .process_epoch = config.process_epoch,
                         .source_journal_sequence = ordinal,
                         .source_journal_hash = 10'000U + ordinal,
                         .process_monotonic_time_ns = now_ns};
  result.stable_hash = stable_emission_request_hash(result);
  return result;
}

[[nodiscard]] inline RecoveryPayload
payload(const RecoveryAuthority authority, const RecoveryRecordKind kind,
        const std::uint64_t ordinal, const common::GlobalEventId command_id = {},
        const std::uint64_t command_hash = 0U) noexcept {
  RecoveryPayload result{.kind = kind,
                         .command_id = command_id,
                         .command_hash = command_hash,
                         .source_journal_sequence = ordinal,
                         .source_journal_hash = 4'000U + ordinal,
                         .process_monotonic_time_ns = kStart + ordinal,
                         .authority = authority};
  result.stable_hash = stable_recovery_payload_hash(result);
  return result;
}

} // namespace aegis::high_availability::test

#endif // AEGIS_HIGH_AVAILABILITY_TEST_SUPPORT_HPP
