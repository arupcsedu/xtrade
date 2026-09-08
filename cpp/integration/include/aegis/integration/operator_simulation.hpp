#ifndef AEGIS_INTEGRATION_OPERATOR_SIMULATION_HPP
#define AEGIS_INTEGRATION_OPERATOR_SIMULATION_HPP

#include "aegis/common/sha256.hpp"
#include "aegis/risk/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace aegis::integration {

inline constexpr std::uint64_t kDefaultOperatorSimulationSeed = 20'260'908U;
inline constexpr std::size_t kOperatorSimulationScopeCount = 4U;
inline constexpr std::size_t kOperatorSimulationRiskDecisionCount = 9U;

struct OperatorSimulationScopeResult final {
  risk::KillSwitchScope scope{risk::KillSwitchScope::firm};
  std::uint64_t engage_command_sequence{};
  std::uint64_t clear_command_sequence{};
  risk::StateUpdateStatus engage_status{risk::StateUpdateStatus::invalid};
  risk::StateUpdateStatus unauthorized_clear_status{risk::StateUpdateStatus::invalid};
  risk::StateUpdateStatus authorized_clear_status{risk::StateUpdateStatus::invalid};
  risk::RiskDecision blocked_decision;
  risk::RiskDecision recovered_decision;
  bool passed{false};
};

struct OperatorAuditRecord final {
  std::uint64_t sequence{};
  std::string previous_sha256;
  std::string event;
  std::string scope;
  std::uint64_t command_sequence{};
  bool operator_authorized{false};
  std::string update_status;
  std::uint64_t risk_journal_sequence{};
  std::uint64_t risk_decision_hash{};
  std::string record_sha256;
};

struct OperatorSimulationReport final {
  std::uint64_t seed{};
  risk::RiskDecision baseline_decision;
  std::array<OperatorSimulationScopeResult, kOperatorSimulationScopeCount> scopes{};
  std::vector<OperatorAuditRecord> audit_records;
  common::Sha256Digest audit_extract_sha256{};
  std::string audit_chain_final_sha256;
  std::uint64_t extracted_risk_decisions{};
  std::uint64_t risk_journal_records_remaining{};
  bool paper_mode_only{false};
  bool production_activation_attempted{false};
  bool baseline_approved{false};
  bool every_kill_blocked{false};
  bool unauthorized_clear_rejected{false};
  bool authorized_recovery_succeeded{false};
  bool decision_journal_complete{false};
  bool audit_chain_valid{false};
  bool passed{false};
};

struct OperatorSimulationOutputPaths final {
  std::filesystem::path audit_extract;
  std::filesystem::path machine_report;
};

[[nodiscard]] OperatorSimulationReport
run_operator_simulation(std::uint64_t seed = kDefaultOperatorSimulationSeed);

[[nodiscard]] bool write_operator_audit_extract(const OperatorSimulationReport& report,
                                                const std::filesystem::path& path);

[[nodiscard]] bool
write_operator_simulation_report(const OperatorSimulationReport& report,
                                 const OperatorSimulationOutputPaths& paths);

[[nodiscard]] const char* operator_scope_name(risk::KillSwitchScope scope) noexcept;

} // namespace aegis::integration

#endif // AEGIS_INTEGRATION_OPERATOR_SIMULATION_HPP
