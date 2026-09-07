#include "aegis/observability/telemetry.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <locale>
#include <sstream>

namespace aegis::observability {
namespace {

[[nodiscard]] std::uint64_t saturating_add(const std::uint64_t left,
                                           const std::uint64_t right,
                                           std::uint64_t& saturations) noexcept {
  std::uint64_t result{};
  if (__builtin_add_overflow(left, right, &result)) {
    if (saturations != std::numeric_limits<std::uint64_t>::max()) {
      ++saturations;
    }
    return std::numeric_limits<std::uint64_t>::max();
  }
  return result;
}

void record_invalid(RegistrySnapshot& state) noexcept {
  state.invalid_points =
      saturating_add(state.invalid_points, 1U, state.saturated_values);
}

void append_labels(std::ostringstream& output, const std::uint16_t slot) {
  if (slot != kUnscopedMetricSlot) {
    output << "{slot=\"" << slot << "\"}";
  }
}

void append_metric(std::ostringstream& output, const RegistrySnapshot& snapshot,
                   const MetricId metric) {
  const auto index = static_cast<std::size_t>(metric);
  const auto name = metric_name(metric);
  output << "# HELP " << name << ' ' << metric_help(metric) << '\n';
  output << "# TYPE " << name
         << (metric_kind(metric) == MetricKind::counter ? " counter\n" : " gauge\n");
  for (std::uint16_t slot = 0U; slot <= kMaximumMetricSlots; ++slot) {
    if (!snapshot.metric_present[index][slot]) {
      continue;
    }
    output << name;
    append_labels(output, slot);
    output << ' ';
    if (metric_kind(metric) == MetricKind::counter) {
      output << snapshot.counters[index][slot];
    } else {
      output << snapshot.gauges[index][slot];
    }
    output << '\n';
  }
}

void append_latency(std::ostringstream& output, const LatencyStage stage,
                    const LatencyHistogram& histogram) {
  const auto stage_name = latency_stage_name(stage);
  std::uint64_t cumulative = 0U;
  for (std::size_t index = 0U; index < kLatencyBoundsNs.size(); ++index) {
    cumulative += histogram.bucket_counts[index];
    output << "aegis_latency_ns_bucket{stage=\"" << stage_name << "\",le=\""
           << kLatencyBoundsNs[index] << "\"} " << cumulative << '\n';
  }
  cumulative += histogram.bucket_counts.back();
  output << "aegis_latency_ns_bucket{stage=\"" << stage_name << R"(",le="+Inf"} )"
         << cumulative << '\n';
  output << "aegis_latency_ns_count{stage=\"" << stage_name << "\"} " << histogram.count
         << '\n';
  output << "aegis_latency_ns_sum{stage=\"" << stage_name << "\"} " << histogram.sum_ns
         << '\n';
  for (const auto quantile : {500U, 950U, 990U, 999U}) {
    output << "aegis_latency_quantile_ns{stage=\"" << stage_name << "\",quantile=\""
           << quantile << "\"} " << histogram_quantile_ns(histogram, quantile) << '\n';
  }
  output << "aegis_latency_max_ns{stage=\"" << stage_name << "\"} "
         << histogram.maximum_ns << '\n';
}

} // namespace

