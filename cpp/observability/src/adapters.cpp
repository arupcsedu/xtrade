#include "aegis/observability/adapters.hpp"

#include <algorithm>
#include <limits>

namespace aegis::observability {
namespace {

void combine(bool& accepted, const bool result) noexcept {
  accepted = result && accepted;
}

[[nodiscard]] std::uint64_t saturating_add(const std::uint64_t left,
                                           const std::uint64_t right) noexcept {
  std::uint64_t result{};
  return __builtin_add_overflow(left, right, &result)
             ? std::numeric_limits<std::uint64_t>::max()
             : result;
}

[[nodiscard]] std::int64_t bounded_signed(const std::uint64_t value) noexcept {
  constexpr auto kMaximum =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  return static_cast<std::int64_t>(std::min(value, kMaximum));
}

} // namespace

bool publish_feed_metrics(TelemetryPublisher& publisher,
                          const market_data::feed::FeedMetrics& metrics,
                          const market_data::feed::DataQualityState& quality,
                          const std::uint64_t now_process_monotonic_time_ns,
                          const bool book_valid) noexcept {
  bool accepted = true;
  combine(accepted,
          publisher.counter_set(MetricId::market_packets, metrics.packets_received));
  combine(accepted,
          publisher.counter_set(MetricId::market_sequences, metrics.normalized_events));
  combine(accepted, publisher.counter_set(MetricId::market_gaps,
                                          saturating_add(metrics.canonical_gaps,
                                                         metrics.redundant_leg_gaps)));
  combine(accepted, publisher.counter_set(MetricId::market_duplicates,
                                          metrics.duplicate_packets));
  combine(accepted,
          publisher.counter_set(MetricId::market_out_of_order,
                                saturating_add(metrics.out_of_order_packets,
                                               metrics.redundant_leg_out_of_order)));
  const auto feed_age =
      now_process_monotonic_time_ns >= quality.observed_process_monotonic_time_ns
          ? now_process_monotonic_time_ns - quality.observed_process_monotonic_time_ns
          : std::numeric_limits<std::uint64_t>::max();
  combine(accepted,
          publisher.gauge_set(MetricId::market_feed_age_ns, bounded_signed(feed_age)));
  combine(accepted,
          publisher.gauge_set(MetricId::market_book_valid, book_valid ? 1 : 0));
  combine(accepted,
          publisher.counter_set(
              MetricId::infrastructure_queue_drops,
              saturating_add(saturating_add(metrics.raw_journal_drops,
                                            metrics.normalized_publisher_drops),
                             metrics.health_publisher_drops),
              1U));
  return accepted;
}

bool publish_model_result(TelemetryPublisher& publisher,
                          const models::RunResult& result,
                          const ModelTelemetryContext& context) noexcept {
  if (!valid_metric_slot(context.model_slot) ||
      context.model_slot == kUnscopedMetricSlot) {
    return false;
  }
  bool accepted = true;
  combine(accepted,
          publisher.counter_add(MetricId::model_inferences, 1U, context.model_slot));
  if (result.status == models::RunStatus::deadline_missed ||
      result.status == models::RunStatus::late_response_discarded) {
    combine(accepted, publisher.counter_add(MetricId::model_deadline_misses, 1U,
                                            context.model_slot));
  }
  combine(accepted, publisher.observe_latency(LatencyStage::inference,
                                              context.inference_latency_ns));
  if (result.status != models::RunStatus::accepted &&
      result.status != models::RunStatus::fallback_accepted) {
    return accepted;
  }
  const auto& forecast = result.forecast;
  if (models::ModelForecastValidator::validate_forecast(
          forecast, context.now_process_monotonic_time_ns) !=
      models::ForecastValidationError::none) {
    return false;
  }
  const auto freshness = context.now_process_monotonic_time_ns >=
                                 forecast.production_process_monotonic_time_ns
                             ? context.now_process_monotonic_time_ns -
                                   forecast.production_process_monotonic_time_ns
                             : std::numeric_limits<std::uint64_t>::max();
  combine(accepted, publisher.gauge_set(MetricId::model_freshness_ns,
                                        bounded_signed(freshness), context.model_slot));
  combine(accepted,
          publisher.gauge_set(MetricId::model_ood_ppm,
                              forecast.prediction.ood_score_ppm, context.model_slot));
  combine(accepted, publisher.gauge_set(MetricId::model_calibration_ppm,
                                        forecast.prediction.calibration_score_ppm,
                                        context.model_slot));
  combine(accepted, publisher.gauge_set(MetricId::model_forecast_expected_return_ppm,
                                        forecast.prediction.expected_return_ppm,
                                        context.model_slot));
  combine(accepted,
          publisher.gauge_set(MetricId::model_forecast_p10_ppm,
                              forecast.prediction.return_p10_ppm, context.model_slot));
  combine(accepted,
          publisher.gauge_set(MetricId::model_forecast_p50_ppm,
                              forecast.prediction.return_p50_ppm, context.model_slot));
  combine(accepted,
          publisher.gauge_set(MetricId::model_forecast_p90_ppm,
                              forecast.prediction.return_p90_ppm, context.model_slot));
  combine(accepted, publisher.gauge_set(MetricId::model_forecast_up_probability_ppm,
                                        forecast.prediction.probability_up_ppm,
                                        context.model_slot));
  combine(accepted,
          publisher.gauge_set(MetricId::model_forecast_volatility_ppm,
                              bounded_signed(forecast.prediction.volatility_ppm),
                              context.model_slot));
  return accepted;
}

bool publish_ensemble_metrics(TelemetryPublisher& publisher,
                              const ensemble::EnsembleForecast& forecast) noexcept {
  if (!ensemble::valid_ensemble_forecast(forecast)) {
    return false;
  }
  bool accepted = true;
  combine(accepted, publisher.gauge_set(MetricId::ensemble_disagreement_ppm,
                                        bounded_signed(forecast.disagreement_ppm)));
  for (std::size_t index = 0U; index < forecast.explanation.supplied_expert_count &&
                               index < kMaximumMetricSlots;
       ++index) {
    combine(accepted,
            publisher.gauge_set(MetricId::ensemble_weight_ppm,
                                forecast.explanation.experts[index].weight_ppm,
                                static_cast<std::uint16_t>(index + 1U)));
  }
  return accepted;
}

bool publish_risk_metrics(TelemetryPublisher& publisher,
                          const risk::RiskMetrics& metrics) noexcept {
  bool accepted = true;
  combine(accepted,
          publisher.counter_set(MetricId::trading_intents, metrics.evaluations));
  combine(accepted,
          publisher.counter_set(MetricId::trading_risk_rejects, metrics.rejections));
  return accepted;
}

bool publish_gateway_metrics(TelemetryPublisher& publisher,
                             const execution::GatewayMetrics& metrics) noexcept {
  bool accepted = true;
  combine(accepted,
          publisher.counter_set(MetricId::trading_orders, metrics.new_orders));
  combine(accepted, publisher.counter_set(MetricId::trading_cancels, metrics.cancels));
  combine(accepted, publisher.counter_set(MetricId::trading_fills, metrics.fills));
  combine(accepted,
          publisher.counter_set(MetricId::trading_venue_rejects, metrics.rejects));
  combine(accepted,
          publisher.gauge_set(MetricId::infrastructure_ring_occupancy,
                              bounded_signed(metrics.event_queue_high_watermark), 1U));
  return accepted;
}

bool publish_portfolio_snapshot(
    TelemetryPublisher& publisher,
    const risk::portfolio::PortfolioSnapshot& snapshot) noexcept {
  if (!snapshot.risk_snapshot_id.valid() || !snapshot.session_id.valid() ||
      !snapshot.configuration_version.valid() || snapshot.stable_hash == 0U ||
      snapshot.stable_hash != risk::portfolio::stable_snapshot_hash(snapshot)) {
    return false;
  }
  bool accepted = true;
  combine(accepted,
          publisher.gauge_set(MetricId::trading_gross_exposure_currency_nanos,
                              bounded_signed(snapshot.gross_exposure_currency_nanos)));
  combine(accepted, publisher.gauge_set(MetricId::trading_net_exposure_currency_nanos,
                                        snapshot.net_exposure_currency_nanos));
  combine(accepted, publisher.gauge_set(MetricId::trading_realized_pnl_currency_nanos,
                                        snapshot.realized_pnl_currency_nanos));
  combine(accepted, publisher.gauge_set(MetricId::trading_unrealized_pnl_currency_nanos,
                                        snapshot.unrealized_pnl_currency_nanos));
  combine(accepted,
          publisher.gauge_set(MetricId::trading_drawdown_currency_nanos,
                              bounded_signed(snapshot.drawdown_currency_nanos)));
  return accepted;
}

bool publish_execution_cost(
    TelemetryPublisher& publisher,
    const execution::ExecutionCostAttribution& attribution) noexcept {
  if (!attribution.objective_id.valid() || !attribution.venue_id.valid() ||
      attribution.stable_hash == 0U ||
      attribution.stable_hash !=
          execution::stable_execution_cost_attribution_hash(attribution)) {
    return false;
  }
  bool accepted = true;
  combine(accepted, publisher.gauge_set(MetricId::trading_slippage_currency_nanos,
                                        attribution.slippage_cost_currency_nanos));
  combine(accepted,
          publisher.gauge_set(MetricId::trading_adverse_selection_currency_nanos,
                              attribution.adverse_selection_cost_currency_nanos));
  return accepted;
}

bool publish_clock_metrics(TelemetryPublisher& publisher,
                           const time::ClockQualityMetrics& metrics) noexcept {
  return publisher.gauge_set(MetricId::infrastructure_clock_offset_ns,
                             metrics.offset_ns);
}

bool publish_journal_metrics(TelemetryPublisher& publisher,
                             const journal::WriterMetrics& metrics,
                             const std::uint64_t consumed_journal_sequence) noexcept {
  const auto produced = metrics.next_sequence == 0U ? 0U : metrics.next_sequence - 1U;
  const auto lag =
      produced >= consumed_journal_sequence ? produced - consumed_journal_sequence : 0U;
  return publisher.gauge_set(MetricId::infrastructure_journal_lag_events,
                             bounded_signed(lag));
}

bool publish_infrastructure_metrics(TelemetryPublisher& publisher,
                                    const InfrastructureSnapshot& snapshot) noexcept {
  if (snapshot.cpu_utilization_ppm < 0 || snapshot.cpu_utilization_ppm > 1'000'000) {
    return false;
  }
  bool accepted = true;
  combine(accepted, publisher.gauge_set(MetricId::infrastructure_cpu_utilization_ppm,
                                        snapshot.cpu_utilization_ppm));
  combine(accepted, publisher.counter_set(MetricId::infrastructure_numa_remote_accesses,
                                          snapshot.numa_remote_accesses));
  combine(accepted, publisher.counter_set(MetricId::infrastructure_cache_misses,
                                          snapshot.cache_misses));
  combine(accepted, publisher.gauge_set(MetricId::infrastructure_ring_occupancy,
                                        bounded_signed(snapshot.ring_occupancy)));
  combine(accepted, publisher.counter_set(MetricId::infrastructure_queue_drops,
                                          snapshot.queue_drops));
  combine(accepted, publisher.counter_set(MetricId::infrastructure_nic_errors,
                                          snapshot.nic_errors));
  combine(accepted, publisher.gauge_set(MetricId::infrastructure_clock_offset_ns,
                                        snapshot.clock_offset_ns));
  combine(accepted, publisher.gauge_set(MetricId::infrastructure_journal_lag_events,
                                        bounded_signed(snapshot.journal_lag_events)));
  return accepted;
}

bool publish_observability_drops(TelemetryPublisher& publisher,
                                 const ObservabilityDropSnapshot& snapshot) noexcept {
  bool accepted = true;
  combine(accepted,
          publisher.counter_set(MetricId::observability_log_drops,
                                saturating_add(snapshot.structured_log_drops,
                                               snapshot.decision_explanation_drops)));
  combine(accepted, publisher.counter_set(MetricId::observability_trace_drops,
                                          snapshot.trace_drops));
  return accepted;
}

} // namespace aegis::observability
