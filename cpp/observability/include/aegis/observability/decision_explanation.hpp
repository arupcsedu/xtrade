#ifndef AEGIS_OBSERVABILITY_DECISION_EXPLANATION_HPP
#define AEGIS_OBSERVABILITY_DECISION_EXPLANATION_HPP

#include "aegis/ensemble/types.hpp"
#include "aegis/event_bus/mpsc_queue.hpp"
#include "aegis/execution/router.hpp"
#include "aegis/journal/async_journal.hpp"
#include "aegis/risk/types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>

namespace aegis::observability {

inline constexpr std::size_t kDecisionExplanationQueueCapacity = 256U;
inline constexpr std::size_t kMaximumDecisionExplanationBinaryBytes =
    std::size_t{4U} * 1024U;

enum class DecisionDisposition : std::uint8_t {
  abstained = 1,
  awaiting_risk = 2,
  risk_rejected = 3,
  awaiting_route = 4,
  routed = 5,
};

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct ExpertDecisionRecord {
  common::ForecastId forecast_id;
  common::ModelId model_id;
  common::ModelVersion model_version;
  common::FeatureSnapshotId feature_snapshot_id;
  ensemble::ExpertRole role{ensemble::ExpertRole::generic};
  ensemble::EligibilityReason eligibility{
      ensemble::EligibilityReason::invalid_forecast};
  models::ModelHealthState health{models::ModelHealthState::unknown};
  ensemble::CalibrationHealth calibration_health{ensemble::CalibrationHealth::unknown};
  std::uint32_t weight_ppm{};
  std::uint32_t freshness_ppm{};
  std::int64_t expected_return_ppm{};
  std::int64_t return_p10_ppm{};
  std::int64_t return_p50_ppm{};
  std::int64_t return_p90_ppm{};
  std::uint32_t probability_down_ppm{};
  std::uint32_t probability_flat_ppm{};
  std::uint32_t probability_up_ppm{};
  std::uint64_t volatility_ppm{};
  std::uint32_t confidence_ppm{};
  std::uint32_t calibration_score_ppm{};
  std::uint32_t data_quality_score_ppm{};
  std::uint32_t ood_score_ppm{};
  std::uint64_t production_process_monotonic_time_ns{};
  std::uint64_t expiration_process_monotonic_time_ns{};
  std::uint64_t forecast_hash{};
};

// Semantic audit grouping is preferred over saving 56 bytes in this off-path
// record; the bounded queue capacity already accounts for sizeof(record).
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct DecisionExplanationRecord {
  std::uint16_t schema_major{1U};
  std::uint16_t schema_minor{0U};
  common::GlobalEventId explanation_id;
  common::GlobalEventId correlation_id;
  common::SessionId session_id;
  common::InstrumentId instrument_id;
  common::ConfigurationVersion configuration_version;
  common::ForecastId ensemble_forecast_id;
  std::uint64_t created_process_monotonic_time_ns{};
  std::int64_t created_wall_clock_utc_time_ns{};
  market_state::MarketState market_state{market_state::MarketState::startup};
  ensemble::EventState event_state{ensemble::EventState::none};
  std::uint64_t market_state_snapshot_hash{};
  std::uint32_t input_data_quality_ppm{};
  std::uint32_t expert_count{};
  std::uint32_t eligible_expert_count{};
  std::array<ExpertDecisionRecord, ensemble::kMaximumExperts> experts{};
  std::int64_t combined_expected_return_ppm{};
  std::uint32_t combined_probability_down_ppm{};
  std::uint32_t combined_probability_flat_ppm{};
  std::uint32_t combined_probability_up_ppm{};
  std::uint64_t combined_variance_ppm_squared{};
  std::int64_t combined_return_p10_ppm{};
  std::int64_t combined_return_p50_ppm{};
  std::int64_t combined_return_p90_ppm{};
  std::uint64_t effective_uncertainty_ppm{};
  std::uint64_t disagreement_ppm{};
  std::uint64_t estimated_transaction_cost_ppm{};
  std::uint64_t uncertainty_penalty_ppm{};
  std::uint64_t safety_margin_ppm{};
  std::int64_t net_robust_edge_ppm{};
  bool abstain{true};
  ensemble::DecisionReason ensemble_reason{ensemble::DecisionReason::invalid_request};
  std::uint64_t ensemble_reason_mask{};
  std::uint64_t ensemble_hash{};
  std::uint64_t transaction_cost_forecast_hash{};
  bool risk_present{false};
  common::IntentId risk_intent_id;
  risk::DecisionCode risk_decision{risk::DecisionCode::rejected};
  risk::RiskReason risk_reason{risk::RiskReason::risk_state_unavailable};
  risk::RiskCheck failed_risk_check{risk::RiskCheck::none};
  common::RiskSnapshotId risk_snapshot_id;
  std::uint64_t approved_quantity_units{};
  std::int64_t approved_price_ticks{};
  std::uint64_t risk_snapshot_hash{};
  std::uint64_t risk_context_hash{};
  std::uint64_t risk_decision_hash{};
  bool routing_present{false};
  execution::RoutingStatus routing_status{execution::RoutingStatus::invalid_request};
  execution::RoutingReason routing_reason{execution::RoutingReason::invalid_request};
  common::VenueId selected_venue_id;
  execution::RoutedAction routed_action{execution::RoutedAction::new_order};
  execution::RoutedTimeInForce time_in_force{execution::RoutedTimeInForce::day};
  std::int64_t routed_price_ticks{};
  std::uint64_t routed_quantity_units{};
  std::int64_t routed_expected_value_currency_nanos_per_unit{};
  std::uint16_t evaluated_venue_count{};
  std::uint16_t eligible_venue_count{};
  std::uint64_t routing_decision_hash{};
  DecisionDisposition disposition{DecisionDisposition::abstained};
  std::uint64_t stable_hash{};
};

struct DecisionExplanationInput {
  common::GlobalEventId explanation_id;
  common::GlobalEventId correlation_id;
  std::uint64_t created_process_monotonic_time_ns{};
  std::int64_t created_wall_clock_utc_time_ns{};
  const ensemble::EnsembleRequest* request{};
  const ensemble::EnsembleForecast* ensemble_forecast{};
  const risk::RiskDecision* risk_decision{};
  const execution::RoutingDecision* routing_decision{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] bool
build_decision_explanation(const DecisionExplanationInput& input,
                           DecisionExplanationRecord& output) noexcept;
[[nodiscard]] bool
valid_decision_explanation(const DecisionExplanationRecord& record) noexcept;
[[nodiscard]] std::uint64_t
stable_decision_explanation_hash(DecisionExplanationRecord record) noexcept;

// Canonical little-endian representation. It contains every field, excludes
// object padding, and is stable across compiler ABIs for schema version 1.0.
[[nodiscard]] bool
serialize_decision_explanation(const DecisionExplanationRecord& value,
                               std::span<std::uint8_t> output,
                               std::size_t& bytes_written) noexcept;
[[nodiscard]] bool
deserialize_decision_explanation(std::span<const std::uint8_t> input,
                                 DecisionExplanationRecord& output) noexcept;

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct DecisionExplanationJournalResult {
  journal::Status status{journal::Status::stopped};
  std::uint64_t producer_sequence{};

  [[nodiscard]] bool accepted() const noexcept {
    return status == journal::Status::ok && producer_sequence != 0U;
  }
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

// Mandatory explanations are accepted into the bounded fail-closed journal
// queue before an order may cross the gateway boundary. Disk I/O remains on
// the journal consumer thread.
class DecisionExplanationJournalPublisher final {
public:
  explicit DecisionExplanationJournalPublisher(journal::AsyncJournal& journal) noexcept
      : journal_(journal) {}

  [[nodiscard]] DecisionExplanationJournalResult
  publish(const DecisionExplanationRecord& record) noexcept;

private:
  journal::AsyncJournal& journal_;
};

class DecisionExplanationPublisher final {
public:
  [[nodiscard]] bool publish(const DecisionExplanationRecord& record) noexcept;
  [[nodiscard]] bool consume(DecisionExplanationRecord& record) noexcept;
  [[nodiscard]] std::uint64_t dropped_records() const noexcept;

private:
  event_bus::MpscQueue<DecisionExplanationRecord, kDecisionExplanationQueueCapacity>
      queue_;
  std::atomic<std::uint64_t> dropped_records_{0U};
};

class DecisionExplanationJsonExporter final {
public:
  [[nodiscard]] static std::string render(const DecisionExplanationRecord& record);
};

static_assert(std::is_trivially_copyable_v<ExpertDecisionRecord>);
static_assert(std::is_trivially_copyable_v<DecisionExplanationRecord>);

} // namespace aegis::observability

#endif // AEGIS_OBSERVABILITY_DECISION_EXPLANATION_HPP
