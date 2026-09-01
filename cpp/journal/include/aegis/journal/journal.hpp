#ifndef AEGIS_JOURNAL_JOURNAL_HPP
#define AEGIS_JOURNAL_JOURNAL_HPP

#include "aegis/common/identifiers.hpp"
#include "aegis/common/sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace aegis::journal {

inline constexpr std::uint16_t kJournalFormatMajor = 1U;
inline constexpr std::uint16_t kJournalFormatMinor = 0U;
inline constexpr std::size_t kMaximumPersistentPayloadBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaximumHotPayloadBytes = 16U * 1024U;
inline constexpr std::size_t kAsyncQueueCapacity = 256U;

enum class RecordKind : std::uint16_t {
  unknown = 0,
  raw_packet_metadata = 1,
  normalized_market_event = 2,
  book_validity_change = 3,
  feature_snapshot = 4,
  model_forecast = 5,
  ensemble_decision = 6,
  risk_result = 7,
  order_command = 8,
  gateway_response = 9,
  fill = 10,
  position_change = 11,
  kill_switch = 12,
  configuration_change = 13,
  operator_action = 14,
  data_quality = 15,
  clock_quality = 16,
};

enum class PayloadEncoding : std::uint8_t {
  unknown = 0,
  canonical_audit_envelope = 1,
  synthetic_raw_packet_metadata = 2,
  opaque_binary = 3,
};

enum class RecordPriority : std::uint8_t {
  advisory = 1,
  mandatory = 2,
};

enum class SyncPolicy : std::uint8_t {
  none = 1,
  every_record = 2,
  periodic_records = 3,
  segment_close = 4,
};

enum class BackpressurePolicy : std::uint8_t {
  reject_newest = 1,
  fail_closed = 2,
};

enum class Status : std::uint8_t {
  ok = 0,
  invalid_argument,
  invalid_payload,
  payload_too_large,
  unsupported_format,
  target_exists,
  target_not_empty,
  not_open,
  already_open,
  io_error,
  disk_full,
  corrupt_header,
  corrupt_record,
  corrupt_index,
  truncated_tail,
  sequence_gap,
  chain_mismatch,
  queue_full,
  queue_contention,
  inhibited,
  stopped,
  deadline_exceeded,
  record_not_found,
};

enum class HealthState : std::uint8_t {
  stopped = 1,
  starting = 2,
  healthy = 3,
  degraded = 4,
  unsafe = 5,
  draining = 6,
};

struct SchemaVersion {
  std::uint16_t major{};
  std::uint16_t minor{};
  std::uint32_t patch{};
};

struct RecordMetadata {
  RecordKind kind{RecordKind::unknown};
  PayloadEncoding encoding{PayloadEncoding::unknown};
  RecordPriority priority{RecordPriority::mandatory};
  SchemaVersion schema_version{};
  std::uint64_t created_process_monotonic_time_ns{};
  std::int64_t recorded_wall_clock_utc_time_ns{};
  std::uint64_t source_event_sequence{};
  common::GlobalEventId global_event_id{};
};

struct RecordView {
  RecordMetadata metadata;
  std::span<const std::uint8_t> payload;
};

struct PersistedRecord {
  RecordMetadata metadata;
  std::uint64_t journal_sequence{};
  std::uint64_t segment_id{};
  std::uint64_t file_offset{};
  common::Sha256Digest previous_record_sha256{};
  common::Sha256Digest payload_sha256{};
  common::Sha256Digest record_sha256{};
  std::vector<std::uint8_t> payload;
};

