#ifndef AEGIS_OBSERVABILITY_TYPES_HPP
#define AEGIS_OBSERVABILITY_TYPES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace aegis::observability {

inline constexpr std::size_t kMaximumMetricSlots = 16U;
inline constexpr std::size_t kMetricSlotCount = kMaximumMetricSlots + 1U;
inline constexpr std::uint16_t kUnscopedMetricSlot = 0U;

enum class MetricId : std::uint8_t {
  market_packets = 0,
  market_sequences,
  market_gaps,
  market_duplicates,
  market_out_of_order,
  market_feed_age_ns,
  market_book_valid,
  model_inferences,
  model_deadline_misses,
  model_freshness_ns,
  model_ood_ppm,
  model_calibration_ppm,
  model_forecast_expected_return_ppm,
  model_forecast_p10_ppm,
  model_forecast_p50_ppm,
  model_forecast_p90_ppm,
  model_forecast_up_probability_ppm,
  model_forecast_volatility_ppm,
  ensemble_weight_ppm,
  ensemble_disagreement_ppm,
  trading_intents,
  trading_risk_rejects,
  trading_orders,
  trading_cancels,
  trading_fills,
  trading_venue_rejects,
  trading_gross_exposure_currency_nanos,
  trading_net_exposure_currency_nanos,
  trading_realized_pnl_currency_nanos,
  trading_unrealized_pnl_currency_nanos,
  trading_drawdown_currency_nanos,
  trading_slippage_currency_nanos,
  trading_adverse_selection_currency_nanos,
  infrastructure_cpu_utilization_ppm,
  infrastructure_numa_remote_accesses,
  infrastructure_cache_misses,
  infrastructure_ring_occupancy,
  infrastructure_queue_drops,
  infrastructure_nic_errors,
  infrastructure_clock_offset_ns,
  infrastructure_journal_lag_events,
  observability_metric_drops,
  observability_log_drops,
  observability_trace_drops,
  count,
};

enum class MetricOperation : std::uint8_t {
  counter_add = 1,
  counter_set = 2,
  gauge_set = 3,
  histogram_observe = 4,
};

enum class LatencyStage : std::uint8_t {
  packet_receive_to_normalized = 0,
  wire_to_decode,
  decode_to_book,
  book_to_feature,
  event_to_feature,
  feature_to_model,
  inference,
  ensemble_decision,
  risk_check,
  decision_to_send,
  order_round_trip,
  journal_append,
  end_to_end,
  count,
};

inline constexpr std::size_t kMetricCount = static_cast<std::size_t>(MetricId::count);
inline constexpr std::size_t kLatencyStageCount =
    static_cast<std::size_t>(LatencyStage::count);
inline constexpr std::array<std::uint64_t, 20U> kLatencyBoundsNs{
    100U,        250U,        500U,        1'000U,       2'500U,
    5'000U,      10'000U,     25'000U,     50'000U,      100'000U,
    250'000U,    500'000U,    1'000'000U,  2'500'000U,   5'000'000U,
    10'000'000U, 25'000'000U, 50'000'000U, 100'000'000U, 1'000'000'000U};
inline constexpr std::size_t kLatencyBucketCount = kLatencyBoundsNs.size() + 1U;

// A point is the sole producer-side telemetry contract. It contains no strings,
// pointers, or dynamic ownership and is copied into a bounded preallocated queue.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct MetricPoint {
  MetricId metric{MetricId::market_packets};
  MetricOperation operation{MetricOperation::counter_add};
  LatencyStage stage{LatencyStage::packet_receive_to_normalized};
  std::uint16_t slot{kUnscopedMetricSlot};
  std::uint64_t value_bits{};
};

struct LatencyHistogram {
  std::array<std::uint64_t, kLatencyBucketCount> bucket_counts{};
  std::uint64_t count{};
  std::uint64_t sum_ns{};
  std::uint64_t maximum_ns{};
};

struct RegistrySnapshot {
  std::array<std::array<std::uint64_t, kMetricSlotCount>, kMetricCount> counters{};
  std::array<std::array<std::int64_t, kMetricSlotCount>, kMetricCount> gauges{};
  std::array<std::array<bool, kMetricSlotCount>, kMetricCount> metric_present{};
  std::array<LatencyHistogram, kLatencyStageCount> latency{};
  std::uint64_t invalid_points{};
  std::uint64_t saturated_values{};
  std::uint64_t source_queue_occupancy{};
  std::uint64_t source_queue_drops{};
};

struct InfrastructureSnapshot {
  std::int64_t cpu_utilization_ppm{};
  std::uint64_t numa_remote_accesses{};
  std::uint64_t cache_misses{};
  std::uint64_t ring_occupancy{};
  std::uint64_t queue_drops{};
  std::uint64_t nic_errors{};
  std::int64_t clock_offset_ns{};
  std::uint64_t journal_lag_events{};
};

struct ModelTelemetryContext {
  std::uint16_t model_slot{};
  std::uint64_t now_process_monotonic_time_ns{};
  std::uint64_t inference_latency_ns{};
};

struct ObservabilityDropSnapshot {
  std::uint64_t structured_log_drops{};
  std::uint64_t trace_drops{};
  std::uint64_t decision_explanation_drops{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] constexpr bool valid_metric(const MetricId metric) noexcept {
  return static_cast<std::size_t>(metric) < kMetricCount;
}

[[nodiscard]] constexpr bool valid_latency_stage(const LatencyStage stage) noexcept {
  return static_cast<std::size_t>(stage) < kLatencyStageCount;
}

[[nodiscard]] constexpr bool valid_metric_slot(const std::uint16_t slot) noexcept {
  return slot <= kMaximumMetricSlots;
}

static_assert(std::is_trivially_copyable_v<MetricPoint>);
static_assert(std::is_trivially_copyable_v<InfrastructureSnapshot>);
static_assert(std::is_trivially_copyable_v<ModelTelemetryContext>);
static_assert(std::is_trivially_copyable_v<ObservabilityDropSnapshot>);

} // namespace aegis::observability

#endif // AEGIS_OBSERVABILITY_TYPES_HPP