std::string_view metric_name(const MetricId metric) noexcept {
  switch (metric) {
  case MetricId::market_packets:
    return "aegis_market_data_packets_total";
  case MetricId::market_sequences:
    return "aegis_market_data_sequences_total";
  case MetricId::market_gaps:
    return "aegis_market_data_gaps_total";
  case MetricId::market_duplicates:
    return "aegis_market_data_duplicates_total";
  case MetricId::market_out_of_order:
    return "aegis_market_data_out_of_order_total";
  case MetricId::market_feed_age_ns:
    return "aegis_market_data_feed_age_ns";
  case MetricId::market_book_valid:
    return "aegis_market_data_book_valid";
  case MetricId::model_inferences:
    return "aegis_model_inferences_total";
  case MetricId::model_deadline_misses:
    return "aegis_model_deadline_misses_total";
  case MetricId::model_freshness_ns:
    return "aegis_model_freshness_ns";
  case MetricId::model_ood_ppm:
    return "aegis_model_ood_ppm";
  case MetricId::model_calibration_ppm:
    return "aegis_model_calibration_ppm";
  case MetricId::model_forecast_expected_return_ppm:
    return "aegis_model_forecast_expected_return_ppm";
  case MetricId::model_forecast_p10_ppm:
    return "aegis_model_forecast_p10_ppm";
  case MetricId::model_forecast_p50_ppm:
    return "aegis_model_forecast_p50_ppm";
  case MetricId::model_forecast_p90_ppm:
    return "aegis_model_forecast_p90_ppm";
  case MetricId::model_forecast_up_probability_ppm:
    return "aegis_model_forecast_up_probability_ppm";
  case MetricId::model_forecast_volatility_ppm:
    return "aegis_model_forecast_volatility_ppm";
  case MetricId::ensemble_weight_ppm:
    return "aegis_ensemble_weight_ppm";
  case MetricId::ensemble_disagreement_ppm:
    return "aegis_ensemble_disagreement_ppm";
  case MetricId::trading_intents:
    return "aegis_trading_intents_total";
  case MetricId::trading_risk_rejects:
    return "aegis_trading_risk_rejects_total";
  case MetricId::trading_orders:
    return "aegis_trading_orders_total";
  case MetricId::trading_cancels:
    return "aegis_trading_cancels_total";
  case MetricId::trading_fills:
    return "aegis_trading_fills_total";
  case MetricId::trading_venue_rejects:
    return "aegis_trading_venue_rejects_total";
  case MetricId::trading_gross_exposure_currency_nanos:
    return "aegis_trading_gross_exposure_currency_nanos";
  case MetricId::trading_net_exposure_currency_nanos:
    return "aegis_trading_net_exposure_currency_nanos";
  case MetricId::trading_realized_pnl_currency_nanos:
    return "aegis_trading_realized_pnl_currency_nanos";
  case MetricId::trading_unrealized_pnl_currency_nanos:
    return "aegis_trading_unrealized_pnl_currency_nanos";
  case MetricId::trading_drawdown_currency_nanos:
    return "aegis_trading_drawdown_currency_nanos";
  case MetricId::trading_slippage_currency_nanos:
    return "aegis_trading_slippage_currency_nanos";
  case MetricId::trading_adverse_selection_currency_nanos:
    return "aegis_trading_adverse_selection_currency_nanos";
  case MetricId::infrastructure_cpu_utilization_ppm:
    return "aegis_infrastructure_cpu_utilization_ppm";
  case MetricId::infrastructure_numa_remote_accesses:
    return "aegis_infrastructure_numa_remote_accesses_total";
  case MetricId::infrastructure_cache_misses:
    return "aegis_infrastructure_cache_misses_total";
  case MetricId::infrastructure_ring_occupancy:
    return "aegis_infrastructure_ring_occupancy";
  case MetricId::infrastructure_queue_drops:
    return "aegis_infrastructure_queue_drops_total";
  case MetricId::infrastructure_nic_errors:
    return "aegis_infrastructure_nic_errors_total";
  case MetricId::infrastructure_clock_offset_ns:
    return "aegis_infrastructure_clock_offset_ns";
  case MetricId::infrastructure_journal_lag_events:
    return "aegis_infrastructure_journal_lag_events";
  case MetricId::observability_metric_drops:
    return "aegis_observability_metric_drops_total";
  case MetricId::observability_log_drops:
    return "aegis_observability_log_drops_total";
  case MetricId::observability_trace_drops:
    return "aegis_observability_trace_drops_total";
  case MetricId::count:
    break;
  }
  return "aegis_invalid_metric";
}

std::string_view metric_help(const MetricId metric) noexcept {
  switch (metric_kind(metric)) {
  case MetricKind::counter:
    return "Monotonic Aegis-MX event count.";
  case MetricKind::gauge:
    return "Current Aegis-MX fixed-unit value.";
  }
  return "Invalid Aegis-MX metric.";
}

MetricKind metric_kind(const MetricId metric) noexcept {
  switch (metric) {
  case MetricId::market_packets:
  case MetricId::market_sequences:
  case MetricId::market_gaps:
  case MetricId::market_duplicates:
  case MetricId::market_out_of_order:
  case MetricId::model_inferences:
  case MetricId::model_deadline_misses:
  case MetricId::trading_intents:
  case MetricId::trading_risk_rejects:
  case MetricId::trading_orders:
  case MetricId::trading_cancels:
  case MetricId::trading_fills:
  case MetricId::trading_venue_rejects:
  case MetricId::infrastructure_numa_remote_accesses:
  case MetricId::infrastructure_cache_misses:
  case MetricId::infrastructure_queue_drops:
  case MetricId::infrastructure_nic_errors:
  case MetricId::observability_metric_drops:
  case MetricId::observability_log_drops:
  case MetricId::observability_trace_drops:
    return MetricKind::counter;
  default:
    return MetricKind::gauge;
  }
}