struct WriterConfig {
  std::filesystem::path directory;
  std::uint64_t initial_sequence{1U};
  std::uint64_t maximum_segment_bytes{256U * 1024U * 1024U};
  std::uint64_t maximum_records_per_segment{1'000'000U};
  std::uint32_t index_stride_records{1'024U};
  SyncPolicy sync_policy{SyncPolicy::periodic_records};
  std::uint32_t periodic_sync_records{4'096U};
  std::uint64_t preallocate_bytes{0U};
  common::Sha256Digest configuration_sha256{};
  common::Sha256Digest build_sha256{};
  common::GlobalEventId writer_instance_id{};
  bool validate_canonical_envelopes{true};
};

struct AppendResult {
  Status status{Status::not_open};
  std::uint64_t journal_sequence{};
  common::Sha256Digest record_sha256{};
  bool durable_at_return{false};
};

struct WriterMetrics {
  std::uint64_t appended_records{};
  std::uint64_t appended_bytes{};
  std::uint64_t sync_calls{};
  std::uint64_t rotated_segments{};
  std::uint64_t io_failures{};
  std::uint64_t disk_full_failures{};
  std::uint64_t current_segment_id{};
  std::uint64_t next_sequence{1U};
};

class SegmentWriter final {
public:
  SegmentWriter();
  ~SegmentWriter();
  SegmentWriter(SegmentWriter&&) noexcept;
  SegmentWriter& operator=(SegmentWriter&&) noexcept;
  SegmentWriter(const SegmentWriter&) = delete;
  SegmentWriter& operator=(const SegmentWriter&) = delete;

  [[nodiscard]] Status open(const WriterConfig& config);
  [[nodiscard]] AppendResult append(const RecordView& record);
  [[nodiscard]] Status flush();
  [[nodiscard]] Status close();
  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] WriterMetrics metrics() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

struct ScanOptions {
  bool load_payloads{true};
  bool validate_canonical_envelopes{true};
  bool permit_recoverable_tail{true};
};

struct ScanIssue {
  Status status{Status::ok};
  std::filesystem::path path;
  std::uint64_t file_offset{};
  std::uint64_t expected_sequence{};
  std::string detail;
};

struct SegmentReport {
  Status status{Status::ok};
  std::filesystem::path path;
  std::uint64_t segment_id{};
  std::uint64_t first_sequence{};
  std::uint64_t last_sequence{};
  std::uint64_t valid_record_count{};
  std::uint64_t valid_bytes{};
  std::uint64_t file_bytes{};
  bool sealed{false};
  bool recoverable_tail{false};
  common::Sha256Digest previous_terminal_record_sha256{};
  common::Sha256Digest terminal_record_sha256{};
  std::vector<PersistedRecord> records;
  ScanIssue issue;
};

struct DirectoryReport {
  Status status{Status::ok};
  std::uint64_t first_sequence{};
  std::uint64_t last_sequence{};
  std::uint64_t valid_record_count{};
  common::Sha256Digest terminal_record_sha256{};
  std::vector<SegmentReport> segments;
  ScanIssue issue;
};

class RecoveryScanner final {
public:
  [[nodiscard]] static SegmentReport scan_segment(const std::filesystem::path& path,
                                                  const ScanOptions& options = {});

  [[nodiscard]] static DirectoryReport
  scan_directory(const std::filesystem::path& directory,
                 const ScanOptions& options = {});

  [[nodiscard]] static Status read_record(const std::filesystem::path& directory,
                                          std::uint64_t sequence,
                                          PersistedRecord& output,
                                          const ScanOptions& options = {});
};

struct RepairReport {
  Status source_status{Status::ok};
  Status target_status{Status::not_open};
  std::uint64_t copied_records{};
  std::uint64_t stopped_at_source_offset{};
  common::Sha256Digest source_terminal_sha256{};
  common::Sha256Digest target_terminal_sha256{};
};

[[nodiscard]] Status repair_copy(const std::filesystem::path& source,
                                 const WriterConfig& target_config,
                                 RepairReport& report);

using ReplayCallback = std::function<Status(const PersistedRecord&)>;

[[nodiscard]] Status replay_verified(const std::filesystem::path& source,
                                     std::uint64_t first_sequence,
                                     std::uint64_t last_sequence,
                                     const ReplayCallback& callback,
                                     std::uint64_t& replayed_records);

struct RetentionPolicy {
  bool enabled{false};
  std::uint64_t retain_at_least_segments{2U};
  std::uint64_t retain_at_least_records{1U};
  std::uint64_t minimum_age_ns{};
  std::uint64_t maximum_total_bytes{};
};

struct RetentionReport {
  Status status{Status::ok};
  std::uint64_t removed_segments{};
  std::uint64_t removed_bytes{};
  std::vector<std::filesystem::path> removed_paths;
};

// Retention only removes verified, sealed segment/index pairs and is disabled
// by default. The caller supplies wall UTC time and must durably audit the
// resulting report in a different retained journal before treating it as done.
[[nodiscard]] RetentionReport apply_retention(const std::filesystem::path& directory,
                                              const RetentionPolicy& policy,
                                              std::int64_t now_wall_clock_utc_ns);

[[nodiscard]] const char* status_name(Status status) noexcept;
[[nodiscard]] const char* record_kind_name(RecordKind kind) noexcept;
[[nodiscard]] bool is_known_record_kind(RecordKind kind) noexcept;

} // namespace aegis::journal

#endif // AEGIS_JOURNAL_JOURNAL_HPP
