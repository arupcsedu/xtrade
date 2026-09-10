#include "aegis/common/audit_envelope.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace common = aegis::common;
namespace wire = aegis::mx::contracts::v1;

namespace {

constexpr common::ModelForecastContractInput kInput{
    .record_id = common::GlobalEventId{0x701U, 0x702U},
    .forecast_id = common::ForecastId{0x711U, 0x712U},
    .session_id = common::SessionId{0x721U, 0x722U},
    .model_id = common::ModelId{0x731U, 0x732U},
    .model_version = common::ModelVersion{0x741U, 0x742U},
    .instrument_id = common::InstrumentId{0x751U, 0x752U},
    .feature_snapshot_id = common::FeatureSnapshotId{0x761U, 0x762U},
    .configuration_version = common::ConfigurationVersion{0x771U, 0x772U},
    .expected_return_ppm = 1'250,
    .return_p10_ppm = -2'000,
    .return_p50_ppm = 1'000,
    .return_p90_ppm = 4'500,
    .probability_down_ppm = 250'000U,
    .probability_flat_ppm = 150'000U,
    .probability_up_ppm = 600'000U,
    .volatility_ppm = 3'250U,
    .confidence_ppm = 800'000U,
    .calibration_score_ppm = 900'000U,
    .data_quality_score_ppm = 1'000'000U,
    .ood_score_ppm = 25'000U,
    .horizon_ns = 300'000'000'000U,
    .as_of_exchange_event_time_ns = 1'800'000'001'000'000'000LL,
    .production_process_monotonic_time_ns = 6'000'000U,
    .expiration_process_monotonic_time_ns = 6'250'000U,
    .has_transaction_cost_estimate = true,
    .estimated_spread_cost_ppm = 100U,
    .estimated_slippage_cost_ppm = 125U,
    .estimated_market_impact_ppm = 250U,
    .estimated_adverse_selection_cost_ppm = 75U,
    .estimated_fee_cost_ppm = 50U,
    .horizon_unit = wire::ForecastHorizonUnit::TRADING_MINUTES,
    .horizon_value = 5U,
    .horizon_halt_policy = wire::HorizonHaltPolicy::PAUSE,
    .horizon_session_endpoint = wire::HorizonSessionEndpoint::NOT_APPLICABLE,
    .horizon_calendar_version = common::ConfigurationVersion{0x791U, 0x792U},
    .target_exchange_event_time_ns = 1'800'000'301'000'000'000LL};

constexpr common::AuditMetadata kMetadata{
    .envelope_id = common::GlobalEventId{0x781U, 0x782U},
    .session_id = common::SessionId{0x721U, 0x722U},
    .configuration_version = common::ConfigurationVersion{0x771U, 0x772U},
    .created_process_monotonic_time_ns = 6'000'100U,
    .recorded_wall_clock_utc_time_ns = 1'800'000'001'100'000'000LL};

[[nodiscard]] std::vector<std::uint8_t>
envelope(const common::ModelForecastContractInput& input = kInput) {
  const auto contract = common::build_model_forecast_contract(input);
  const auto encoded = common::build_size_prefixed_audit_envelope(
      {contract.data(), contract.size()}, wire::RecordType::MODEL_FORECAST, kMetadata);
  return {encoded.data(), encoded.data() + encoded.size()};
}

} // namespace

TEST(ModelContractTest, ValidatesV19HorizonAndMatchesPythonGolden) {
  const auto actual = envelope();
  common::ValidatedAuditEnvelopeView view{};
  ASSERT_TRUE(common::validate_size_prefixed_audit_envelope(actual, &view).ok());
  const auto* forecast = view.record->payload_as_ModelForecast();
  ASSERT_NE(forecast, nullptr);
  EXPECT_EQ(forecast->forecast_unit(), wire::ForecastUnit::RETURN_PPM);
  EXPECT_EQ(forecast->forecast_target(), wire::ForecastTarget::RETURN);
  EXPECT_EQ(forecast->target_value(), 1'250);
  EXPECT_EQ(forecast->expected_return_ppm(), 1'250);
  EXPECT_EQ(forecast->probability_up_ppm(), 600'000U);
  EXPECT_TRUE(forecast->has_transaction_cost_estimate());
  EXPECT_EQ(forecast->estimated_slippage_cost_ppm(), 125U);
  EXPECT_EQ(forecast->estimated_adverse_selection_cost_ppm(), 75U);
  ASSERT_NE(forecast->horizon_spec(), nullptr);
  EXPECT_EQ(forecast->horizon_spec()->unit(),
            wire::ForecastHorizonUnit::TRADING_MINUTES);
  EXPECT_EQ(forecast->horizon_spec()->value(), 5U);
  EXPECT_EQ(forecast->horizon_spec()->halt_policy(), wire::HorizonHaltPolicy::PAUSE);
  ASSERT_NE(forecast->target_exchange_event_time(), nullptr);
  EXPECT_EQ(forecast->target_exchange_event_time()->value(),
            1'800'000'301'000'000'000LL);

  const auto path = std::filesystem::path{AEGIS_SOURCE_DIR} /
                    "schemas/golden/model_forecast_v1_9.amae";
  std::ifstream stream{path, std::ios::binary};
  ASSERT_TRUE(stream.good()) << path;
  const std::vector<std::uint8_t> golden{std::istreambuf_iterator<char>{stream},
                                         std::istreambuf_iterator<char>{}};
  EXPECT_EQ(actual, golden);
}

TEST(ModelContractTest, SemanticValidationRejectsBadDirectionalProbabilities) {
  auto invalid = kInput;
  invalid.probability_up_ppm -= 1U;
  const auto result = common::validate_size_prefixed_audit_envelope(envelope(invalid));
  EXPECT_EQ(result.error(), common::ValidationError::invalid_value);
}

TEST(ModelContractTest, SemanticValidationRejectsUnknownHorizonEnum) {
  auto invalid = kInput;
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  invalid.horizon_unit = static_cast<wire::ForecastHorizonUnit>(255U);
  const auto result = common::validate_size_prefixed_audit_envelope(envelope(invalid));
  EXPECT_EQ(result.error(), common::ValidationError::invalid_enum);
}

TEST(ModelContractTest, SemanticValidationRejectsHorizonTargetMismatch) {
  auto invalid = kInput;
  invalid.target_exchange_event_time_ns += 1;
  const auto result = common::validate_size_prefixed_audit_envelope(envelope(invalid));
  EXPECT_EQ(result.error(), common::ValidationError::invalid_relationship);
}

TEST(ModelContractTest, V19ReaderAcceptsStoredV18ForecastWithoutNewFields) {
  const auto path = std::filesystem::path{AEGIS_SOURCE_DIR} /
                    "schemas/golden/model_forecast_v1_3.amae";
  std::ifstream stream{path, std::ios::binary};
  ASSERT_TRUE(stream.good()) << path;
  const std::vector<std::uint8_t> golden{std::istreambuf_iterator<char>{stream},
                                         std::istreambuf_iterator<char>{}};
  common::ValidatedAuditEnvelopeView view{};
  const auto result = common::validate_size_prefixed_audit_envelope(golden, &view);
  ASSERT_TRUE(result.ok());
  ASSERT_NE(view.record, nullptr);
  const auto* forecast = view.record->payload_as_ModelForecast();
  ASSERT_NE(forecast, nullptr);
  EXPECT_EQ(forecast->schema_version()->minor(), 8U);
  EXPECT_EQ(forecast->horizon_spec(), nullptr);
  EXPECT_EQ(forecast->target_exchange_event_time(), nullptr);
}
