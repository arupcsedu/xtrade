#include "aegis/models/runner.hpp"

#include <cstdint>
#include <limits>

namespace aegis::models {
namespace {

inline constexpr std::uint64_t kHealthMask = 0xFFU;
inline constexpr unsigned kGenerationShift = 8U;
inline constexpr std::uint64_t kMaximumGeneration =
    std::numeric_limits<std::uint64_t>::max() >> kGenerationShift;

[[nodiscard]] std::uint64_t pack(const ModelControlUpdate update) noexcept {
  return (update.generation << kGenerationShift) |
         static_cast<std::uint64_t>(update.state);
}

[[nodiscard]] ModelHealthSnapshot unpack(const std::uint64_t packed) noexcept {
  return {.generation = packed >> kGenerationShift,
          .state = static_cast<ModelHealthState>(packed & kHealthMask)};
}

[[nodiscard]] bool runnable(const ModelHealthState state) noexcept {
  return state == ModelHealthState::healthy || state == ModelHealthState::degraded;
}

[[nodiscard]] bool valid_control_state(const ModelHealthState state) noexcept {
  return state == ModelHealthState::warming || state == ModelHealthState::healthy ||
         state == ModelHealthState::degraded || state == ModelHealthState::disabled ||
         state == ModelHealthState::failed;
}

[[nodiscard]] RunStatus
status_for_validation(const ForecastValidationError error) noexcept {
  return error == ForecastValidationError::late ? RunStatus::deadline_missed
                                                : RunStatus::invalid_request;
}

} // namespace

LocalModelRunner::LocalModelRunner(IForecastModel& model, MonotonicClockSource clock,
                                   const FallbackPolicy fallback_policy,
                                   IForecastModel* fallback_model) noexcept
    : model_(model), clock_(clock), fallback_policy_(fallback_policy),
      fallback_model_(fallback_model),
      control_(pack({.generation = 1U, .state = ModelHealthState::healthy})) {}

RunResult LocalModelRunner::run(const ModelInput& input) noexcept {
  const auto control = health();
  if (!runnable(control.state)) {
    return {.status = RunStatus::disabled, .forecast = {}};
  }
  auto result = run_one(model_, input, false);
  if (result.status != RunStatus::model_failure ||
      fallback_policy_ != FallbackPolicy::explicit_model_on_failure ||
      fallback_model_ == nullptr) {
    return result;
  }
  auto fallback_result = run_one(*fallback_model_, input, true);
  if (fallback_result.status == RunStatus::accepted) {
    fallback_result.status = RunStatus::fallback_accepted;
    fallback_result.used_fallback = true;
    return fallback_result;
  }
  fallback_result.status = RunStatus::fallback_failure;
  fallback_result.used_fallback = true;
  return fallback_result;
}

RunResult LocalModelRunner::run_one(IForecastModel& model, const ModelInput& input,
                                    const bool fallback) noexcept {
  const auto admitted_control = health();
  if (!runnable(admitted_control.state)) {
    return {.status = RunStatus::disabled, .forecast = {}, .used_fallback = fallback};
  }
  const auto started = clock_.now_ns();
  const auto input_error =
      ModelForecastValidator::validate_input(model.metadata(), input, started);
  if (input_error != ForecastValidationError::none) {
    return {.status = status_for_validation(input_error),
            .validation_error = input_error,
            .forecast = {},
            .used_fallback = fallback};
  }
  ModelPrediction prediction{};
  if (model.predict(input, prediction) != PredictionStatus::ok) {
    return {
        .status = RunStatus::model_failure, .forecast = {}, .used_fallback = fallback};
  }
  const auto completed = clock_.now_ns();
  const auto completed_control = health();
  if (!runnable(completed_control.state) ||
      completed_control.generation != admitted_control.generation) {
    return {.status = RunStatus::disabled, .forecast = {}, .used_fallback = fallback};
  }
  if (completed == 0U ||
      completed > input.deadline.complete_by_process_monotonic_time_ns) {
    return {.status = RunStatus::deadline_missed,
            .validation_error = ForecastValidationError::late,
            .forecast = {},
            .used_fallback = fallback};
  }
  if (prediction.ood_score_ppm > model.metadata().ood.reject_threshold_ppm) {
    return {.status = RunStatus::invalid_forecast,
            .validation_error = ForecastValidationError::invalid_score,
            .forecast = {},
            .used_fallback = fallback};
  }
  const auto& metadata = model.metadata();
  if (metadata.forecast_ttl_ns >
      std::numeric_limits<std::uint64_t>::max() - completed) {
    return {.status = RunStatus::invalid_forecast,
            .validation_error = ForecastValidationError::invalid_timestamp,
            .forecast = {},
            .used_fallback = fallback};
  }
  ModelForecast forecast{.forecast_id = input.forecast_id,
                         .session_id = input.feature_snapshot.session_id,
                         .model_id = metadata.model_id,
                         .model_version = metadata.model_version,
                         .instrument_id = metadata.instrument_id,
                         .feature_snapshot_id = input.feature_snapshot.snapshot_id,
                         .configuration_version = metadata.configuration_version,
                         .model_control_generation = admitted_control.generation,
                         .horizon_ns = metadata.horizon_ns,
                         .as_of_exchange_event_time_ns =
                             input.feature_snapshot.as_of_exchange_event_time_ns,
                         .production_process_monotonic_time_ns = completed,
                         .expiration_process_monotonic_time_ns =
                             completed + metadata.forecast_ttl_ns,
                         .prediction = prediction};
  forecast.stable_hash = stable_forecast_hash(forecast);
  const auto forecast_error =
      ModelForecastValidator::validate_forecast(forecast, completed);
  if (forecast_error != ForecastValidationError::none) {
    return {.status = RunStatus::invalid_forecast,
            .validation_error = forecast_error,
            .forecast = {},
            .used_fallback = fallback};
  }
  return {.status = RunStatus::accepted,
          .validation_error = ForecastValidationError::none,
          .forecast = forecast,
          .used_fallback = fallback};
}

bool LocalModelRunner::apply_control(const ModelControlUpdate update) noexcept {
  if (update.generation == 0U || update.generation > kMaximumGeneration ||
      !valid_control_state(update.state)) {
    return false;
  }
  auto observed = control_.load(std::memory_order_acquire);
  while (unpack(observed).generation < update.generation) {
    if (control_.compare_exchange_weak(observed, pack(update),
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

ModelHealthSnapshot LocalModelRunner::health() const noexcept {
  return unpack(control_.load(std::memory_order_acquire));
}

} // namespace aegis::models
