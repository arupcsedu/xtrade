#include "aegis/high_availability/coordinator.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace aegis::high_availability {
namespace {

constexpr std::uint64_t kHashOffset = 1'469'598'103'934'665'603ULL;
constexpr std::uint64_t kHashPrime = 1'099'511'628'211ULL;

void mix(std::uint64_t& hash, const std::uint64_t value) noexcept {
  hash ^= value;
  hash *= kHashPrime;
}

template <std::size_t Size>
void mix_bytes(std::uint64_t& hash,
               const std::array<std::uint8_t, Size>& value) noexcept {
  for (const auto byte : value) {
    mix(hash, byte);
  }
}

void put_u64(std::array<std::uint8_t, 192U>& output, std::size_t& offset,
             const std::uint64_t value) noexcept {
  for (std::size_t index = 0U; index < 8U; ++index) {
    output[offset++] = static_cast<std::uint8_t>(value >> ((7U - index) * 8U));
  }
}

template <typename Identifier>
void put_identifier(std::array<std::uint8_t, 192U>& output, std::size_t& offset,
                    const Identifier& value) noexcept {
  put_u64(output, offset, value.high());
  put_u64(output, offset, value.low());
}

[[nodiscard]] std::array<std::uint8_t, 192U>
fencing_grant_message(const FencingGrant& value, std::size_t& size) noexcept {
  std::array<std::uint8_t, 192U> output{};
  constexpr std::array<std::uint8_t, 17U> domain{'A', 'E', 'G', 'I', 'S', '-',
                                                 'H', 'A', '-', 'G', 'R', 'A',
                                                 'N', 'T', '-', 'V', '1'};
  std::ranges::copy(domain, output.begin());
  size = domain.size();
  put_identifier(output, size, value.session_id);
  put_u64(output, size, value.node_id);
  put_u64(output, size, value.process_epoch);
  put_u64(output, size, value.exchange_session_epoch);
  put_u64(output, size, value.fencing_token);
  put_u64(output, size, value.previous_fencing_token);
  put_u64(output, size, value.grant_sequence);
  put_u64(output, size, value.issued_process_monotonic_time_ns);
  put_u64(output, size, value.valid_until_process_monotonic_time_ns);
  output[size++] = value.quorum_confirmed ? 1U : 0U;
  output[size++] = value.previous_owner_fenced ? 1U : 0U;
  std::ranges::copy(value.witness_key_id_sha256,
                    output.begin() + static_cast<std::ptrdiff_t>(size));
  size += value.witness_key_id_sha256.size();
  std::ranges::copy(value.witness_trust_root_sha256,
                    output.begin() + static_cast<std::ptrdiff_t>(size));
  size += value.witness_trust_root_sha256.size();
  return output;
}

template <typename Identifier>
void mix_identifier(std::uint64_t& hash, const Identifier& value) noexcept {
  mix(hash, value.high());
  mix(hash, value.low());
}

[[nodiscard]] bool valid_locality(const DeploymentLocality value) noexcept {
  return value == DeploymentLocality::colocated_edge ||
         value == DeploymentLocality::remote_region;
}

[[nodiscard]] bool active_state(const LeadershipState value) noexcept {
  return value == LeadershipState::active_leader ||
         value == LeadershipState::active_degraded;
}

[[nodiscard]] std::uint64_t snapshot_checksum(const void* value) noexcept {
  return stable_leadership_snapshot_hash(
      *static_cast<const LeadershipSnapshot*>(value));
}

} // namespace

std::uint64_t stable_ha_configuration_hash(HaConfiguration value) noexcept {
  value.stable_hash = 0U;
  auto hash = kHashOffset;
  mix_identifier(hash, value.session_id);
  mix_identifier(hash, value.configuration_version);
  mix(hash, value.node_id);
  mix(hash, value.process_id);
  mix(hash, value.process_epoch);
  mix(hash, value.exchange_session_epoch);
  mix(hash, value.heartbeat_timeout_ns);
  mix(hash, value.peer_timeout_ns);
  mix(hash, static_cast<std::uint64_t>(value.locality));
  mix_bytes(hash, value.witness_public_key);
  mix_bytes(hash, value.witness_key_id_sha256);
  mix_bytes(hash, value.witness_trust_root_sha256);
  return hash == 0U ? 1U : hash;
}

std::uint64_t stable_fencing_grant_hash(FencingGrant value) noexcept {
  value.stable_hash = 0U;
  auto hash = kHashOffset;
  mix_identifier(hash, value.session_id);
  mix(hash, value.node_id);
  mix(hash, value.process_epoch);
  mix(hash, value.exchange_session_epoch);
  mix(hash, value.fencing_token);
  mix(hash, value.previous_fencing_token);
  mix(hash, value.grant_sequence);
  mix(hash, value.issued_process_monotonic_time_ns);
  mix(hash, value.valid_until_process_monotonic_time_ns);
  mix(hash, value.quorum_confirmed ? 1U : 0U);
  mix(hash, value.previous_owner_fenced ? 1U : 0U);
  mix_bytes(hash, value.witness_key_id_sha256);
  mix_bytes(hash, value.witness_trust_root_sha256);
  mix_bytes(hash, value.witness_signature);
  return hash == 0U ? 1U : hash;
}

