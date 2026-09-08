#ifndef AEGIS_INTEGRATION_PAPER_ACCEPTANCE_HPP
#define AEGIS_INTEGRATION_PAPER_ACCEPTANCE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace aegis::integration {

inline constexpr std::uint16_t kPaperAcceptanceSchemaMajor = 1U;
inline constexpr std::uint16_t kPaperAcceptanceSchemaMinor = 0U;
inline constexpr std::size_t kPaperScenarioCount = 16U;
inline constexpr std::uint64_t kDefaultPaperAcceptanceSeed = 20'260'906U;

enum class PaperScenario : std::uint8_t {
  ordinary_midday = 1,
  market_open = 2,
  market_close = 3,
  earnings_release = 4,
  cpi_release = 5,
  breaking_negative_news = 6,
  false_rumor_correction = 7,
  index_rebalance = 8,
  hidden_liquidity_replenishment = 9,
  trading_halt_reopening = 10,
  feed_gap = 11,
  clock_degradation = 12,
  model_timeout = 13,
  risk_service_restart = 14,
  gateway_disconnect = 15,
  split_brain_attempt = 16,
};

// Fixed-layout results keep scenario comparisons deterministic. Report rendering
// and filesystem activity happen only after every edge component is quiescent.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct ComponentHashes {
  std::uint64_t source_events{};
  std::uint64_t feature_snapshot{};
  std::uint64_t specialist_forecasts{};
  std::uint64_t market_state{};
  std::uint64_t ensemble{};
  std::uint64_t risk{};
  std::uint64_t oms{};
  std::uint64_t gateway{};
  std::uint64_t portfolio{};
  std::uint64_t audit{};
};

struct ScenarioCounters {
  std::uint64_t source_events{};
  std::uint64_t normalized_events{};
  std::uint64_t specialist_forecasts{};
  std::uint64_t late_forecasts_discarded{};
  std::uint64_t eligible_experts{};
  std::uint64_t ensemble_abstentions{};
  std::uint64_t route_decisions{};
  std::uint64_t risk_evaluations{};
  std::uint64_t risk_approvals{};
  std::uint64_t risk_rejections{};
  std::uint64_t oms_orders{};
  std::uint64_t gateway_commands{};
  std::uint64_t gateway_accepted{};
  std::uint64_t fills{};
  std::uint64_t explained_orders{};
  std::uint64_t telemetry_drops{};
  std::uint64_t telemetry_final_queue_occupancy{};
  std::uint64_t telemetry_maximum_queue_occupancy{};
  std::uint64_t final_absolute_position_units{};
  std::int64_t final_pnl_currency_nanos{};
};

struct ScenarioChecks {
  bool paper_mode_only{false};
  bool pipeline_complete{false};
  bool no_risk_bypass{false};
  bool expected_restriction_observed{false};
  bool audit_complete{false};
  bool restart_reconstructed{false};
  bool correction_retained{false};
  bool replay_hash_match{false};
};

struct ScenarioResult {
  PaperScenario scenario{PaperScenario::ordinary_midday};
  std::uint64_t seed{};
  ScenarioCounters counters;
  ScenarioChecks checks;
  ComponentHashes hashes;
  std::uint64_t outcome_hash{};
  std::uint64_t replay_outcome_hash{};
  bool passed{false};
};

struct AcceptanceChecks {
  bool no_order_bypasses_risk{false};
  bool halt_blocks_new_orders{false};
  bool invalid_data_causes_restriction{false};
  bool late_forecasts_are_discarded{false};
  bool high_disagreement_reduces_exposure{false};
  bool all_invalid_models_abstain{false};
  bool restart_reconstructs_orders_and_positions{false};
  bool audit_trace_explains_every_order{false};
  bool deterministic_replay_matches_hashes{false};
  bool paper_mode_only{false};
};

struct PaperAcceptanceReport {
  std::uint16_t schema_major{kPaperAcceptanceSchemaMajor};
  std::uint16_t schema_minor{kPaperAcceptanceSchemaMinor};
  std::uint64_t seed{kDefaultPaperAcceptanceSeed};
  std::array<ScenarioResult, kPaperScenarioCount> scenarios{};
  AcceptanceChecks acceptance;
  std::uint64_t report_hash{};
  bool passed{false};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] std::string_view scenario_name(PaperScenario scenario) noexcept;
[[nodiscard]] PaperAcceptanceReport
run_paper_acceptance(std::uint64_t seed = kDefaultPaperAcceptanceSeed);
[[nodiscard]] bool write_machine_report(const PaperAcceptanceReport& report,
                                        const std::filesystem::path& path);
[[nodiscard]] bool write_human_report(const PaperAcceptanceReport& report,
                                      const std::filesystem::path& path);

} // namespace aegis::integration

#endif // AEGIS_INTEGRATION_PAPER_ACCEPTANCE_HPP
