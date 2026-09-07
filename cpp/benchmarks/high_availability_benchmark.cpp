#include "aegis/high_availability/coordinator.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>

namespace {

namespace ha = aegis::high_availability;
namespace common = aegis::common;
namespace bus = aegis::event_bus;

[[nodiscard]] ha::HaConfiguration configuration() noexcept {
  ha::HaConfiguration result{.session_id = common::SessionId{1U, 1U},
                             .configuration_version =
                                 common::ConfigurationVersion{2U, 1U},
                             .node_id = 1U,
                             .process_id = 101U,
                             .process_epoch = 1U,
                             .exchange_session_epoch = 77U,
                             .heartbeat_timeout_ns = 1'000'000U,
                             .peer_timeout_ns = 1'000'000U,
                             .locality = ha::DeploymentLocality::colocated_edge};
  result.stable_hash = ha::stable_ha_configuration_hash(result);
  return result;
}

void BM_HaFailClosedAuthorityCheck(benchmark::State& state) {
  bus::ProcessEpochState epoch;
  const auto config = configuration();
  auto coordinator = std::make_unique<ha::EdgeHaCoordinator>(config, epoch);
  benchmark::DoNotOptimize(coordinator->start(1U));
  ha::BoundedRecoveryStream recovery(
      {.session_id = config.session_id,
       .exchange_session_epoch = config.exchange_session_epoch});
  ha::EmissionRequest request{.session_id = config.session_id,
                              .command_id = common::GlobalEventId{3U, 1U},
                              .command_hash = 99U,
                              .exchange_session_epoch = config.exchange_session_epoch,
                              .fencing_token = 1U,
                              .process_epoch = config.process_epoch,
                              .source_journal_sequence = 1U,
                              .source_journal_hash = 2U,
                              .process_monotonic_time_ns = 2U};
  request.stable_hash = ha::stable_emission_request_hash(request);
  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(coordinator->authorize_emission(request, recovery));
  }
}

void BM_RecoveryAppendApply(benchmark::State& state) {
  std::uint64_t ordinal = 1U;
  for ([[maybe_unused]] auto iteration : state) {
    state.PauseTiming();
    const ha::RecoveryAuthority authority{.session_id = common::SessionId{1U, 1U},
                                          .exchange_session_epoch = 77U,
                                          .fencing_token = 10U};
    auto recovery = std::make_unique<ha::BoundedRecoveryStream>(authority);
    ha::RecoveryPayload payload{.kind = ha::RecoveryRecordKind::order_emission,
                                .command_id = common::GlobalEventId{3U, ordinal},
                                .command_hash = 300U + ordinal,
                                .source_journal_sequence = ordinal,
                                .source_journal_hash = 100U + ordinal,
                                .process_monotonic_time_ns = 200U + ordinal,
                                .authority = authority};
    payload.stable_hash = ha::stable_recovery_payload_hash(payload);
    ++ordinal;
    state.ResumeTiming();
    benchmark::DoNotOptimize(recovery->append(payload));
    benchmark::DoNotOptimize(recovery->apply_next());
    state.PauseTiming();
    recovery.reset();
    state.ResumeTiming();
  }
}

BENCHMARK(BM_HaFailClosedAuthorityCheck);
BENCHMARK(BM_RecoveryAppendApply);

} // namespace