common::Sha256Digest fencing_grant_signing_digest(const FencingGrant& value) noexcept {
  std::size_t size{};
  const auto message = fencing_grant_message(value, size);
  return common::sha256(std::span<const std::uint8_t>(message.data(), size));
}

bool sign_fencing_grant(FencingGrant& value,
                        const common::Ed25519PrivateKey& private_key) noexcept {
  const auto digest = fencing_grant_signing_digest(value);
  if (!common::ed25519_sign(private_key, digest, value.witness_signature)) {
    value.witness_signature.fill(0U);
    value.stable_hash = 0U;
    return false;
  }
  value.stable_hash = stable_fencing_grant_hash(value);
  return true;
}

bool VerifiedFencingGrant::valid() const noexcept { return verified_; }

const FencingGrant& VerifiedFencingGrant::value() const noexcept { return grant_; }

FencingGrantVerifier::FencingGrantVerifier(const FencingGrantTrust trust) noexcept
    : witness_public_key_(trust.witness_public_key),
      witness_key_id_sha256_(trust.witness_key_id_sha256),
      witness_trust_root_sha256_(trust.witness_trust_root_sha256),
      initialized_(!common::is_zero_digest(witness_public_key_) &&
                   !common::is_zero_digest(witness_key_id_sha256_) &&
                   !common::is_zero_digest(witness_trust_root_sha256_)) {}

bool FencingGrantVerifier::initialized() const noexcept { return initialized_; }

bool FencingGrantVerifier::verify(const FencingGrant& signed_grant,
                                  const std::uint64_t now_ns,
                                  VerifiedFencingGrant& output) const noexcept {
  output = {};
  if (!initialized_ || now_ns == 0U ||
      signed_grant.witness_key_id_sha256 != witness_key_id_sha256_ ||
      signed_grant.witness_trust_root_sha256 != witness_trust_root_sha256_ ||
      signed_grant.issued_process_monotonic_time_ns == 0U ||
      signed_grant.issued_process_monotonic_time_ns > now_ns ||
      now_ns > signed_grant.valid_until_process_monotonic_time_ns ||
      signed_grant.stable_hash == 0U ||
      signed_grant.stable_hash != stable_fencing_grant_hash(signed_grant)) {
    return false;
  }
  const auto digest = fencing_grant_signing_digest(signed_grant);
  if (!common::ed25519_verify(witness_public_key_, digest,
                              signed_grant.witness_signature)) {
    return false;
  }
  output.grant_ = signed_grant;
  output.verified_ = true;
  return true;
}

std::uint64_t
stable_reconciliation_evidence_hash(ReconciliationEvidence value) noexcept {
  value.stable_hash = 0U;
  auto hash = kHashOffset;
  mix(hash, value.fencing_token);
  mix(hash, value.exchange_session_epoch);
  mix(hash, value.risk_service_epoch);
  mix(hash, value.source_journal_sequence);
  mix(hash, value.source_journal_hash);
  mix(hash, value.observed_process_monotonic_time_ns);
  mix(hash, value.oms_reconciled ? 1U : 0U);
  mix(hash, value.gateway_reconciled ? 1U : 0U);
  mix(hash, value.positions_reconciled ? 1U : 0U);
  mix(hash, value.risk_reconciled ? 1U : 0U);
  mix(hash, value.journal_verified ? 1U : 0U);
  return hash == 0U ? 1U : hash;
}

std::uint64_t stable_emission_request_hash(EmissionRequest value) noexcept {
  value.stable_hash = 0U;
  auto hash = kHashOffset;
  mix_identifier(hash, value.session_id);
  mix_identifier(hash, value.command_id);
  mix(hash, value.command_hash);
  mix(hash, value.exchange_session_epoch);
  mix(hash, value.fencing_token);
  mix(hash, value.process_epoch);
  mix(hash, value.source_journal_sequence);
  mix(hash, value.source_journal_hash);
  mix(hash, value.process_monotonic_time_ns);
  return hash == 0U ? 1U : hash;
}

std::uint64_t stable_emission_decision_hash(EmissionDecision value) noexcept {
  value.stable_hash = 0U;
  auto hash = kHashOffset;
  mix(hash, static_cast<std::uint64_t>(value.status));
  mix(hash, static_cast<std::uint64_t>(value.reason));
  mix(hash, value.recovery_stream_sequence);
  mix(hash, value.recovery_record_hash);
  return hash == 0U ? 1U : hash;
}

