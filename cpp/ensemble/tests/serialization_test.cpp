#include "aegis/ensemble/serialization.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace ensemble = aegis::ensemble;
namespace common = aegis::common;

namespace {

[[nodiscard]] common::AuditMetadata audit_metadata() noexcept {
  return {.envelope_id = common::GlobalEventId{80U, 80U},
          .session_id = ensemble::test::kSession,
          .configuration_version = ensemble::test::kConfiguration,
          .created_process_monotonic_time_ns = ensemble::test::kNow + 1U,
          .recorded_wall_clock_utc_time_ns = 1'800'000'000'000'000'000LL,
          .previous_envelope_sha256 = {}};
}

[[nodiscard]] std::span<const std::uint8_t>
bytes(const flatbuffers::DetachedBuffer& buffer) noexcept {
  return {buffer.data(), buffer.size()};
}

} // namespace

TEST(EnsembleSerializationTest, RoundTripsCompleteDecisionThroughAuditEnvelope) {
  const ensemble::MixtureOfExpertsGate gate{ensemble::test::config()};
  const auto result = gate.evaluate(ensemble::test::request());
  const auto contract = ensemble::build_ensemble_forecast_contract(
      {.record_id = common::GlobalEventId{81U, 81U}, .forecast = result.forecast});
  ASSERT_NE(contract.data(), nullptr);

  const auto envelope = common::build_size_prefixed_audit_envelope(
      bytes(contract), common::wire::RecordType::ENSEMBLE_FORECAST, audit_metadata());
  common::ValidatedAuditEnvelopeView view{};
  const auto validation =
      common::validate_size_prefixed_audit_envelope(bytes(envelope), &view);
  ASSERT_TRUE(validation.ok());
  const auto* wire_forecast = view.record->payload_as_EnsembleForecast();
  ASSERT_NE(wire_forecast, nullptr);
  const auto* model_weights = wire_forecast->model_weights();
  const auto* contributing_ids = wire_forecast->contributing_forecast_ids();
  ASSERT_NE(model_weights, nullptr);
  ASSERT_NE(contributing_ids, nullptr);
  EXPECT_EQ(wire_forecast->ensemble_hash(), result.forecast.stable_hash);
  EXPECT_EQ(model_weights->size(), 2U);
  EXPECT_EQ(contributing_ids->size(), 2U);
  EXPECT_FALSE(wire_forecast->abstain());
}

TEST(EnsembleSerializationTest, RoundTripsAllInvalidAbstentionWithEmptyContributors) {
  auto request = ensemble::test::request();
  request.experts[0U].health.state = aegis::models::ModelHealthState::disabled;
  request.experts[1U].health.state = aegis::models::ModelHealthState::disabled;
  const ensemble::MixtureOfExpertsGate gate{ensemble::test::config()};
  const auto result = gate.evaluate(request);
  ASSERT_TRUE(result.forecast.abstain);
  const auto contract = ensemble::build_ensemble_forecast_contract(
      {.record_id = common::GlobalEventId{82U, 82U}, .forecast = result.forecast});
  ASSERT_NE(contract.data(), nullptr);

  const auto envelope = common::build_size_prefixed_audit_envelope(
      bytes(contract), common::wire::RecordType::ENSEMBLE_FORECAST, audit_metadata());
  common::ValidatedAuditEnvelopeView view{};
  const auto validation =
      common::validate_size_prefixed_audit_envelope(bytes(envelope), &view);
  ASSERT_TRUE(validation.ok());
  const auto* wire_forecast = view.record->payload_as_EnsembleForecast();
  ASSERT_NE(wire_forecast, nullptr);
  const auto* model_weights = wire_forecast->model_weights();
  const auto* contributing_ids = wire_forecast->contributing_forecast_ids();
  ASSERT_NE(model_weights, nullptr);
  ASSERT_NE(contributing_ids, nullptr);
  EXPECT_TRUE(contributing_ids->empty());
  EXPECT_EQ(model_weights->size(), 2U);
}

TEST(EnsembleSerializationTest, RejectsTamperedInMemoryDecision) {
  const ensemble::MixtureOfExpertsGate gate{ensemble::test::config()};
  auto forecast = gate.evaluate(ensemble::test::request()).forecast;
  ++forecast.expected_return_ppm;
  const auto contract = ensemble::build_ensemble_forecast_contract(
      {.record_id = common::GlobalEventId{83U, 83U}, .forecast = forecast});
  EXPECT_EQ(contract.data(), nullptr);
  EXPECT_EQ(contract.size(), 0U);
}
