#ifndef AEGIS_HIGH_AVAILABILITY_COORDINATOR_HPP
#define AEGIS_HIGH_AVAILABILITY_COORDINATOR_HPP

#include "aegis/common/authentication.hpp"
#include "aegis/common/build_info.hpp"
#include "aegis/common/identifiers.hpp"
#include "aegis/event_bus/process_epoch.hpp"
#include "aegis/event_bus/snapshot_store.hpp"
#include "aegis/high_availability/recovery_stream.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace aegis::high_availability {

inline constexpr std::size_t kMaximumHaTransitions = 1'024U;

enum class DeploymentLocality : std::uint8_t {
  colocated_edge = 1,
  remote_region = 2,
};

enum class LeadershipState : std::uint8_t {
  starting = 1,
  standby_synchronizing = 2,
  hot_standby = 3,
  leader_reconciling = 4,
  active_leader = 5,
  active_degraded = 6,
  risk_reduction_only = 7,
  fenced = 8,
  unsafe = 9,
  stopped = 10,
};

enum class LeadershipReason : std::uint8_t {
  none = 0,
  process_epoch_claimed = 1,
  process_epoch_conflict = 2,
  delayed_local_heartbeat = 3,
  process_authority_lost = 4,
  standby_synchronizing = 5,
  standby_ready = 6,
  fencing_grant_received = 7,
  fencing_grant_renewed = 8,
  invalid_fencing_grant = 9,
  fencing_lease_expired = 10,
  reconciliation_required = 11,
  reconciliation_completed = 12,
  gateway_disconnected = 13,
  risk_unavailable = 14,
  risk_epoch_changed = 15,
  journal_unavailable = 16,
  shared_memory_stale = 17,
  recovery_stream_unavailable = 18,
  standby_unavailable = 19,
  peer_claims_leadership = 20,
  partial_colocation_outage = 21,
  remote_region_restricted = 22,
  duplicate_order_suppressed = 23,
  command_identity_conflict = 24,
  operator_fenced = 25,
  shutdown = 26,
};

enum class GrantStatus : std::uint8_t {
  accepted = 1,
  renewed = 2,
  invalid = 3,
  stale = 4,
  recovery_stream_not_ready = 5,
  fenced = 6,
  stopped = 7,
};

enum class ReconciliationStatus : std::uint8_t {
  completed = 1,
  incomplete = 2,
  inconsistent = 3,
  invalid = 4,
  fenced = 5,
  stopped = 6,
};

enum class EmissionStatus : std::uint8_t {
  permitted = 1,
  duplicate_suppressed = 2,
  not_leader = 3,
  stale_authority = 4,
  recovery_stream_unavailable = 5,
  invalid = 6,
  unsafe = 7,
  stopped = 8,
};

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct HaConfiguration {
  common::SessionId session_id;
  common::ConfigurationVersion configuration_version;
  std::uint64_t node_id{};
  std::uint64_t process_id{};
  std::uint64_t process_epoch{};
  std::uint64_t exchange_session_epoch{};
  std::uint64_t heartbeat_timeout_ns{};
  std::uint64_t peer_timeout_ns{};
  DeploymentLocality locality{DeploymentLocality::colocated_edge};
  common::Ed25519PublicKey witness_public_key{};
  common::Sha256Digest witness_key_id_sha256{};
  common::Sha256Digest witness_trust_root_sha256{};
  std::uint64_t stable_hash{};
};

struct FencingGrant {
  common::SessionId session_id;
  std::uint64_t node_id{};
  std::uint64_t process_epoch{};
  std::uint64_t exchange_session_epoch{};
  std::uint64_t fencing_token{};
  std::uint64_t previous_fencing_token{};
  std::uint64_t grant_sequence{};
  std::uint64_t issued_process_monotonic_time_ns{};
  std::uint64_t valid_until_process_monotonic_time_ns{};
  bool quorum_confirmed{false};
  bool previous_owner_fenced{false};
  common::Sha256Digest witness_key_id_sha256{};
  common::Sha256Digest witness_trust_root_sha256{};
  common::Ed25519Signature witness_signature{};
  std::uint64_t stable_hash{};
};

struct PeerObservation {
  std::uint64_t node_id{};
  std::uint64_t process_epoch{};
  std::uint64_t fencing_token{};
  std::uint64_t heartbeat_process_monotonic_time_ns{};
  LeadershipState state{LeadershipState::starting};
  bool observed{false};
};