std::string_view latency_stage_name(const LatencyStage stage) noexcept {
  switch (stage) {
  case LatencyStage::packet_receive_to_normalized:
    return "packet_receive_to_normalized";
  case LatencyStage::wire_to_decode:
    return "wire_to_decode";
  case LatencyStage::decode_to_book:
    return "decode_to_book";
  case LatencyStage::book_to_feature:
    return "book_to_feature";
  case LatencyStage::event_to_feature:
    return "event_to_feature";
  case LatencyStage::feature_to_model:
    return "feature_to_model";
  case LatencyStage::inference:
    return "inference";
  case LatencyStage::ensemble_decision:
    return "ensemble_decision";
  case LatencyStage::risk_check:
    return "risk_check";
  case LatencyStage::decision_to_send:
    return "decision_to_send";
  case LatencyStage::order_round_trip:
    return "order_round_trip";
  case LatencyStage::journal_append:
    return "journal_append";
  case LatencyStage::end_to_end:
    return "end_to_end";
  case LatencyStage::count:
    break;
  }
  return "invalid";
}

bool TelemetryPublisher::publish(const MetricPoint& point) noexcept {
  const auto valid_operation = point.operation >= MetricOperation::counter_add &&
                               point.operation <= MetricOperation::histogram_observe;
  const auto compatible_operation =
      point.operation == MetricOperation::histogram_observe ||
      (metric_kind(point.metric) == MetricKind::counter &&
       (point.operation == MetricOperation::counter_add ||
        point.operation == MetricOperation::counter_set)) ||
      (metric_kind(point.metric) == MetricKind::gauge &&
       point.operation == MetricOperation::gauge_set);
  if (!valid_metric(point.metric) || !valid_metric_slot(point.slot) ||
      !valid_operation || !compatible_operation ||
      (point.operation == MetricOperation::histogram_observe &&
       !valid_latency_stage(point.stage))) {
    dropped_points_.fetch_add(1U, std::memory_order_relaxed);
    return false;
  }
  const auto status = queue_.enqueue(point, event_bus::OverloadPolicy::reject_newest);
  if (status != event_bus::EnqueueStatus::accepted) {
    dropped_points_.fetch_add(1U, std::memory_order_relaxed);
    return false;
  }
  return true;
}

bool TelemetryPublisher::counter_add(const MetricId metric, const std::uint64_t delta,
                                     const std::uint16_t slot) noexcept {
  return publish({.metric = metric,
                  .operation = MetricOperation::counter_add,
                  .slot = slot,
                  .value_bits = delta});
}

bool TelemetryPublisher::counter_set(const MetricId metric, const std::uint64_t value,
                                     const std::uint16_t slot) noexcept {
  return publish({.metric = metric,
                  .operation = MetricOperation::counter_set,
                  .slot = slot,
                  .value_bits = value});
}

bool TelemetryPublisher::gauge_set(const MetricId metric, const std::int64_t value,
                                   const std::uint16_t slot) noexcept {
  return publish({.metric = metric,
                  .operation = MetricOperation::gauge_set,
                  .slot = slot,
                  .value_bits = std::bit_cast<std::uint64_t>(value)});
}

bool TelemetryPublisher::observe_latency(const LatencyStage stage,
                                         const std::uint64_t duration_ns) noexcept {
  return publish({.metric = MetricId::market_packets,
                  .operation = MetricOperation::histogram_observe,
                  .stage = stage,
                  .value_bits = duration_ns});
}

std::uint64_t TelemetryPublisher::dropped_points() const noexcept {
  return dropped_points_.load(std::memory_order_relaxed);
}

event_bus::QueueMetrics TelemetryPublisher::queue_metrics() const noexcept {
  return queue_.metrics();
}

void MetricRegistry::apply(const MetricPoint& point) noexcept {
  if (!valid_metric(point.metric) || !valid_metric_slot(point.slot)) {
    record_invalid(state_);
    return;
  }
  if (point.operation == MetricOperation::histogram_observe) {
    if (!valid_latency_stage(point.stage)) {
      record_invalid(state_);
      return;
    }
    auto& histogram = state_.latency[static_cast<std::size_t>(point.stage)];
    const auto value = point.value_bits;
    const auto* const position = std::ranges::lower_bound(kLatencyBoundsNs, value);
    const auto bucket = static_cast<std::size_t>(position - kLatencyBoundsNs.begin());
    histogram.bucket_counts[bucket] =
        saturating_add(histogram.bucket_counts[bucket], 1U, state_.saturated_values);
    histogram.count = saturating_add(histogram.count, 1U, state_.saturated_values);
    histogram.sum_ns = saturating_add(histogram.sum_ns, value, state_.saturated_values);
    histogram.maximum_ns = std::max(histogram.maximum_ns, value);
    return;
  }

  const auto metric_index = static_cast<std::size_t>(point.metric);
  if (metric_kind(point.metric) == MetricKind::counter &&
      point.operation != MetricOperation::counter_add &&
      point.operation != MetricOperation::counter_set) {
    record_invalid(state_);
    return;
  }
  if (metric_kind(point.metric) == MetricKind::gauge &&
      point.operation != MetricOperation::gauge_set) {
    record_invalid(state_);
    return;
  }
  state_.metric_present[metric_index][point.slot] = true;
  if (point.operation == MetricOperation::gauge_set) {
    state_.gauges[metric_index][point.slot] =
        std::bit_cast<std::int64_t>(point.value_bits);
  } else if (point.operation == MetricOperation::counter_set) {
    state_.counters[metric_index][point.slot] = point.value_bits;
  } else {
    state_.counters[metric_index][point.slot] =
        saturating_add(state_.counters[metric_index][point.slot], point.value_bits,
                       state_.saturated_values);
  }
}