std::uint64_t stable_leadership_snapshot_hash(LeadershipSnapshot value) noexcept {
  value.stable_hash = 0U;
  auto hash = kHashOffset;
  mix_identifier(hash, value.session_id);
  mix_identifier(hash, value.configuration_version);
  mix(hash, static_cast<std::uint64_t>(value.state));
  mix(hash, static_cast<std::uint64_t>(value.reason));
  mix(hash, static_cast<std::uint64_t>(value.locality));
  mix(hash, value.node_id);
  mix(hash, value.process_id);
  mix(hash, value.process_epoch);
  mix(hash, value.exchange_session_epoch);
  mix(hash, value.fencing_token);
  mix(hash, value.grant_sequence);
  mix(hash, value.grant_valid_until_ns);
  mix(hash, value.last_local_heartbeat_ns);
  mix(hash, value.risk_service_epoch);
  mix(hash, value.recovery_published_sequence);
  mix(hash, value.recovery_applied_sequence);
  mix(hash, value.transition_sequence);
  mix(hash, value.configuration_hash);
  mix(hash, value.can_emit_orders ? 1U : 0U);
  mix(hash, value.reconciliation_required ? 1U : 0U);
  mix(hash, value.standby_available ? 1U : 0U);
  mix(hash, value.operator_attention_required ? 1U : 0U);
  return hash == 0U ? 1U : hash;
}

std::uint64_t stable_ha_transition_hash(HaTransition value) noexcept {
  value.record_hash = 0U;
  auto hash = kHashOffset;
  mix(hash, value.sequence);
  mix(hash, static_cast<std::uint64_t>(value.previous_state));
  mix(hash, static_cast<std::uint64_t>(value.current_state));
  mix(hash, static_cast<std::uint64_t>(value.reason));
  mix(hash, value.fencing_token);
  mix(hash, value.process_epoch);
  mix(hash, value.process_monotonic_time_ns);
  mix(hash, value.previous_record_hash);
  return hash == 0U ? 1U : hash;
}

const char* to_string(const LeadershipState value) noexcept {
  switch (value) {
  case LeadershipState::starting:
    return "STARTING";
  case LeadershipState::standby_synchronizing:
    return "STANDBY_SYNCHRONIZING";
  case LeadershipState::hot_standby:
    return "HOT_STANDBY";
  case LeadershipState::leader_reconciling:
    return "LEADER_RECONCILING";
  case LeadershipState::active_leader:
    return "ACTIVE_LEADER";
  case LeadershipState::active_degraded:
    return "ACTIVE_DEGRADED";
  case LeadershipState::risk_reduction_only:
    return "RISK_REDUCTION_ONLY";
  case LeadershipState::fenced:
    return "FENCED";
  case LeadershipState::unsafe:
    return "UNSAFE";
  case LeadershipState::stopped:
    return "STOPPED";
  }
  return "UNKNOWN";
}

const char* to_string(const LeadershipReason value) noexcept {
  switch (value) {
  case LeadershipReason::none:
    return "none";
  case LeadershipReason::process_epoch_claimed:
    return "process_epoch_claimed";
  case LeadershipReason::process_epoch_conflict:
    return "process_epoch_conflict";
  case LeadershipReason::delayed_local_heartbeat:
    return "delayed_local_heartbeat";
  case LeadershipReason::process_authority_lost:
    return "process_authority_lost";
  case LeadershipReason::standby_synchronizing:
    return "standby_synchronizing";
  case LeadershipReason::standby_ready:
    return "standby_ready";
  case LeadershipReason::fencing_grant_received:
    return "fencing_grant_received";
  case LeadershipReason::fencing_grant_renewed:
    return "fencing_grant_renewed";
  case LeadershipReason::invalid_fencing_grant:
    return "invalid_fencing_grant";
  case LeadershipReason::fencing_lease_expired:
    return "fencing_lease_expired";
  case LeadershipReason::reconciliation_required:
    return "reconciliation_required";
  case LeadershipReason::reconciliation_completed:
    return "reconciliation_completed";
  case LeadershipReason::gateway_disconnected:
    return "gateway_disconnected";
  case LeadershipReason::risk_unavailable:
    return "risk_unavailable";
  case LeadershipReason::risk_epoch_changed:
    return "risk_epoch_changed";
  case LeadershipReason::journal_unavailable:
    return "journal_unavailable";
  case LeadershipReason::shared_memory_stale:
    return "shared_memory_stale";
  case LeadershipReason::recovery_stream_unavailable:
    return "recovery_stream_unavailable";
  case LeadershipReason::standby_unavailable:
    return "standby_unavailable";
  case LeadershipReason::peer_claims_leadership:
    return "peer_claims_leadership";
  case LeadershipReason::partial_colocation_outage:
    return "partial_colocation_outage";
  case LeadershipReason::remote_region_restricted:
    return "remote_region_restricted";
  case LeadershipReason::duplicate_order_suppressed:
    return "duplicate_order_suppressed";
  case LeadershipReason::command_identity_conflict:
    return "command_identity_conflict";
  case LeadershipReason::operator_fenced:
    return "operator_fenced";
  case LeadershipReason::shutdown:
    return "shutdown";
  }
  return "unknown";
}

