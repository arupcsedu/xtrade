#ifndef AEGIS_OBSERVABILITY_TELEMETRY_HPP
#define AEGIS_OBSERVABILITY_TELEMETRY_HPP

#include "aegis/event_bus/mpsc_queue.hpp"
#include "aegis/observability/types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace aegis::observability {

inline constexpr std::size_t kTelemetryQueueCapacity = 4'096U;

enum class MetricKind : std::uint8_t { counter = 1, gauge = 2 };

[[nodiscard]] std::string_view metric_name(MetricId metric) noexcept;
[[nodiscard]] std::string_view metric_help(MetricId metric) noexcept;
[[nodiscard]] MetricKind metric_kind(MetricId metric) noexcept;
[[nodiscard]] std::string_view latency_stage_name(LatencyStage stage) noexcept;

class TelemetryPublisher final {
public:
  // This operation is bounded and never waits for capacity. A false return is
  // an explicit telemetry loss; trading behavior must not depend on the result.
  [[nodiscard]] bool publish(const MetricPoint& point) noexcept;
  [[nodiscard]] bool counter_add(MetricId metric, std::uint64_t delta = 1U,
                                 std::uint16_t slot = kUnscopedMetricSlot) noexcept;
  [[nodiscard]] bool counter_set(MetricId metric, std::uint64_t value,
                                 std::uint16_t slot = kUnscopedMetricSlot) noexcept;
  [[nodiscard]] bool gauge_set(MetricId metric, std::int64_t value,
                               std::uint16_t slot = kUnscopedMetricSlot) noexcept;
  [[nodiscard]] bool observe_latency(LatencyStage stage,
                                     std::uint64_t duration_ns) noexcept;
  [[nodiscard]] std::uint64_t dropped_points() const noexcept;
  [[nodiscard]] event_bus::QueueMetrics queue_metrics() const noexcept;

private:
  friend class TelemetryProcessor;
  event_bus::MpscQueue<MetricPoint, kTelemetryQueueCapacity> queue_;
  std::atomic<std::uint64_t> dropped_points_{0U};
};

class MetricRegistry final {
public:
  void apply(const MetricPoint& point) noexcept;
  [[nodiscard]] RegistrySnapshot snapshot() const noexcept;

private:
  RegistrySnapshot state_{};
};

class TelemetryProcessor final {
public:
  explicit TelemetryProcessor(TelemetryPublisher& publisher) noexcept;
  [[nodiscard]] std::size_t drain(std::size_t maximum_points) noexcept;
  [[nodiscard]] RegistrySnapshot snapshot() const noexcept;

private:
  TelemetryPublisher& publisher_;
  MetricRegistry registry_;
};

class PrometheusExporter final {
public:
  // Text rendering allocates and is deliberately consumer-side only.
  [[nodiscard]] static std::string render(const RegistrySnapshot& snapshot);
};

[[nodiscard]] std::uint64_t
histogram_quantile_ns(const LatencyHistogram& histogram,
                      std::uint32_t quantile_millis) noexcept;

} // namespace aegis::observability

#endif // AEGIS_OBSERVABILITY_TELEMETRY_HPP