struct DependencyHealth {
  std::uint64_t observed_process_monotonic_time_ns{};
  std::uint64_t gateway_session_epoch{};
  std::uint64_t risk_service_epoch{};
  bool gateway_connected{false};
  bool risk_ready{false};
  bool journal_ready{false};
  bool shared_memory_current{false};
  bool partial_colocation_outage{false};
  bool standby_heartbeat_expected{true};
  PeerObservation peer;
};

struct ReconciliationEvidence {
  std::uint64_t fencing_token{};
  std::uint64_t exchange_session_epoch{};
  std::uint64_t risk_service_epoch{};
  std::uint64_t source_journal_sequence{};
  std::uint64_t source_journal_hash{};
  std::uint64_t observed_process_monotonic_time_ns{};
  bool oms_reconciled{false};
  bool gateway_reconciled{false};
  bool positions_reconciled{false};
  bool risk_reconciled{false};
  bool journal_verified{false};
  std::uint64_t stable_hash{};
};

struct EmissionRequest {
  common::SessionId session_id;
  common::GlobalEventId command_id;
  std::uint64_t command_hash{};
  std::uint64_t exchange_session_epoch{};
  std::uint64_t fencing_token{};
  std::uint64_t process_epoch{};
  std::uint64_t source_journal_sequence{};
  std::uint64_t source_journal_hash{};
  std::uint64_t process_monotonic_time_ns{};
  std::uint64_t stable_hash{};
};

struct EmissionDecision {
  EmissionStatus status{EmissionStatus::invalid};
  LeadershipReason reason{LeadershipReason::none};
  std::uint64_t recovery_stream_sequence{};
  std::uint64_t recovery_record_hash{};
  std::uint64_t stable_hash{};
};

struct LeadershipSnapshot {
  common::SessionId session_id;
  common::ConfigurationVersion configuration_version;
  LeadershipState state{LeadershipState::starting};
  LeadershipReason reason{LeadershipReason::none};
  DeploymentLocality locality{DeploymentLocality::colocated_edge};
  std::uint64_t node_id{};
  std::uint64_t process_id{};
  std::uint64_t process_epoch{};
  std::uint64_t exchange_session_epoch{};
  std::uint64_t fencing_token{};
  std::uint64_t grant_sequence{};
  std::uint64_t grant_valid_until_ns{};
  std::uint64_t last_local_heartbeat_ns{};
  std::uint64_t risk_service_epoch{};
  std::uint64_t recovery_published_sequence{};
  std::uint64_t recovery_applied_sequence{};
  std::uint64_t transition_sequence{};
  std::uint64_t configuration_hash{};
  bool can_emit_orders{false};
  bool reconciliation_required{true};
  bool standby_available{false};
  bool operator_attention_required{false};
  std::uint64_t stable_hash{};
};

struct HaTransition {
  std::uint64_t sequence{};
  LeadershipState previous_state{LeadershipState::starting};
  LeadershipState current_state{LeadershipState::starting};
  LeadershipReason reason{LeadershipReason::none};
  std::uint64_t fencing_token{};
  std::uint64_t process_epoch{};
  std::uint64_t process_monotonic_time_ns{};
  std::uint64_t previous_record_hash{};
  std::uint64_t record_hash{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] std::uint64_t
stable_ha_configuration_hash(HaConfiguration value) noexcept;
[[nodiscard]] std::uint64_t stable_fencing_grant_hash(FencingGrant value) noexcept;
[[nodiscard]] common::Sha256Digest
fencing_grant_signing_digest(const FencingGrant& value) noexcept;
[[nodiscard]] bool
sign_fencing_grant(FencingGrant& value,
                   const common::Ed25519PrivateKey& private_key) noexcept;
[[nodiscard]] std::uint64_t
stable_reconciliation_evidence_hash(ReconciliationEvidence value) noexcept;
[[nodiscard]] std::uint64_t
stable_emission_request_hash(EmissionRequest value) noexcept;
[[nodiscard]] std::uint64_t
stable_emission_decision_hash(EmissionDecision value) noexcept;
[[nodiscard]] std::uint64_t
stable_leadership_snapshot_hash(LeadershipSnapshot value) noexcept;
[[nodiscard]] std::uint64_t stable_ha_transition_hash(HaTransition value) noexcept;
[[nodiscard]] const char* to_string(LeadershipState value) noexcept;
[[nodiscard]] const char* to_string(LeadershipReason value) noexcept;

using LeadershipSnapshotStore =
    event_bus::ImmutableSnapshotStore<LeadershipSnapshot, 3U>;

class VerifiedFencingGrant final {
public:
  VerifiedFencingGrant() noexcept = default;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] const FencingGrant& value() const noexcept;

private:
  friend class FencingGrantVerifier;
  FencingGrant grant_{};
  bool verified_{false};
};

