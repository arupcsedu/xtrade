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

[[nodiscard]] const Fixture& fixture() {
  static const Fixture value{};
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
BENCHMARK(benchmark_sha256);
BENCHMARK(benchmark_identifier_render);

} // namespace
