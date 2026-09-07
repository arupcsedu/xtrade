#include "aegis/observability/decision_explanation.hpp"

#include "../../ensemble/tests/test_support.hpp"
#include "../../execution/tests/test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>

namespace aegis::observability::test {
namespace {

struct EnsembleFixture {
  ensemble::EnsembleRequest request;
  ensemble::EnsembleForecast forecast;
};

[[nodiscard]] EnsembleFixture ensemble_fixture(const std::uint32_t experts = 2U) {
  auto request = ensemble::test::request(experts);
  request.session_id = risk::test::kSession;
  request.instrument_id = risk::test::kInstrument;
  request.configuration_version = risk::test::kConfiguration;
  for (std::size_t index = 0U; index < request.expert_count; ++index) {
    request.experts[index].forecast.session_id = risk::test::kSession;
    request.experts[index].forecast.instrument_id = risk::test::kInstrument;
    request.experts[index].forecast.configuration_version = risk::test::kConfiguration;
    request.experts[index].forecast.stable_hash =
        models::stable_forecast_hash(request.experts[index].forecast);
  }
  request.transaction_cost_forecast.session_id = risk::test::kSession;
  request.transaction_cost_forecast.instrument_id = risk::test::kInstrument;
  request.transaction_cost_forecast.configuration_version = risk::test::kConfiguration;
  request.transaction_cost_forecast.stable_hash =
      models::stable_forecast_hash(request.transaction_cost_forecast);
  auto config = ensemble::test::config();
  config.configuration_version = risk::test::kConfiguration;
  config.stable_hash = ensemble::stable_ensemble_config_hash(config);
  const ensemble::MixtureOfExpertsGate gate{config};
  return {.request = request, .forecast = gate.evaluate(request).forecast};
}

[[nodiscard]] execution::RoutingDecision routing_decision() noexcept {
  execution::RoutingDecision decision{
      .status = execution::RoutingStatus::routed,
      .reason = execution::RoutingReason::routed,
      .objective_id = common::IntentId{100U, 1U},
      .venue_id = risk::test::kVenue,
      .target_order_id = {},
      .configuration_version = risk::test::kConfiguration,
      .action = execution::RoutedAction::new_order,
      .time_in_force = execution::RoutedTimeInForce::day,
      .price_ticks = 100,
      .quantity_units = 10U,
      .expected_value_currency_nanos_per_unit = 15,
      .route_attempt = 1U,
      .evaluated_venue_count = 1U,
      .eligible_venue_count = 1U,
      .source_objective_hash = 101U,
      .source_request_hash = 102U,
      .configuration_hash = 103U,
      .requires_fresh_pretrade_risk = true};
  decision.venue_scores[0U] = {.venue_id = risk::test::kVenue,
                               .eligibility =
                                   execution::VenueEligibilityReason::eligible,
                               .selected_expected_value_currency_nanos_per_unit = 15,
                               .effective_fill_probability_ppm = 800'000U,
                               .fill_uncertainty_ppm = 640'000U,
                               .proposed_quantity_units = 10U,
                               .proposed_price_ticks = 100,
                               .tie_break_rank = 1U,
                               .observation_hash = 104U};
  decision.stable_hash = execution::stable_routing_decision_hash(decision);
  return decision;
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    std::array<char, 64U> pattern{};
    constexpr std::string_view value = "/tmp/aegis-observability-test.XXXXXX";
    std::ranges::copy(value, pattern.begin());
    if (auto* path = ::mkdtemp(pattern.data()); path != nullptr) {
      path_ = path;
    }
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

[[nodiscard]] common::Sha256Digest test_digest(const std::string_view value) {
  return common::sha256(std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

[[nodiscard]] journal::WriterConfig
journal_configuration(const std::filesystem::path& path) {
  return {.directory = path,
          .maximum_segment_bytes = std::uint64_t{1024U} * 1024U,
          .maximum_records_per_segment = 1'000U,
          .index_stride_records = 2U,
          .sync_policy = journal::SyncPolicy::every_record,
          .periodic_sync_records = 1U,
          .configuration_sha256 = test_digest("observability-test-config"),
          .build_sha256 = test_digest("observability-test-build"),
          .writer_instance_id = common::GlobalEventId{0xA11D17U, 0x0B5E12U}};
}

[[nodiscard]] DecisionExplanationRecord full_record() {
  const auto ensemble = ensemble_fixture();
  const auto approved = execution::test::approval();
  const auto routing = routing_decision();
  DecisionExplanationRecord record{};
  const bool built = build_decision_explanation(
      {.explanation_id = common::GlobalEventId{200U, 10U},
       .correlation_id = approved.decision.global_event_id,
       .created_process_monotonic_time_ns = risk::test::kNow + 10U,
       .created_wall_clock_utc_time_ns = 1'800'000'000'000'000'010LL,
       .request = &ensemble.request,
       .ensemble_forecast = &ensemble.forecast,
       .risk_decision = &approved.decision,
       .routing_decision = &routing},
      record);
  return built ? record : DecisionExplanationRecord{};
}

TEST(DecisionExplanationTest, CapturesModelRiskAndVenueDecisionWithCorrelation) {
  const auto ensemble = ensemble_fixture();
  const auto approved = execution::test::approval();
  const auto routing = routing_decision();
  DecisionExplanationRecord record;
  const DecisionExplanationInput input{
      .explanation_id = common::GlobalEventId{200U, 1U},
      .correlation_id = approved.decision.global_event_id,
      .created_process_monotonic_time_ns = risk::test::kNow + 1U,
      .created_wall_clock_utc_time_ns = 1'800'000'000'000'000'001LL,
      .request = &ensemble.request,
      .ensemble_forecast = &ensemble.forecast,
      .risk_decision = &approved.decision,
      .routing_decision = &routing};

  ASSERT_TRUE(build_decision_explanation(input, record));
  EXPECT_TRUE(valid_decision_explanation(record));
  EXPECT_EQ(record.disposition, DecisionDisposition::routed);
  EXPECT_EQ(record.expert_count, 2U);
  EXPECT_EQ(record.experts[0U].feature_snapshot_id,
            ensemble.request.experts[0U].forecast.feature_snapshot_id);
  EXPECT_EQ(record.experts[0U].expected_return_ppm,
            ensemble.request.experts[0U].forecast.prediction.expected_return_ppm);
  EXPECT_EQ(record.risk_decision, risk::DecisionCode::approved);
  EXPECT_EQ(record.selected_venue_id, risk::test::kVenue);

  const auto json = DecisionExplanationJsonExporter::render(record);
  EXPECT_NE(json.find("\"correlation_id\":\""), std::string::npos);
  EXPECT_NE(json.find("\"model_outputs\":["), std::string::npos);
  EXPECT_NE(json.find("\"selected_venue_id\":\""), std::string::npos);
}

TEST(DecisionExplanationTest, AbstentionHasNoRiskOrRouteAndTamperingIsDetected) {
  const auto ensemble = ensemble_fixture(0U);
  ASSERT_TRUE(ensemble.forecast.abstain);
  DecisionExplanationRecord record;
  ASSERT_TRUE(build_decision_explanation(
      {.explanation_id = common::GlobalEventId{200U, 2U},
       .correlation_id = common::GlobalEventId{201U, 2U},
       .created_process_monotonic_time_ns = risk::test::kNow + 2U,
       .created_wall_clock_utc_time_ns = 1'800'000'000'000'000'002LL,
       .request = &ensemble.request,
       .ensemble_forecast = &ensemble.forecast},
      record));
  EXPECT_EQ(record.disposition, DecisionDisposition::abstained);
  EXPECT_FALSE(record.risk_present);
  EXPECT_FALSE(record.routing_present);

  ++record.disagreement_ppm;
  EXPECT_FALSE(valid_decision_explanation(record));
}

TEST(DecisionExplanationTest,
     RobustEdgeAbstentionRetainsNormalizedExpertWeightsForAudit) {
  auto request = ensemble::test::request(2U);
  request.session_id = risk::test::kSession;
  request.instrument_id = risk::test::kInstrument;
  request.configuration_version = risk::test::kConfiguration;
  for (std::size_t index = 0U; index < request.expert_count; ++index) {
    auto& forecast = request.experts[index].forecast;
    forecast.session_id = risk::test::kSession;
    forecast.instrument_id = risk::test::kInstrument;
    forecast.configuration_version = risk::test::kConfiguration;
    forecast.prediction.expected_return_ppm = 1'000;
    forecast.prediction.return_p10_ppm = 500;
    forecast.prediction.return_p50_ppm = 1'000;
    forecast.prediction.return_p90_ppm = 1'500;
    forecast.stable_hash = models::stable_forecast_hash(forecast);
  }
  request.transaction_cost_forecast.session_id = risk::test::kSession;
  request.transaction_cost_forecast.instrument_id = risk::test::kInstrument;
  request.transaction_cost_forecast.configuration_version = risk::test::kConfiguration;
  request.transaction_cost_forecast.stable_hash =
      models::stable_forecast_hash(request.transaction_cost_forecast);
  auto config = ensemble::test::config();
  config.configuration_version = risk::test::kConfiguration;
  config.stable_hash = ensemble::stable_ensemble_config_hash(config);
  const ensemble::MixtureOfExpertsGate gate{config};
  const auto forecast = gate.evaluate(request).forecast;
  ASSERT_TRUE(forecast.abstain);
  ASSERT_NE(forecast.explanation.contributing_expert_count, 0U);

  DecisionExplanationRecord record;
  ASSERT_TRUE(build_decision_explanation(
      {.explanation_id = common::GlobalEventId{200U, 4U},
       .correlation_id = common::GlobalEventId{201U, 4U},
       .created_process_monotonic_time_ns = ensemble::test::kNow + 4U,
       .created_wall_clock_utc_time_ns = 1'800'000'000'000'000'004LL,
       .request = &request,
       .ensemble_forecast = &forecast},
      record));
  EXPECT_TRUE(valid_decision_explanation(record));
  EXPECT_EQ(record.disposition, DecisionDisposition::abstained);
  EXPECT_EQ(record.experts[0U].weight_ppm + record.experts[1U].weight_ppm,
            models::kProbabilityScale);
}

TEST(DecisionExplanationTest, PreservesExcludedMalformedExpertWithoutMakingItEligible) {
  auto request = ensemble::test::request(1U);
  request.experts[0U].forecast.feature_snapshot_id = {};
  request.experts[0U].forecast.stable_hash =
      models::stable_forecast_hash(request.experts[0U].forecast);
  const auto config = ensemble::test::config();
  const ensemble::MixtureOfExpertsGate gate{config};
  const auto forecast = gate.evaluate(request).forecast;
  ASSERT_TRUE(forecast.abstain);
  ASSERT_EQ(forecast.explanation.experts[0U].eligibility,
            ensemble::EligibilityReason::missing_provenance);

  DecisionExplanationRecord record;
  ASSERT_TRUE(build_decision_explanation(
      {.explanation_id = common::GlobalEventId{200U, 3U},
       .correlation_id = common::GlobalEventId{201U, 3U},
       .created_process_monotonic_time_ns = ensemble::test::kNow + 3U,
       .created_wall_clock_utc_time_ns = 1'800'000'000'000'000'003LL,
       .request = &request,
       .ensemble_forecast = &forecast},
      record));
  EXPECT_FALSE(record.experts[0U].feature_snapshot_id.valid());
  EXPECT_EQ(record.experts[0U].weight_ppm, 0U);
  EXPECT_EQ(record.experts[0U].eligibility,
            ensemble::EligibilityReason::missing_provenance);
}

TEST(DecisionExplanationTest, PublisherRejectsInvalidAndNeverRepairsIt) {
  DecisionExplanationPublisher publisher;
  DecisionExplanationRecord invalid;
  EXPECT_FALSE(publisher.publish(invalid));
  EXPECT_EQ(publisher.dropped_records(), 1U);
  EXPECT_FALSE(publisher.consume(invalid));
}

TEST(DecisionExplanationTest, CanonicalBinaryRoundTripRetainsFullExplanation) {
  const auto source = full_record();
  ASSERT_TRUE(valid_decision_explanation(source));
  std::array<std::uint8_t, kMaximumDecisionExplanationBinaryBytes> bytes{};
  std::size_t size{};
  ASSERT_TRUE(serialize_decision_explanation(source, bytes, size));
  ASSERT_GT(size, 0U);
  DecisionExplanationRecord restored{};
  ASSERT_TRUE(deserialize_decision_explanation(
      std::span<const std::uint8_t>{bytes.data(), size}, restored));
  EXPECT_EQ(restored.stable_hash, source.stable_hash);
  EXPECT_EQ(restored.routing_decision_hash, source.routing_decision_hash);
  EXPECT_EQ(restored.risk_decision_hash, source.risk_decision_hash);
  EXPECT_EQ(restored.experts[0U].forecast_hash, source.experts[0U].forecast_hash);
}

// GoogleTest assertion macros dominate the reported score; the loop fills the
// bounded queue to its exact, security-relevant capacity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(DecisionExplanationTest, MandatoryJournalOverflowFailsClosed) {
  const TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  journal::AsyncJournal journal;
  ASSERT_EQ(journal.open(journal_configuration(temporary.path() / "journal")),
            journal::Status::ok);
  DecisionExplanationJournalPublisher publisher{journal};
  const auto record = full_record();
  for (std::size_t index = 0U; index < journal::kAsyncQueueCapacity; ++index) {
    ASSERT_TRUE(publisher.publish(record).accepted());
  }
  const auto rejected = publisher.publish(record);
  EXPECT_FALSE(rejected.accepted());
  EXPECT_EQ(rejected.status, journal::Status::inhibited);
  EXPECT_EQ(journal.health(), journal::HealthState::unsafe);
  EXPECT_EQ(journal.shutdown(std::chrono::seconds(2)), journal::Status::inhibited);
}

} // namespace
} // namespace aegis::observability::test