RegistrySnapshot MetricRegistry::snapshot() const noexcept { return state_; }

TelemetryProcessor::TelemetryProcessor(TelemetryPublisher& publisher) noexcept
    : publisher_(publisher) {}

std::size_t TelemetryProcessor::drain(const std::size_t maximum_points) noexcept {
  std::size_t count = 0U;
  MetricPoint point;
  while (count < maximum_points) {
    const auto status = publisher_.queue_.dequeue(point);
    if (status != event_bus::DequeueStatus::item) {
      break;
    }
    registry_.apply(point);
    ++count;
  }
  return count;
}

RegistrySnapshot TelemetryProcessor::snapshot() const noexcept {
  auto result = registry_.snapshot();
  const auto metrics = publisher_.queue_metrics();
  result.source_queue_occupancy = metrics.consumer_lag_events;
  result.source_queue_drops = publisher_.dropped_points();
  const auto drop_index =
      static_cast<std::size_t>(MetricId::observability_metric_drops);
  result.counters[drop_index][kUnscopedMetricSlot] = result.source_queue_drops;
  result.metric_present[drop_index][kUnscopedMetricSlot] = true;
  return result;
}

std::uint64_t histogram_quantile_ns(const LatencyHistogram& histogram,
                                    const std::uint32_t quantile_millis) noexcept {
  if (histogram.count == 0U || quantile_millis == 0U || quantile_millis > 1'000U) {
    return 0U;
  }
  const auto quotient = histogram.count / 1'000U;
  const auto remainder = histogram.count % 1'000U;
  const auto target =
      (quotient * quantile_millis) + (((remainder * quantile_millis) + 999U) / 1'000U);
  std::uint64_t cumulative = 0U;
  for (std::size_t index = 0U; index < kLatencyBoundsNs.size(); ++index) {
    cumulative += histogram.bucket_counts[index];
    if (cumulative >= target) {
      return kLatencyBoundsNs[index];
    }
  }
  return histogram.maximum_ns;
}

std::string PrometheusExporter::render(const RegistrySnapshot& snapshot) {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  for (std::size_t index = 0U; index < kMetricCount; ++index) {
    append_metric(output, snapshot, static_cast<MetricId>(index));
  }
  output
      << "# HELP aegis_latency_ns Fixed-stage latency histogram in nanoseconds.\n"
      << "# TYPE aegis_latency_ns histogram\n"
      << "# HELP aegis_latency_quantile_ns Derived fixed-bucket latency quantile in "
         "nanoseconds.\n"
      << "# TYPE aegis_latency_quantile_ns gauge\n"
      << "# HELP aegis_latency_max_ns Maximum observed stage latency in nanoseconds.\n"
      << "# TYPE aegis_latency_max_ns gauge\n";
  for (std::size_t index = 0U; index < kLatencyStageCount; ++index) {
    append_latency(output, static_cast<LatencyStage>(index), snapshot.latency[index]);
  }
  output << "# HELP aegis_observability_invalid_points_total Rejected consumer-side "
            "metric points.\n"
         << "# TYPE aegis_observability_invalid_points_total counter\n"
         << "aegis_observability_invalid_points_total " << snapshot.invalid_points
         << '\n'
         << "# HELP aegis_observability_saturated_values_total Saturating arithmetic "
            "events in aggregation.\n"
         << "# TYPE aegis_observability_saturated_values_total counter\n"
         << "aegis_observability_saturated_values_total " << snapshot.saturated_values
         << '\n'
         << "# HELP aegis_observability_queue_occupancy Pending points in the bounded "
            "metric queue.\n"
         << "# TYPE aegis_observability_queue_occupancy gauge\n"
         << "aegis_observability_queue_occupancy " << snapshot.source_queue_occupancy
         << '\n';
  return output.str();
}

} // namespace aegis::observability