EdgeHaCoordinator::EdgeHaCoordinator(
    HaConfiguration configuration,
    event_bus::ProcessEpochState& process_epoch_state) noexcept
    : configuration_(configuration), process_epoch_state_(process_epoch_state) {
  initialized_ =
      configuration_.session_id.valid() &&
      configuration_.configuration_version.valid() && configuration_.node_id != 0U &&
      configuration_.process_id != 0U && configuration_.process_epoch != 0U &&
      configuration_.exchange_session_epoch != 0U &&
      configuration_.heartbeat_timeout_ns != 0U &&
      configuration_.peer_timeout_ns != 0U && valid_locality(configuration_.locality) &&
      !common::is_zero_digest(configuration_.witness_public_key) &&
      !common::is_zero_digest(configuration_.witness_key_id_sha256) &&
      !common::is_zero_digest(configuration_.witness_trust_root_sha256) &&
      configuration_.stable_hash != 0U &&
      configuration_.stable_hash == stable_ha_configuration_hash(configuration_) &&
      snapshot_store_.initialize(snapshot_checksum);
  if (initialized_) {
    publish_snapshot();
  }
}

bool EdgeHaCoordinator::initialized() const noexcept { return initialized_; }

bool EdgeHaCoordinator::start(const std::uint64_t now_ns) noexcept {
  if (!initialized_ || stopped_ || started_ || now_ns == 0U) {
    return false;
  }
  const auto claim = event_bus::ProcessEpochMechanism::claim(
      process_epoch_state_, configuration_.process_id, configuration_.process_epoch,
      now_ns, configuration_.heartbeat_timeout_ns);
  if (claim != event_bus::EpochClaimStatus::acquired &&
      claim != event_bus::EpochClaimStatus::renewed &&
      claim != event_bus::EpochClaimStatus::stale_takeover) {
    transition(LeadershipState::fenced, LeadershipReason::process_epoch_conflict,
               now_ns);
    return false;
  }
  started_ = true;
  last_local_heartbeat_ns_ = now_ns;
  transition(LeadershipState::standby_synchronizing,
             LeadershipReason::process_epoch_claimed, now_ns);
  return true;
}

bool EdgeHaCoordinator::valid_health_time(const std::uint64_t now_ns) noexcept {
  if (now_ns == 0U || now_ns < health_.observed_process_monotonic_time_ns) {
    transition(LeadershipState::unsafe, LeadershipReason::delayed_local_heartbeat,
               now_ns == 0U ? last_local_heartbeat_ns_ : now_ns);
    return false;
  }
  return true;
}

bool EdgeHaCoordinator::heartbeat(const std::uint64_t now_ns) noexcept {
  if (!initialized_ || !started_ || stopped_ || now_ns == 0U ||
      now_ns < last_local_heartbeat_ns_) {
    return false;
  }
  if (last_local_heartbeat_ns_ != 0U &&
      now_ns - last_local_heartbeat_ns_ > configuration_.heartbeat_timeout_ns) {
    transition(LeadershipState::fenced, LeadershipReason::delayed_local_heartbeat,
               now_ns);
    return false;
  }
  const auto result = event_bus::ProcessEpochMechanism::heartbeat(
      process_epoch_state_, configuration_.process_id, configuration_.process_epoch,
      now_ns);
  if (result != event_bus::EpochHeartbeatStatus::recorded) {
    transition(LeadershipState::fenced, LeadershipReason::process_authority_lost,
               now_ns);
    return false;
  }
  last_local_heartbeat_ns_ = now_ns;
  publish_snapshot();
  return true;
}

bool EdgeHaCoordinator::grant_is_current(const std::uint64_t now_ns) const noexcept {
  return grant_.fencing_token != 0U && grant_.quorum_confirmed && now_ns != 0U &&
         now_ns >= grant_.issued_process_monotonic_time_ns &&
         now_ns <= grant_.valid_until_process_monotonic_time_ns;
}

