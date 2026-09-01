#include "aegis/models/baselines.hpp"
#include "aegis/models/microstructure.hpp"
#include "aegis/models/runner.hpp"
#include "aegis/models/validator.hpp"

#include "allocation_probe.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

namespace models = aegis::models;
namespace features = aegis::features;
namespace common = aegis::common;

namespace {

[[nodiscard]] std::uint64_t fixed_clock(void* context) noexcept {
  return *static_cast<std::uint64_t*>(context);
}

[[nodiscard]] models::ModelMetadata metadata() noexcept {
  models::ModelMetadata result{
      .model_id = common::ModelId{1U, 1U},
      .model_version = common::ModelVersion{2U, 2U},
      .instrument_id = common::InstrumentId{3U, 3U},
      .configuration_version = common::ConfigurationVersion{4U, 4U},
      .feature_definition_version = common::ConfigurationVersion{8U, 8U},
      .kind = models::ModelKind::last_value,
      .requirement_count = 1U,
      .horizon_ns = 1'000'000U,
      .forecast_ttl_ns = 1'000U,
      .calibration = {},
      .ood = {}};
  result.requirements[0U] = {.name = features::FeatureName::rolling_return_ppm,
                             .minimum_value = -models::kMaximumAbsoluteReturnPpm,
                             .maximum_value = models::kMaximumAbsoluteReturnPpm,
                             .maximum_age_ns = 1'000U};
  return result;
}

[[nodiscard]] models::ModelInput input() noexcept {
  models::ModelInput result{
      .forecast_id = common::ForecastId{5U, 5U},
      .feature_snapshot =
          {.snapshot_id = common::FeatureSnapshotId{6U, 6U},
           .instrument_id = common::InstrumentId{3U, 3U},
           .session_id = common::SessionId{7U, 7U},
           .feature_definition_version = common::ConfigurationVersion{8U, 8U},
           .source_first_global_event_id = common::GlobalEventId{9U, 9U},
           .source_last_global_event_id = common::GlobalEventId{10U, 10U},
           .source_first_ordinal = 1U,
           .source_last_ordinal = 2U,
           .as_of_exchange_event_time_ns = 1'800'000'000'000'000'000LL,
           .created_process_monotonic_time_ns = 100U,
           .stable_hash = 1U,
           .state = features::EngineState::ready},
      .deadline = {.submitted_process_monotonic_time_ns = 100U,
                   .complete_by_process_monotonic_time_ns = 200U},
      .microstructure_context = {}};
  for (auto& feature : result.feature_snapshot.values) {
    feature = {.value = 0,
               .as_of_process_monotonic_time_ns = 100U,
               .validity = features::FeatureValidity::valid};
  }
  result.feature_snapshot
      .values[static_cast<std::size_t>(features::FeatureName::rolling_return_ppm)]
      .value = 1'000;
  result.feature_snapshot.stable_hash =
      features::stable_snapshot_hash(result.feature_snapshot);
  return result;
}

void benchmark_model_validation(benchmark::State& state) {
  const auto model_metadata = metadata();
  const auto model_input = input();
  const auto allocations_before = allocation_probe::count();
  for (auto _ : state) {
    static_cast<void>(_);
    auto result = models::ModelForecastValidator::validate_input(model_metadata,
                                                                 model_input, 110U);
    benchmark::DoNotOptimize(result);
  }
  state.counters["steady_state_allocations"] = benchmark::Counter(
      static_cast<double>(allocation_probe::count() - allocations_before));
}

void benchmark_local_model_runner(benchmark::State& state) {
  auto clock = std::uint64_t{110U};
  models::LastValueModel model{metadata()};
  models::LocalModelRunner runner{model, {fixed_clock, &clock}};
  const auto model_input = input();
  const auto allocations_before = allocation_probe::count();
  for (auto _ : state) {
    static_cast<void>(_);
    auto result = runner.run(model_input);
    benchmark::DoNotOptimize(result);
  }
  state.counters["steady_state_allocations"] = benchmark::Counter(
      static_cast<double>(allocation_probe::count() - allocations_before));
}

[[nodiscard]] models::NativeModelArtifact native_artifact() noexcept {
  auto result = models::NativeModelArtifact{
      .metadata = metadata(),
      .role = models::MicrostructureModelRole::queue_depletion,
      .family = models::NativeModelFamily::logistic_regression,
      .feature_count = 8U,
      .intercept_ppm = -100'000,
      .platt_slope_ppm = 1'000'000,
      .training_sample_count = 100'000U,
      .calibration_error_ppm = 20'000U,
      .expires_wall_clock_utc_ns = 1'900'000'000'000'000'000LL,
      .activation_expiration_process_monotonic_time_ns = 1'000'000U,
  };
  result.metadata.kind = models::ModelKind::logistic_regression;
  result.features[0U] = {.name = models::NativeFeatureName::quantity_ahead_units,
                         .center = 500,
                         .scale = 500U,
                         .training_minimum = 1,
                         .training_maximum = 1'000,
                         .weight_ppm = -100'000};
  result.features[1U] = {.name = models::NativeFeatureName::add_rate_millihertz,
                         .center = 1'000,
                         .scale = 1'000U,
                         .training_minimum = 0,
                         .training_maximum = 10'000,
                         .weight_ppm = -50'000};
  result.features[2U] = {.name = models::NativeFeatureName::cancel_rate_millihertz,
                         .center = 1'000,
                         .scale = 1'000U,
                         .training_minimum = 0,
                         .training_maximum = 10'000,
                         .weight_ppm = 100'000};
  result.features[3U] = {.name = models::NativeFeatureName::execute_rate_millihertz,
                         .center = 1'000,
                         .scale = 1'000U,
                         .training_minimum = 0,
                         .training_maximum = 10'000,
                         .weight_ppm = 200'000};
  result.features[4U] = {.name = models::NativeFeatureName::order_age_ns,
                         .center = 100'000,
                         .scale = 100'000U,
                         .training_minimum = 0,
                         .training_maximum = 1'000'000,
                         .weight_ppm = 50'000};
  result.features[5U] = {.name = models::NativeFeatureName::price_distance_ticks,
                         .center = 1,
                         .scale = 1U,
                         .training_minimum = 0,
                         .training_maximum = 10,
                         .weight_ppm = -50'000};
  result.features[6U] = {.name = models::NativeFeatureName::venue_number,
                         .center = 1,
                         .scale = 1U,
                         .training_minimum = 1,
                         .training_maximum = 8,
                         .weight_ppm = 10'000};
  result.features[7U] = {.name = models::NativeFeatureName::session_progress_ppm,
                         .center = 500'000,
                         .scale = 500'000U,
                         .training_minimum = 0,
                         .training_maximum = 1'000'000,
                         .weight_ppm = 10'000};
  return result;
}

void benchmark_native_microstructure_inference(benchmark::State& state) {
  constexpr double kConfiguredLatencyBudgetNs = 10'000.0;
  models::NativeMicrostructureModel model{native_artifact()};
  auto model_input = input();
  model_input.microstructure_context = {.quantity_ahead_units = 400U,
                                        .order_quantity_units = 100U,
                                        .order_age_ns = 50'000U,
                                        .price_distance_ticks = 1,
                                        .venue_number = 1U,
                                        .fee_rate_ppm = 30U,
                                        .present = true};
  model_input.feature_snapshot
      .values[static_cast<std::size_t>(features::FeatureName::add_rate_millihertz)]
      .value = 2'000;
  model_input.feature_snapshot
      .values[static_cast<std::size_t>(features::FeatureName::cancel_rate_millihertz)]
      .value = 2'000;
  model_input.feature_snapshot
      .values[static_cast<std::size_t>(features::FeatureName::execute_rate_millihertz)]
      .value = 1'000;
  model_input.feature_snapshot
      .values[static_cast<std::size_t>(features::FeatureName::session_progress_ppm)]
      .value = 500'000;
  models::ModelPrediction prediction{};
  const auto allocations_before = allocation_probe::count();
  for (auto _ : state) {
    static_cast<void>(_);
    auto status = model.predict(model_input, prediction);
    benchmark::DoNotOptimize(status);
    benchmark::DoNotOptimize(prediction);
  }
  const auto steady_state_allocations = allocation_probe::count() - allocations_before;
  state.counters["steady_state_allocations"] =
      benchmark::Counter(static_cast<double>(steady_state_allocations));
  state.counters["configured_budget_ns"] = kConfiguredLatencyBudgetNs;
}

BENCHMARK(benchmark_model_validation);
BENCHMARK(benchmark_local_model_runner);
BENCHMARK(benchmark_native_microstructure_inference);

} // namespace
