#include "aegis/common/audit_envelope.hpp"
#include "aegis/common/identifiers.hpp"
#include "aegis/common/sha256.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <span>

namespace {

namespace wire = aegis::mx::contracts::v1;

constexpr aegis::common::DataQualityContractInput kInput{
    .record_id = aegis::common::GlobalEventId{0x101U, 0x102U},
    .venue_id = aegis::common::VenueId{0x201U, 0x202U},
    .channel_id = aegis::common::ChannelId{0x301U, 0x302U},
    .state = wire::DataQualityCode::VALID,
    .observed_process_monotonic_time_ns = 5'000'000U,
    .last_good_exchange_event_time_ns = 1'800'000'000'000'000'000LL,
    .last_good_nic_receive_time_ns = 1'800'000'000'000'000'500LL,
    .missing_sequence_count = 0,
    .malformed_record_count = 0,
    .stale_after_ns = 250'000'000U,
};

constexpr aegis::common::AuditMetadata kMetadata{
    .envelope_id = aegis::common::GlobalEventId{0x401U, 0x402U},
    .session_id = aegis::common::SessionId{0x501U, 0x502U},
    .configuration_version = aegis::common::ConfigurationVersion{0x601U, 0x602U},
    .created_process_monotonic_time_ns = 5'000'100U,
    .recorded_wall_clock_utc_time_ns = 1'800'000'000'100'000'000LL,
    .previous_envelope_sha256 = {},
};

struct Fixture final {
  flatbuffers::DetachedBuffer contract =
      aegis::common::build_data_quality_contract(kInput);
  flatbuffers::DetachedBuffer envelope =
      aegis::common::build_size_prefixed_audit_envelope(
          {contract.data(), contract.size()}, wire::RecordType::DATA_QUALITY_STATE,
          kMetadata);
};

constexpr aegis::common::ModelForecastContractInput kForecastInput{
    .record_id = aegis::common::GlobalEventId{0x701U, 0x702U},
    .forecast_id = aegis::common::ForecastId{0x711U, 0x712U},
    .session_id = aegis::common::SessionId{0x721U, 0x722U},
    .model_id = aegis::common::ModelId{0x731U, 0x732U},
    .model_version = aegis::common::ModelVersion{0x741U, 0x742U},
    .instrument_id = aegis::common::InstrumentId{0x751U, 0x752U},
    .feature_snapshot_id = aegis::common::FeatureSnapshotId{0x761U, 0x762U},
    .configuration_version = aegis::common::ConfigurationVersion{0x771U, 0x772U},
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
    .horizon_unit = wire::ForecastHorizonUnit::TRADING_MINUTES,
    .horizon_value = 5U,
    .horizon_halt_policy = wire::HorizonHaltPolicy::PAUSE,
    .horizon_session_endpoint = wire::HorizonSessionEndpoint::NOT_APPLICABLE,
    .horizon_calendar_version = aegis::common::ConfigurationVersion{0x791U, 0x792U},
    .target_exchange_event_time_ns = 1'800'000'301'000'000'000LL,
};

struct ForecastFixture final {
  flatbuffers::DetachedBuffer contract =
      aegis::common::build_model_forecast_contract(kForecastInput);
  flatbuffers::DetachedBuffer envelope =
      aegis::common::build_size_prefixed_audit_envelope(
          {contract.data(), contract.size()}, wire::RecordType::MODEL_FORECAST,
          kMetadata);
};

[[nodiscard]] const Fixture& fixture() {
  static const Fixture value{};
  return value;
}

[[nodiscard]] const ForecastFixture& forecast_fixture() {
  static const ForecastFixture value{};
  return value;
}

void benchmark_contract_validation(benchmark::State& state) {
  const auto& encoded = fixture().envelope;
  for (auto _ : state) {
    static_cast<void>(_);
    const auto result = aegis::common::validate_size_prefixed_audit_envelope(
        {encoded.data(), encoded.size()});
    auto error = result.error();
    benchmark::DoNotOptimize(error);
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(encoded.size()));
}

void benchmark_contract_serialization(benchmark::State& state) {
  for (auto _ : state) {
    static_cast<void>(_);
    auto contract = aegis::common::build_data_quality_contract(kInput);
    benchmark::DoNotOptimize(contract.data());
    benchmark::ClobberMemory();
  }
}

void benchmark_forecast_contract_validation(benchmark::State& state) {
  const auto& encoded = forecast_fixture().envelope;
  for (auto _ : state) {
    static_cast<void>(_);
    const auto result = aegis::common::validate_size_prefixed_audit_envelope(
        {encoded.data(), encoded.size()});
    auto error = result.error();
    benchmark::DoNotOptimize(error);
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(encoded.size()));
}

void benchmark_forecast_contract_serialization(benchmark::State& state) {
  for (auto _ : state) {
    static_cast<void>(_);
    auto contract = aegis::common::build_model_forecast_contract(kForecastInput);
    benchmark::DoNotOptimize(contract.data());
    benchmark::ClobberMemory();
  }
}

void benchmark_sha256(benchmark::State& state) {
  const auto& encoded = fixture().contract;
  for (auto _ : state) {
    static_cast<void>(_);
    const auto digest = aegis::common::sha256(
        std::span<const std::uint8_t>{encoded.data(), encoded.size()});
    benchmark::DoNotOptimize(digest.data());
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(encoded.size()));
}

void benchmark_identifier_render(benchmark::State& state) {
  constexpr aegis::common::GlobalEventId identifier{0x0123456789ABCDEFU,
                                                    0xFEDCBA9876543210U};
  for (auto _ : state) {
    static_cast<void>(_);
    const auto rendered = aegis::common::to_hex(identifier);
    benchmark::DoNotOptimize(rendered.data());
  }
}

BENCHMARK(benchmark_contract_validation);
BENCHMARK(benchmark_contract_serialization);
BENCHMARK(benchmark_forecast_contract_validation);
BENCHMARK(benchmark_forecast_contract_serialization);
BENCHMARK(benchmark_sha256);
BENCHMARK(benchmark_identifier_render);

} // namespace