GrantStatus EdgeHaCoordinator::apply_fencing_grant(
    const VerifiedFencingGrant& verified_grant, const std::uint64_t now_ns,
    BoundedRecoveryStream& recovery_stream) noexcept {
  if (stopped_) {
    return GrantStatus::stopped;
  }
  if (!initialized_ || !started_) {
    return GrantStatus::invalid;
  }
  if (!verified_grant.valid()) {
    return GrantStatus::invalid;
  }
  const auto& grant = verified_grant.value();
  if (state_ == LeadershipState::unsafe) {
    return GrantStatus::fenced;
  }
  const bool valid =
      grant.session_id == configuration_.session_id &&
      grant.node_id == configuration_.node_id &&
      grant.process_epoch == configuration_.process_epoch &&
      grant.exchange_session_epoch == configuration_.exchange_session_epoch &&
      grant.witness_key_id_sha256 == configuration_.witness_key_id_sha256 &&
      grant.witness_trust_root_sha256 == configuration_.witness_trust_root_sha256 &&
      grant.fencing_token != 0U && grant.grant_sequence != 0U &&
      grant.issued_process_monotonic_time_ns != 0U &&
      grant.issued_process_monotonic_time_ns <= now_ns &&
      now_ns <= grant.valid_until_process_monotonic_time_ns && grant.quorum_confirmed &&
      grant.stable_hash != 0U && grant.stable_hash == stable_fencing_grant_hash(grant);
  if (!valid) {
    return GrantStatus::invalid;
  }
  if (grant_.fencing_token != 0U && (grant.fencing_token < grant_.fencing_token ||
                                     grant.grant_sequence <= grant_.grant_sequence)) {
    return GrantStatus::stale;
  }
  if (grant.fencing_token == grant_.fencing_token) {
    if (grant.previous_fencing_token != grant_.previous_fencing_token) {
      return GrantStatus::invalid;
    }
    grant_ = grant;
    reason_ = LeadershipReason::fencing_grant_renewed;
    const auto recovery = recovery_stream.snapshot();
    publish_snapshot(&recovery);
    return GrantStatus::renewed;
  }
  const auto recovery_before_rotation = recovery_stream.snapshot();
  const auto expected_previous_token =
      grant_.fencing_token == 0U ? recovery_before_rotation.authority.fencing_token
                                 : grant_.fencing_token;
  if (!grant.previous_owner_fenced ||
      grant.previous_fencing_token != expected_previous_token) {
    return GrantStatus::fenced;
  }
  const auto rotate = recovery_stream.rotate_authority(
      {.session_id = configuration_.session_id,
       .exchange_session_epoch = configuration_.exchange_session_epoch,
       .fencing_token = grant.fencing_token});
  if (rotate != RecoveryRotateStatus::rotated &&
      rotate != RecoveryRotateStatus::unchanged) {
    return GrantStatus::recovery_stream_not_ready;
  }
  grant_ = grant;
  reconciliation_required_ = true;
  reconciled_risk_epoch_ = 0U;
  transition(configuration_.locality == DeploymentLocality::remote_region
                 ? LeadershipState::risk_reduction_only
                 : LeadershipState::leader_reconciling,
             configuration_.locality == DeploymentLocality::remote_region
                 ? LeadershipReason::remote_region_restricted
                 : LeadershipReason::fencing_grant_received,
             now_ns);
  return GrantStatus::accepted;
}

bool EdgeHaCoordinator::peer_claims_leadership(
    const DependencyHealth& health) const noexcept {
  if (!health.peer.observed || health.peer.node_id == configuration_.node_id ||
      !active_state(health.peer.state) ||
      health.observed_process_monotonic_time_ns <
          health.peer.heartbeat_process_monotonic_time_ns) {
    return health.peer.observed && active_state(health.peer.state) &&
           health.peer.node_id != configuration_.node_id;
  }
  return health.observed_process_monotonic_time_ns -
             health.peer.heartbeat_process_monotonic_time_ns <=
         configuration_.peer_timeout_ns;
}

bool EdgeHaCoordinator::essential_dependencies_ready() const noexcept {
  return health_.gateway_connected && health_.risk_ready && health_.journal_ready &&
         health_.shared_memory_current && !health_.partial_colocation_outage &&
         health_.gateway_session_epoch == configuration_.exchange_session_epoch &&
         health_.risk_service_epoch != 0U;
}

bool EdgeHaCoordinator::standby_available(
    const DependencyHealth& health,
    const RecoveryStreamSnapshot& recovery) const noexcept {
  if (!health.standby_heartbeat_expected) {
    return recovery.healthy && recovery.caught_up;
  }
  if (!health.peer.observed || health.peer.node_id == configuration_.node_id ||
      health.peer.state != LeadershipState::hot_standby ||
      health.observed_process_monotonic_time_ns <
          health.peer.heartbeat_process_monotonic_time_ns) {
    return false;
  }
  return health.observed_process_monotonic_time_ns -
                 health.peer.heartbeat_process_monotonic_time_ns <=
             configuration_.peer_timeout_ns &&
         recovery.healthy && recovery.caught_up;
}

