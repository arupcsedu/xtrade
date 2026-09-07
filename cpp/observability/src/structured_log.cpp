#include "aegis/observability/structured_log.hpp"

#include <bit>
#include <locale>
#include <sstream>

namespace aegis::observability {
namespace {

constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;

void mix(std::uint64_t& hash, const std::uint64_t value) noexcept {
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    hash ^= (value >> shift) & 0xFFU;
    hash *= kFnvPrime;
  }
}

template <typename Identifier>
void mix_identifier(std::uint64_t& hash, const Identifier& value) noexcept {
  mix(hash, value.high());
  mix(hash, value.low());
}

[[nodiscard]] std::uint64_t nonzero(const std::uint64_t value) noexcept {
  return value == 0U ? 1U : value;
}

} // namespace

std::string_view component_name(const Component component) noexcept {
  switch (component) {
  case Component::market_data:
    return "market_data";
  case Component::order_book:
    return "order_book";
  case Component::features:
    return "features";
  case Component::models:
    return "models";
  case Component::ensemble:
    return "ensemble";
  case Component::risk:
    return "risk";
  case Component::oms:
    return "oms";
  case Component::execution:
    return "execution";
  case Component::journal:
    return "journal";
  case Component::replay:
    return "replay";
  case Component::control_plane:
    return "control_plane";
  case Component::observability:
    return "observability";
  }
  return "invalid";
}

std::string_view severity_name(const LogSeverity severity) noexcept {
  switch (severity) {
  case LogSeverity::debug:
    return "debug";
  case LogSeverity::info:
    return "info";
  case LogSeverity::warning:
    return "warning";
  case LogSeverity::error:
    return "error";
  case LogSeverity::critical:
    return "critical";
  }
  return "invalid";
}

std::string_view log_event_name(const LogEventCode event_code) noexcept {
  switch (event_code) {
  case LogEventCode::health_transition:
    return "health_transition";
  case LogEventCode::readiness_transition:
    return "readiness_transition";
  case LogEventCode::data_quality_transition:
    return "data_quality_transition";
  case LogEventCode::clock_quality_transition:
    return "clock_quality_transition";
  case LogEventCode::model_deadline_miss:
    return "model_deadline_miss";
  case LogEventCode::decision_abstained:
    return "decision_abstained";
  case LogEventCode::risk_rejected:
    return "risk_rejected";
  case LogEventCode::order_transition:
    return "order_transition";
  case LogEventCode::gateway_rejected:
    return "gateway_rejected";
  case LogEventCode::queue_overload:
    return "queue_overload";
  case LogEventCode::journal_lag:
    return "journal_lag";
  case LogEventCode::graceful_shutdown:
    return "graceful_shutdown";
  }
  return "invalid";
}

std::uint64_t stable_structured_log_hash(StructuredLogRecord record) noexcept {
  record.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix(hash, record.schema_major);
  mix(hash, record.schema_minor);
  mix(hash, static_cast<std::uint64_t>(record.severity));
  mix(hash, static_cast<std::uint64_t>(record.component));
  mix(hash, static_cast<std::uint64_t>(record.event_code));
  mix_identifier(hash, record.correlation_id);
  mix_identifier(hash, record.session_id);
  mix_identifier(hash, record.configuration_version);
  mix(hash, record.process_monotonic_time_ns);
  mix(hash, std::bit_cast<std::uint64_t>(record.wall_clock_utc_time_ns));
  mix(hash, std::bit_cast<std::uint64_t>(record.value_1));
  mix(hash, std::bit_cast<std::uint64_t>(record.value_2));
  mix(hash, std::bit_cast<std::uint64_t>(record.value_3));
  return nonzero(hash);
}

bool valid_structured_log_record(const StructuredLogRecord& record) noexcept {
  return record.schema_major == 1U && record.schema_minor == 0U &&
         record.severity >= LogSeverity::debug &&
         record.severity <= LogSeverity::critical &&
         record.component >= Component::market_data &&
         record.component <= Component::observability &&
         record.event_code >= LogEventCode::health_transition &&
         record.event_code <= LogEventCode::graceful_shutdown &&
         record.correlation_id.valid() && record.session_id.valid() &&
         record.configuration_version.valid() &&
         record.process_monotonic_time_ns != 0U && record.wall_clock_utc_time_ns > 0 &&
         record.stable_hash != 0U &&
         record.stable_hash == stable_structured_log_hash(record);
}

bool StructuredLogPublisher::publish(const StructuredLogRecord& record) noexcept {
  if (!valid_structured_log_record(record) ||
      queue_.enqueue(record, event_bus::OverloadPolicy::reject_newest) !=
          event_bus::EnqueueStatus::accepted) {
    dropped_records_.fetch_add(1U, std::memory_order_relaxed);
    return false;
  }
  return true;
}

bool StructuredLogPublisher::consume(StructuredLogRecord& record) noexcept {
  return queue_.dequeue(record) == event_bus::DequeueStatus::item;
}

std::uint64_t StructuredLogPublisher::dropped_records() const noexcept {
  return dropped_records_.load(std::memory_order_relaxed);
}

event_bus::QueueMetrics StructuredLogPublisher::queue_metrics() const noexcept {
  return queue_.metrics();
}

// Escaped fragments keep streamed JSON field boundaries visually explicit.
// NOLINTBEGIN(modernize-raw-string-literal)
std::string JsonLogExporter::render(const StructuredLogRecord& record) {
  const auto correlation = common::to_hex(record.correlation_id);
  const auto session = common::to_hex(record.session_id);
  const auto configuration = common::to_hex(record.configuration_version);
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << "{\"schema\":\"1.0\",\"severity\":\"" << severity_name(record.severity)
         << "\",\"component\":\"" << component_name(record.component)
         << "\",\"event\":\"" << log_event_name(record.event_code)
         << "\",\"correlation_id\":\"" << correlation.data() << "\",\"session_id\":\""
         << session.data() << "\",\"configuration_version\":\"" << configuration.data()
         << "\",\"process_monotonic_time_ns\":" << record.process_monotonic_time_ns
         << ",\"wall_clock_utc_time_ns\":" << record.wall_clock_utc_time_ns
         << ",\"value_1\":" << record.value_1 << ",\"value_2\":" << record.value_2
         << ",\"value_3\":" << record.value_3
         << ",\"stable_hash\":" << record.stable_hash << '}';
  return output.str();
}
// NOLINTEND(modernize-raw-string-literal)

} // namespace aegis::observability
