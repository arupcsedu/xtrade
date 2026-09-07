#ifndef AEGIS_OBSERVABILITY_TRACING_HPP
#define AEGIS_OBSERVABILITY_TRACING_HPP

#include "aegis/observability/structured_log.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace aegis::observability {

inline constexpr std::size_t kTraceQueueCapacity = 1'024U;

enum class TraceOperation : std::uint8_t {
  normalize_market_event = 1,
  publish_feature_snapshot = 2,
  asynchronous_model_inference = 3,
  control_plane_fetch = 4,
  model_registry_fetch = 5,
  journal_export = 6,
  replay_run = 7,
};

enum class TraceStatus : std::uint8_t { ok = 1, error = 2, deadline_exceeded = 3 };

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct TraceSpanRecord {
  std::uint16_t schema_major{1U};
  std::uint16_t schema_minor{0U};
  common::GlobalEventId trace_id;
  std::uint64_t span_id{};
  std::uint64_t parent_span_id{};
  common::GlobalEventId correlation_id;
  common::SessionId session_id;
  common::ConfigurationVersion configuration_version;
  Component component{Component::observability};
  TraceOperation operation{TraceOperation::normalize_market_event};
  TraceStatus status{TraceStatus::ok};
  std::uint64_t start_process_monotonic_time_ns{};
  std::uint64_t end_process_monotonic_time_ns{};
  std::int64_t start_wall_clock_utc_time_ns{};
  std::int64_t end_wall_clock_utc_time_ns{};
  std::uint64_t stable_hash{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] std::string_view trace_operation_name(TraceOperation operation) noexcept;
[[nodiscard]] std::uint64_t stable_trace_span_hash(TraceSpanRecord record) noexcept;
[[nodiscard]] bool valid_trace_span(const TraceSpanRecord& record) noexcept;

class TracePublisher final {
public:
  // This API is named to make its placement constraint visible at the call
  // site. Distributed tracing is forbidden in strict packet/order hot loops.
  [[nodiscard]] bool publish_off_path(const TraceSpanRecord& record) noexcept;
  [[nodiscard]] bool consume(TraceSpanRecord& record) noexcept;
  [[nodiscard]] std::uint64_t dropped_spans() const noexcept;

private:
  event_bus::MpscQueue<TraceSpanRecord, kTraceQueueCapacity> queue_;
  std::atomic<std::uint64_t> dropped_spans_{0U};
};

class OtlpJsonExporter final {
public:
  // Produces an OTLP/HTTP JSON request body. Network transmission belongs to a
  // separate asynchronous worker and is not part of this library.
  [[nodiscard]] static std::string render(const TraceSpanRecord& record);
};

static_assert(std::is_trivially_copyable_v<TraceSpanRecord>);

} // namespace aegis::observability

#endif // AEGIS_OBSERVABILITY_TRACING_HPP