// Ordered checks make identical health input produce identical fail-closed state.
// NOLINTBEGIN(readability-function-cognitive-complexity)
void EdgeHaCoordinator::update_health(const DependencyHealth& health,
                                      const RecoveryStreamSnapshot& recovery) noexcept {
  if (!initialized_ || !started_ || stopped_ ||
      !valid_health_time(health.observed_process_monotonic_time_ns)) {
    return;
  }
  health_ = health;
  recovery_published_sequence_ = recovery.published_sequence;
  recovery_applied_sequence_ = recovery.applied_sequence;
  recovery_healthy_ = recovery.healthy;
  recovery_caught_up_ = recovery.caught_up;
  if (state_ == LeadershipState::unsafe || state_ == LeadershipState::fenced) {
    publish_snapshot(&recovery);
    return;
  }
  const auto now_ns = health.observed_process_monotonic_time_ns;
  const auto process = event_bus::ProcessEpochMechanism::inspect(
      process_epoch_state_, now_ns, configuration_.heartbeat_timeout_ns);
  if (process.health != event_bus::EpochHealth::live ||
      process.process_id != configuration_.process_id ||
      process.epoch != configuration_.process_epoch) {
    transition(LeadershipState::fenced, LeadershipReason::process_authority_lost,
               now_ns);
    return;
  }
  if (!health.journal_ready) {
    transition(LeadershipState::unsafe, LeadershipReason::journal_unavailable, now_ns);
    return;
  }
  if (!health.shared_memory_current) {
    transition(LeadershipState::fenced, LeadershipReason::shared_memory_stale, now_ns);
    return;
  }
  if (health.partial_colocation_outage) {
    transition(LeadershipState::fenced, LeadershipReason::partial_colocation_outage,
               now_ns);
    return;
  }
  if (peer_claims_leadership(health)) {
    transition(LeadershipState::fenced, LeadershipReason::peer_claims_leadership,
               now_ns);
    return;
  }
  if (grant_.fencing_token == 0U) {
    transition(recovery.healthy && recovery.caught_up
                   ? LeadershipState::hot_standby
                   : LeadershipState::standby_synchronizing,
               recovery.healthy && recovery.caught_up
                   ? LeadershipReason::standby_ready
                   : LeadershipReason::standby_synchronizing,
               now_ns);
    return;
  }
  if (!recovery.healthy || recovery.stable_hash == 0U ||
      recovery.stable_hash != stable_recovery_snapshot_hash(recovery)) {
    transition(LeadershipState::unsafe, LeadershipReason::recovery_stream_unavailable,
               now_ns);
    return;
  }
  if (!grant_is_current(now_ns)) {
    transition(LeadershipState::fenced, LeadershipReason::fencing_lease_expired,
               now_ns);
    return;
  }
  if (configuration_.locality == DeploymentLocality::remote_region) {
    transition(LeadershipState::risk_reduction_only,
               LeadershipReason::remote_region_restricted, now_ns);
    return;
  }
  if (!health.gateway_connected ||
      health.gateway_session_epoch != configuration_.exchange_session_epoch) {
    reconciliation_required_ = true;
    transition(LeadershipState::leader_reconciling,
               LeadershipReason::gateway_disconnected, now_ns);
    return;
  }
  if (!health.risk_ready) {
    reconciliation_required_ = true;
    transition(LeadershipState::leader_reconciling, LeadershipReason::risk_unavailable,
               now_ns);
    return;
  }
  if (reconciled_risk_epoch_ != 0U &&
      health.risk_service_epoch != reconciled_risk_epoch_) {
    reconciliation_required_ = true;
    transition(LeadershipState::leader_reconciling,
               LeadershipReason::risk_epoch_changed, now_ns);
    return;
  }
  if (reconciliation_required_) {
    transition(LeadershipState::leader_reconciling,
               LeadershipReason::reconciliation_required, now_ns);
    return;
  }
  const bool standby_ready = standby_available(health, recovery);
  transition(standby_ready ? LeadershipState::active_leader
                           : LeadershipState::active_degraded,
             standby_ready ? LeadershipReason::reconciliation_completed
                           : LeadershipReason::standby_unavailable,
             now_ns);
}
// NOLINTEND(readability-function-cognitive-complexity)

