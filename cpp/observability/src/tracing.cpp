#include "aegis/observability/tracing.hpp"

#include <bit>
#include <iomanip>
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

[[nodiscard]] std::string span_hex(const std::uint64_t value) {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::hex << std::setfill('0') << std::setw(16) << value;
  return output.str();
}

} // namespace

std::string_view trace_operation_name(const TraceOperation operation) noexcept {
  switch (operation) {
  case TraceOperation::normalize_market_event:
    return "normalize_market_event";
  case TraceOperation::publish_feature_snapshot:
    return "publish_feature_snapshot";
  case TraceOperation::asynchronous_model_inference:
    return "asynchronous_model_inference";
  case TraceOperation::control_plane_fetch:
    return "control_plane_fetch";
  case TraceOperation::model_registry_fetch:
    return "model_registry_fetch";
  case TraceOperation::journal_export:
    return "journal_export";
  case TraceOperation::replay_run:
    return "replay_run";
  }
  return "invalid";
}

std::uint64_t stable_trace_span_hash(TraceSpanRecord record) noexcept {
  record.stable_hash = 0U;
  std::uint64_t hash = kFnvOffset;
  mix(hash, record.schema_major);
  mix(hash, record.schema_minor);
  mix_identifier(hash, record.trace_id);
  mix(hash, record.span_id);
  mix(hash, record.parent_span_id);
  mix_identifier(hash, record.correlation_id);
  mix_identifier(hash, record.session_id);
  mix_identifier(hash, record.configuration_version);
  mix(hash, static_cast<std::uint64_t>(record.component));
  mix(hash, static_cast<std::uint64_t>(record.operation));
  mix(hash, static_cast<std::uint64_t>(record.status));
  mix(hash, record.start_process_monotonic_time_ns);
  mix(hash, record.end_process_monotonic_time_ns);
  mix(hash, std::bit_cast<std::uint64_t>(record.start_wall_clock_utc_time_ns));
  mix(hash, std::bit_cast<std::uint64_t>(record.end_wall_clock_utc_time_ns));
  return hash == 0U ? 1U : hash;
}

bool valid_trace_span(const TraceSpanRecord& record) noexcept {
  return record.schema_major == 1U && record.schema_minor == 0U &&
         record.trace_id.valid() && record.span_id != 0U &&
         record.correlation_id.valid() && record.session_id.valid() &&
         record.configuration_version.valid() &&
         record.component >= Component::market_data &&
         record.component <= Component::observability &&
         record.operation >= TraceOperation::normalize_market_event &&
         record.operation <= TraceOperation::replay_run &&
         record.status >= TraceStatus::ok &&
         record.status <= TraceStatus::deadline_exceeded &&
         record.start_process_monotonic_time_ns != 0U &&
         record.end_process_monotonic_time_ns >=
             record.start_process_monotonic_time_ns &&
         record.start_wall_clock_utc_time_ns > 0 &&
         record.end_wall_clock_utc_time_ns >= record.start_wall_clock_utc_time_ns &&
         record.stable_hash != 0U &&
         record.stable_hash == stable_trace_span_hash(record);
}

bool TracePublisher::publish_off_path(const TraceSpanRecord& record) noexcept {
  if (!valid_trace_span(record) ||
      queue_.enqueue(record, event_bus::OverloadPolicy::reject_newest) !=
          event_bus::EnqueueStatus::accepted) {
    dropped_spans_.fetch_add(1U, std::memory_order_relaxed);
    return false;
  }
  return true;
}

bool TracePublisher::consume(TraceSpanRecord& record) noexcept {
  return queue_.dequeue(record) == event_bus::DequeueStatus::item;
}

std::uint64_t TracePublisher::dropped_spans() const noexcept {
  return dropped_spans_.load(std::memory_order_relaxed);
}

// Escaped fragments keep the nested OTLP field boundaries explicit.
// NOLINTBEGIN(modernize-raw-string-literal)
std::string OtlpJsonExporter::render(const TraceSpanRecord& record) {
  const auto trace = common::to_hex(record.trace_id);
  const auto correlation = common::to_hex(record.correlation_id);
  const auto configuration = common::to_hex(record.configuration_version);
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << "{\"resourceSpans\":[{\"resource\":{\"attributes\":[{\"key\":"
            "\"service.name\",\"value\":{\"stringValue\":\"aegis-"
         << component_name(record.component)
         << "\"}}]},\"scopeSpans\":[{\"scope\":{\"name\":\"aegis-mx\"},"
            "\"spans\":[{\"traceId\":\""
         << trace.data() << "\",\"spanId\":\"" << span_hex(record.span_id)
         << "\",\"parentSpanId\":\"" << span_hex(record.parent_span_id)
         << "\",\"name\":\"" << trace_operation_name(record.operation)
         << "\",\"startTimeUnixNano\":\"" << record.start_wall_clock_utc_time_ns
         << "\",\"endTimeUnixNano\":\"" << record.end_wall_clock_utc_time_ns
         << "\",\"attributes\":[{\"key\":\"aegis.correlation_id\",\"value\":{"
            "\"stringValue\":\""
         << correlation.data()
         << "\"}},{\"key\":\"aegis.configuration_version\",\"value\":{"
            "\"stringValue\":\""
         << configuration.data()
         << "\"}}],\"status\":{\"code\":" << (record.status == TraceStatus::ok ? 1 : 2)
         << "}}]}]}]}";
  return output.str();
}
// NOLINTEND(modernize-raw-string-literal)

} // namespace aegis::observability