struct FencingGrantTrust {
  common::Ed25519PublicKey witness_public_key{};
  common::Sha256Digest witness_key_id_sha256{};
  common::Sha256Digest witness_trust_root_sha256{};
};

// Verification is an infrequent control-path operation. Safe C++ callers
// cannot construct authoritative leadership from caller-controlled fields.
class FencingGrantVerifier final {
public:
  explicit FencingGrantVerifier(FencingGrantTrust trust) noexcept;

  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] bool verify(const FencingGrant& signed_grant, std::uint64_t now_ns,
                            VerifiedFencingGrant& output) const noexcept;

private:
  common::Ed25519PublicKey witness_public_key_{};
  common::Sha256Digest witness_key_id_sha256_{};
  common::Sha256Digest witness_trust_root_sha256_{};
  bool initialized_{false};
};

class EdgeHaCoordinator final {
public:
  EdgeHaCoordinator(HaConfiguration configuration,
                    event_bus::ProcessEpochState& process_epoch_state) noexcept;

  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] bool start(std::uint64_t now_ns) noexcept;
  [[nodiscard]] bool heartbeat(std::uint64_t now_ns) noexcept;
  [[nodiscard]] GrantStatus
  apply_fencing_grant(const VerifiedFencingGrant& verified_grant, std::uint64_t now_ns,
                      BoundedRecoveryStream& recovery_stream) noexcept;
  void update_health(const DependencyHealth& health,
                     const RecoveryStreamSnapshot& recovery) noexcept;
  [[nodiscard]] ReconciliationStatus
  complete_reconciliation(ReconciliationEvidence evidence,
                          const RecoveryStreamSnapshot& recovery) noexcept;
  [[nodiscard]] EmissionDecision
  authorize_emission(EmissionRequest request,
                     BoundedRecoveryStream& recovery_stream) noexcept;
  void operator_fence(std::uint64_t now_ns) noexcept;
  void shutdown(std::uint64_t now_ns) noexcept;

  [[nodiscard]] LeadershipSnapshot snapshot() const noexcept;
  [[nodiscard]] event_bus::SnapshotReadStatus
  acquire_snapshot(std::uint64_t acquired_at_ns,
                   LeadershipSnapshotStore::ReadHandle& output) noexcept;
  [[nodiscard]] std::size_t transition_count() const noexcept;
  [[nodiscard]] const HaTransition* transition_at(std::size_t index) const noexcept;
  [[nodiscard]] static const common::BuildInfo& build_info() noexcept;

private:
  [[nodiscard]] bool valid_health_time(std::uint64_t now_ns) noexcept;
  [[nodiscard]] bool grant_is_current(std::uint64_t now_ns) const noexcept;
  [[nodiscard]] bool
  peer_claims_leadership(const DependencyHealth& health) const noexcept;
  [[nodiscard]] bool essential_dependencies_ready() const noexcept;
  [[nodiscard]] bool
  standby_available(const DependencyHealth& health,
                    const RecoveryStreamSnapshot& recovery) const noexcept;
  void transition(LeadershipState next, LeadershipReason reason,
                  std::uint64_t now_ns) noexcept;
  void publish_snapshot(const RecoveryStreamSnapshot* recovery = nullptr) noexcept;

  HaConfiguration configuration_;
  event_bus::ProcessEpochState& process_epoch_state_;
  LeadershipSnapshotStore snapshot_store_;
  std::array<HaTransition, kMaximumHaTransitions> transitions_{};
  FencingGrant grant_{};
  DependencyHealth health_{};
  LeadershipState state_{LeadershipState::starting};
  LeadershipReason reason_{LeadershipReason::none};
  std::size_t transition_count_{};
  std::uint64_t transition_hash_{};
  std::uint64_t last_local_heartbeat_ns_{};
  std::uint64_t reconciled_risk_epoch_{};
  std::uint64_t recovery_published_sequence_{};
  std::uint64_t recovery_applied_sequence_{};
  bool recovery_healthy_{false};
  bool recovery_caught_up_{false};
  bool initialized_{false};
  bool started_{false};
  bool reconciliation_required_{true};
  bool stopped_{false};
};

static_assert(std::is_trivially_copyable_v<LeadershipSnapshot>);
static_assert(std::is_trivially_copyable_v<HaTransition>);

} // namespace aegis::high_availability

#endif // AEGIS_HIGH_AVAILABILITY_COORDINATOR_HPP