ReconciliationStatus EdgeHaCoordinator::complete_reconciliation(
    ReconciliationEvidence evidence, const RecoveryStreamSnapshot& recovery) noexcept {
  if (stopped_) {
    return ReconciliationStatus::stopped;
  }
  if (!initialized_ || !started_ || evidence.stable_hash == 0U ||
      evidence.stable_hash != stable_reconciliation_evidence_hash(evidence) ||
      evidence.observed_process_monotonic_time_ns == 0U) {
    return ReconciliationStatus::invalid;
  }
  if (state_ != LeadershipState::leader_reconciling ||
      !grant_is_current(evidence.observed_process_monotonic_time_ns)) {
    return ReconciliationStatus::fenced;
  }
  if (!evidence.oms_reconciled || !evidence.gateway_reconciled ||
      !evidence.positions_reconciled || !evidence.risk_reconciled ||
      !evidence.journal_verified) {
    return ReconciliationStatus::incomplete;
  }
  const bool consistent =
      evidence.fencing_token == grant_.fencing_token &&
      evidence.exchange_session_epoch == configuration_.exchange_session_epoch &&
      evidence.risk_service_epoch != 0U && essential_dependencies_ready() &&
      recovery.healthy && recovery.caught_up && recovery.stable_hash != 0U &&
      recovery.stable_hash == stable_recovery_snapshot_hash(recovery) &&
      recovery.authority.session_id == configuration_.session_id &&
      recovery.authority.exchange_session_epoch ==
          configuration_.exchange_session_epoch &&
      recovery.authority.fencing_token == grant_.fencing_token &&
      evidence.source_journal_sequence == recovery.latest_source_journal_sequence &&
      evidence.source_journal_hash == recovery.latest_source_journal_hash &&
      !peer_claims_leadership(health_);
  if (!consistent) {
    return ReconciliationStatus::inconsistent;
  }
  reconciled_risk_epoch_ = evidence.risk_service_epoch;
  reconciliation_required_ = false;
  recovery_published_sequence_ = recovery.published_sequence;
  recovery_applied_sequence_ = recovery.applied_sequence;
  recovery_healthy_ = recovery.healthy;
  recovery_caught_up_ = recovery.caught_up;
  transition(standby_available(health_, recovery) ? LeadershipState::active_leader
                                                  : LeadershipState::active_degraded,
             standby_available(health_, recovery)
                 ? LeadershipReason::reconciliation_completed
                 : LeadershipReason::standby_unavailable,
             evidence.observed_process_monotonic_time_ns);
  return ReconciliationStatus::completed;
}

EmissionDecision
EdgeHaCoordinator::authorize_emission(EmissionRequest request,
                                      BoundedRecoveryStream& recovery_stream) noexcept {
  EmissionDecision decision{};
  if (stopped_) {
    decision.status = EmissionStatus::stopped;
  } else if (!initialized_ || !started_ || request.stable_hash == 0U ||
             request.stable_hash != stable_emission_request_hash(request) ||
             !request.command_id.valid() || request.command_hash == 0U ||
             request.source_journal_sequence == 0U ||
             request.source_journal_hash == 0U ||
             request.process_monotonic_time_ns == 0U) {
    decision.status = EmissionStatus::invalid;
  } else if (!active_state(state_) || reconciliation_required_ ||
             configuration_.locality != DeploymentLocality::colocated_edge) {
    decision.status = EmissionStatus::not_leader;
    decision.reason = reason_;
  } else if (!grant_is_current(request.process_monotonic_time_ns) ||
             request.session_id != configuration_.session_id ||
             request.exchange_session_epoch != configuration_.exchange_session_epoch ||
             request.fencing_token != grant_.fencing_token ||
             request.process_epoch != configuration_.process_epoch) {
    decision.status = EmissionStatus::stale_authority;
    decision.reason = LeadershipReason::process_authority_lost;
  } else if (!essential_dependencies_ready()) {
    decision.status = EmissionStatus::unsafe;
    decision.reason = reason_;
  } else {
    RecoveryPayload payload{
        .kind = RecoveryRecordKind::order_emission,
        .command_id = request.command_id,
        .command_hash = request.command_hash,
        .source_journal_sequence = request.source_journal_sequence,
        .source_journal_hash = request.source_journal_hash,
        .process_monotonic_time_ns = request.process_monotonic_time_ns,
        .authority = {.session_id = request.session_id,
                      .exchange_session_epoch = request.exchange_session_epoch,
                      .fencing_token = request.fencing_token}};
    payload.stable_hash = stable_recovery_payload_hash(payload);
    const auto appended = recovery_stream.append(payload);
    decision.recovery_stream_sequence = appended.stream_sequence;
    decision.recovery_record_hash = appended.record_hash;
    if (appended.status == RecoveryAppendStatus::accepted) {
      decision.status = EmissionStatus::permitted;
      const auto recovery = recovery_stream.snapshot();
      publish_snapshot(&recovery);
    } else if (appended.status == RecoveryAppendStatus::duplicate) {
      decision.status = EmissionStatus::duplicate_suppressed;
      decision.reason = LeadershipReason::duplicate_order_suppressed;
    } else {
      decision.status = appended.status == RecoveryAppendStatus::fenced
                            ? EmissionStatus::stale_authority
                            : EmissionStatus::recovery_stream_unavailable;
      decision.reason = appended.status == RecoveryAppendStatus::identity_conflict
                            ? LeadershipReason::command_identity_conflict
                            : LeadershipReason::recovery_stream_unavailable;
      transition(LeadershipState::unsafe, decision.reason,
                 request.process_monotonic_time_ns);
    }
  }
  decision.stable_hash = stable_emission_decision_hash(decision);
  return decision;
}

void EdgeHaCoordinator::operator_fence(const std::uint64_t now_ns) noexcept {
  if (!stopped_) {
    reconciliation_required_ = true;
    transition(LeadershipState::fenced, LeadershipReason::operator_fenced, now_ns);
  }
}

