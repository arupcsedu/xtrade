#ifndef AEGIS_OBSERVABILITY_STRUCTURED_LOG_HPP
#define AEGIS_OBSERVABILITY_STRUCTURED_LOG_HPP

#include "aegis/common/identifiers.hpp"
#include "aegis/event_bus/mpsc_queue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace aegis::observability {

inline constexpr std::size_t kStructuredLogQueueCapacity = 1'024U;

enum class Component : std::uint8_t {
  market_data = 1,
  order_book = 2,
  features = 3,
  models = 4,
  ensemble = 5,
  risk = 6,
  oms = 7,
  execution = 8,
  journal = 9,
  replay = 10,
  control_plane = 11,
  observability = 12,
};

enum class LogSeverity : std::uint8_t {
  debug = 1,
  info = 2,
  warning = 3,
  error = 4,
  critical = 5,
};

// Event codes are fixed rather than caller-controlled text. This prevents
// untrusted documents, secrets, and high-cardinality values entering logs.
enum class LogEventCode : std::uint8_t {
  health_transition = 1,
  readiness_transition = 2,
  data_quality_transition = 3,
  clock_quality_transition = 4,
  model_deadline_miss = 5,
  decision_abstained = 6,
  risk_rejected = 7,
  order_transition = 8,
  gateway_rejected = 9,
  queue_overload = 10,
  journal_lag = 11,
  graceful_shutdown = 12,
};

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct StructuredLogRecord {
  std::uint16_t schema_major{1U};
  std::uint16_t schema_minor{0U};
  LogSeverity severity{LogSeverity::info};
  Component component{Component::observability};
  LogEventCode event_code{LogEventCode::health_transition};
  common::GlobalEventId correlation_id;
  common::SessionId session_id;
  common::ConfigurationVersion configuration_version;
  std::uint64_t process_monotonic_time_ns{};
  std::int64_t wall_clock_utc_time_ns{};
  std::int64_t value_1{};
  std::int64_t value_2{};
  std::int64_t value_3{};
  std::uint64_t stable_hash{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] bool
valid_structured_log_record(const StructuredLogRecord& record) noexcept;
[[nodiscard]] std::uint64_t
stable_structured_log_hash(StructuredLogRecord record) noexcept;
[[nodiscard]] std::string_view component_name(Component component) noexcept;
[[nodiscard]] std::string_view severity_name(LogSeverity severity) noexcept;
[[nodiscard]] std::string_view log_event_name(LogEventCode event_code) noexcept;

class StructuredLogPublisher final {
public:
  [[nodiscard]] bool publish(const StructuredLogRecord& record) noexcept;
  [[nodiscard]] bool consume(StructuredLogRecord& record) noexcept;
  [[nodiscard]] std::uint64_t dropped_records() const noexcept;
  [[nodiscard]] event_bus::QueueMetrics queue_metrics() const noexcept;

private:
  event_bus::MpscQueue<StructuredLogRecord, kStructuredLogQueueCapacity> queue_;
  std::atomic<std::uint64_t> dropped_records_{0U};
};

class JsonLogExporter final {
public:
  [[nodiscard]] static std::string render(const StructuredLogRecord& record);
};

static_assert(std::is_trivially_copyable_v<StructuredLogRecord>);

} // namespace aegis::observability

#endif // AEGIS_OBSERVABILITY_STRUCTURED_LOG_HPP