void EdgeHaCoordinator::shutdown(const std::uint64_t now_ns) noexcept {
  if (stopped_) {
    return;
  }
  (void)event_bus::ProcessEpochMechanism::mark_stopped(
      process_epoch_state_, configuration_.process_id, configuration_.process_epoch);
  reconciliation_required_ = true;
  stopped_ = true;
  transition(LeadershipState::stopped, LeadershipReason::shutdown, now_ns);
}

LeadershipSnapshot EdgeHaCoordinator::snapshot() const noexcept {
  const bool active = active_state(state_) && !reconciliation_required_ &&
                      configuration_.locality == DeploymentLocality::colocated_edge &&
                      essential_dependencies_ready() && recovery_healthy_ &&
                      grant_is_current(health_.observed_process_monotonic_time_ns);
  const bool peer_ready = health_.peer.observed &&
                          health_.peer.state == LeadershipState::hot_standby &&
                          recovery_healthy_ && recovery_caught_up_;
  LeadershipSnapshot result{
      .session_id = configuration_.session_id,
      .configuration_version = configuration_.configuration_version,
      .state = state_,
      .reason = reason_,
      .locality = configuration_.locality,
      .node_id = configuration_.node_id,
      .process_id = configuration_.process_id,
      .process_epoch = configuration_.process_epoch,
      .exchange_session_epoch = configuration_.exchange_session_epoch,
      .fencing_token = grant_.fencing_token,
      .grant_sequence = grant_.grant_sequence,
      .grant_valid_until_ns = grant_.valid_until_process_monotonic_time_ns,
      .last_local_heartbeat_ns = last_local_heartbeat_ns_,
      .risk_service_epoch = health_.risk_service_epoch,
      .recovery_published_sequence = recovery_published_sequence_,
      .recovery_applied_sequence = recovery_applied_sequence_,
      .transition_sequence = transition_count_,
      .configuration_hash = configuration_.stable_hash,
      .can_emit_orders = active,
      .reconciliation_required = reconciliation_required_,
      .standby_available = peer_ready,
      .operator_attention_required = state_ == LeadershipState::active_degraded ||
                                     state_ == LeadershipState::leader_reconciling ||
                                     state_ == LeadershipState::fenced ||
                                     state_ == LeadershipState::unsafe};
  result.stable_hash = stable_leadership_snapshot_hash(result);
  return result;
}

event_bus::SnapshotReadStatus EdgeHaCoordinator::acquire_snapshot(
    const std::uint64_t acquired_at_ns,
    LeadershipSnapshotStore::ReadHandle& output) noexcept {
  return snapshot_store_.acquire(acquired_at_ns, output);
}

std::size_t EdgeHaCoordinator::transition_count() const noexcept {
  return transition_count_;
}

const HaTransition*
EdgeHaCoordinator::transition_at(const std::size_t index) const noexcept {
  return index < transition_count_ ? &transitions_[index] : nullptr;
}

const common::BuildInfo& EdgeHaCoordinator::build_info() noexcept {
  return common::current_build_info();
}

void EdgeHaCoordinator::transition(const LeadershipState next,
                                   const LeadershipReason reason,
                                   const std::uint64_t now_ns) noexcept {
  if (state_ == next && reason_ == reason) {
    publish_snapshot();
    return;
  }
  const auto previous = state_;
  state_ = next;
  reason_ = reason;
  if (next == LeadershipState::fenced || next == LeadershipState::unsafe) {
    reconciliation_required_ = true;
  }
  if (transition_count_ >= transitions_.size()) {
    state_ = LeadershipState::unsafe;
    reason_ = LeadershipReason::journal_unavailable;
    reconciliation_required_ = true;
    publish_snapshot();
    return;
  }
  HaTransition record{.sequence = transition_count_ + 1U,
                      .previous_state = previous,
                      .current_state = state_,
                      .reason = reason_,
                      .fencing_token = grant_.fencing_token,
                      .process_epoch = configuration_.process_epoch,
                      .process_monotonic_time_ns = now_ns,
                      .previous_record_hash = transition_hash_};
  record.record_hash = stable_ha_transition_hash(record);
  transitions_[transition_count_] = record;
  ++transition_count_;
  transition_hash_ = record.record_hash;
  publish_snapshot();
}

void EdgeHaCoordinator::publish_snapshot(
    const RecoveryStreamSnapshot* recovery) noexcept {
  if (recovery != nullptr) {
    recovery_published_sequence_ = recovery->published_sequence;
    recovery_applied_sequence_ = recovery->applied_sequence;
    recovery_healthy_ = recovery->healthy;
    recovery_caught_up_ = recovery->caught_up;
  }
  const auto value = snapshot();
  const auto status = snapshot_store_.publish(value, last_local_heartbeat_ns_);
  if (status != event_bus::SnapshotPublishStatus::published && initialized_) {
    state_ = LeadershipState::unsafe;
    reason_ = LeadershipReason::shared_memory_stale;
    reconciliation_required_ = true;
  }
}

} // namespace aegis::high_availability
