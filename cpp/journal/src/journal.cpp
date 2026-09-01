#include "aegis/journal/journal.hpp"

#include "aegis/common/audit_envelope.hpp"

#include "format.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

namespace aegis::journal {
namespace {

using detail::IndexEntryFields;
using detail::IndexHeaderFields;
using detail::RecordHeaderFields;
using detail::SegmentFooterFields;
using detail::SegmentHeaderFields;

[[nodiscard]] Status errno_status(const int error) noexcept {
  return error == ENOSPC || error == EDQUOT ? Status::disk_full : Status::io_error;
}

[[nodiscard]] std::uint64_t checked_file_size(const struct stat& info) noexcept {
  return info.st_size < 0 ? 0U : static_cast<std::uint64_t>(info.st_size);
}

[[nodiscard]] bool is_segment_path(const std::filesystem::path& path) {
  const auto name = path.filename().string();
  return name.starts_with("journal-") &&
         (name.ends_with(".ajl") ||
          (name.ends_with(".open") && !name.ends_with(".idx.open")));
}

[[nodiscard]] bool is_sealed_path(const std::filesystem::path& path) {
  return path.extension() == ".ajl";
}

[[nodiscard]] std::filesystem::path segment_path(const std::filesystem::path& directory,
                                                 const std::uint64_t segment_id,
                                                 const std::string_view extension) {
  std::array<char, 64> name{};
  const auto result =
      std::snprintf(name.data(), name.size(), "journal-%020llu.%.*s",
                    static_cast<unsigned long long>(segment_id),
                    static_cast<int>(extension.size()), extension.data());
  if (result <= 0 || static_cast<std::size_t>(result) >= name.size()) {
    return {};
  }
  return directory / name.data();
}

[[nodiscard]] Status write_all(const int descriptor,
                               const std::span<const std::uint8_t> bytes) noexcept {
  std::size_t offset{};
  while (offset < bytes.size()) {
    const auto written =
        ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return errno_status(errno);
    }
    if (written == 0) {
      return Status::io_error;
    }
    offset += static_cast<std::size_t>(written);
  }
  return Status::ok;
}

[[nodiscard]] Status pwrite_all(const int descriptor,
                                const std::span<const std::uint8_t> bytes,
                                std::uint64_t offset) noexcept {
  std::size_t consumed{};
  while (consumed < bytes.size()) {
    const auto written =
        ::pwrite(descriptor, bytes.data() + consumed, bytes.size() - consumed,
                 static_cast<off_t>(offset + consumed));
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return errno_status(errno);
    }
    if (written == 0) {
      return Status::io_error;
    }
    consumed += static_cast<std::size_t>(written);
  }
  return Status::ok;
}

[[nodiscard]] Status pread_all(const int descriptor,
                               const std::span<std::uint8_t> bytes,
                               const std::uint64_t offset) noexcept {
  std::size_t consumed{};
  while (consumed < bytes.size()) {
    const auto count =
        ::pread(descriptor, bytes.data() + consumed, bytes.size() - consumed,
                static_cast<off_t>(offset + consumed));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return errno_status(errno);
    }
    if (count == 0) {
      return Status::truncated_tail;
    }
    consumed += static_cast<std::size_t>(count);
  }
  return Status::ok;
}

[[nodiscard]] Status write_frame(const int descriptor,
                                 const std::span<const std::uint8_t> header,
                                 const std::span<const std::uint8_t> payload,
                                 const std::span<const std::uint8_t> trailer) noexcept {
  // POSIX inherited a mutable iov_base even though writev does not modify buffers.
  std::array<iovec, 3> vectors{{
      {.iov_base = const_cast<std::uint8_t*>(header.data()), // NOLINT
       .iov_len = header.size()},
      {.iov_base = const_cast<std::uint8_t*>(payload.data()), // NOLINT
       .iov_len = payload.size()},
      {.iov_base = const_cast<std::uint8_t*>(trailer.data()), // NOLINT
       .iov_len = trailer.size()},
  }};
  std::size_t current{};
  while (current < vectors.size()) {
    const auto count = ::writev(descriptor, vectors.data() + current,
                                static_cast<int>(vectors.size() - current));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return errno_status(errno);
    }
    if (count == 0) {
      return Status::io_error;
    }
    auto remaining = static_cast<std::size_t>(count);
    while (current < vectors.size() && remaining >= vectors[current].iov_len) {
      remaining -= vectors[current].iov_len;
      ++current;
    }
    if (current < vectors.size() && remaining != 0U) {
      auto* base = static_cast<std::uint8_t*>(vectors[current].iov_base);
      vectors[current].iov_base = base + remaining;
      vectors[current].iov_len -= remaining;
    }
  }
  return Status::ok;
}

[[nodiscard]] Status sync_descriptor(const int descriptor) noexcept {
  for (;;) {
    if (::fdatasync(descriptor) == 0) {
      return Status::ok;
    }
    if (errno != EINTR) {
      return errno_status(errno);
    }
  }
}

[[nodiscard]] Status sync_directory(const std::filesystem::path& path) noexcept {
  const auto descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0) {
    return errno_status(errno);
  }
  const auto status = ::fsync(descriptor) == 0 ? Status::ok : errno_status(errno);
  (void)::close(descriptor);
  return status;
}

[[nodiscard]] std::uint64_t monotonic_now_ns() noexcept {
  timespec value{};
  if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0 || value.tv_sec < 0) {
    return 0U;
  }
  return (static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000U) +
         static_cast<std::uint64_t>(value.tv_nsec);
}

[[nodiscard]] std::int64_t wall_now_ns() noexcept {
  timespec value{};
  if (::clock_gettime(CLOCK_REALTIME, &value) != 0 || value.tv_sec < 0) {
    return 0;
  }
  constexpr auto kBillion = static_cast<std::int64_t>(1'000'000'000);
  if (value.tv_sec > std::numeric_limits<std::int64_t>::max() / kBillion) {
    return 0;
  }
  return (static_cast<std::int64_t>(value.tv_sec) * kBillion) + value.tv_nsec;
}

[[nodiscard]] bool valid_writer_config(const WriterConfig& config) noexcept {
  constexpr std::uint64_t kMinimumSegmentBytes =
      detail::kSegmentHeaderBytes + detail::kRecordHeaderBytes +
      detail::kRecordTrailerBytes + detail::kSegmentFooterBytes + 1U;
  return !config.directory.empty() && config.initial_sequence != 0U &&
         config.maximum_segment_bytes >= kMinimumSegmentBytes &&
         config.maximum_records_per_segment != 0U &&
         config.index_stride_records != 0U && config.sync_policy >= SyncPolicy::none &&
         config.sync_policy <= SyncPolicy::segment_close &&
         (config.sync_policy != SyncPolicy::periodic_records ||
          config.periodic_sync_records != 0U) &&
         !common::is_zero_digest(config.configuration_sha256) &&
         !common::is_zero_digest(config.build_sha256) &&
         config.writer_instance_id.valid();
}

[[nodiscard]] bool
canonical_kind_matches(const RecordKind kind,
                       const common::wire::RecordType type) noexcept {
  using Type = common::wire::RecordType;
  switch (kind) {
  case RecordKind::normalized_market_event:
    return type == Type::MARKET_EVENT || type == Type::BOOK_UPDATE ||
           type == Type::TRADE_EVENT || type == Type::QUOTE_EVENT ||
           type == Type::AUCTION_IMBALANCE || type == Type::TRADING_STATUS;
  case RecordKind::book_validity_change:
  case RecordKind::data_quality:
    return type == Type::DATA_QUALITY_STATE;
  case RecordKind::feature_snapshot:
    return type == Type::FEATURE_SNAPSHOT_METADATA;
  case RecordKind::model_forecast:
    return type == Type::MODEL_FORECAST;
  case RecordKind::ensemble_decision:
    return type == Type::ENSEMBLE_FORECAST;
  case RecordKind::risk_result:
    return type == Type::RISK_DECISION;
  case RecordKind::order_command:
    return type == Type::ORDER_INTENT || type == Type::ORDER_EVENT;
  case RecordKind::gateway_response:
    return type == Type::ORDER_EVENT;
  case RecordKind::fill:
    return type == Type::FILL_EVENT;
  case RecordKind::position_change:
    return type == Type::POSITION_SNAPSHOT;
  case RecordKind::kill_switch:
    return type == Type::KILL_SWITCH_EVENT;
  case RecordKind::clock_quality:
    return type == Type::CLOCK_QUALITY_STATE;
  case RecordKind::raw_packet_metadata:
  case RecordKind::configuration_change:
  case RecordKind::operator_action:
  case RecordKind::unknown:
    return false;
  }
  return false;
}

[[nodiscard]] bool
canonical_binding_matches(const RecordMetadata& metadata,
                          const common::ValidatedAuditEnvelopeView& view) noexcept {
  const auto* envelope = view.envelope;
  const auto* identifier = envelope == nullptr ? nullptr : envelope->envelope_id();
  const auto* process_time =
      envelope == nullptr ? nullptr : envelope->created_process_monotonic_time();
  const auto* wall_time =
      envelope == nullptr ? nullptr : envelope->recorded_wall_clock_utc_time();
  return envelope != nullptr && envelope->schema_version() != nullptr &&
         identifier != nullptr && process_time != nullptr && wall_time != nullptr &&
         envelope->schema_version()->major() == metadata.schema_version.major &&
         envelope->schema_version()->minor() == metadata.schema_version.minor &&
         envelope->schema_version()->patch() == metadata.schema_version.patch &&
         identifier->high() == metadata.global_event_id.high() &&
         identifier->low() == metadata.global_event_id.low() &&
         process_time->value() == metadata.created_process_monotonic_time_ns &&
         wall_time->value() == metadata.recorded_wall_clock_utc_time_ns &&
         canonical_kind_matches(metadata.kind, envelope->record_type());
}

[[nodiscard]] Status validate_record(const RecordView& record,
                                     const bool validate_envelope) noexcept {
  if (!is_known_record_kind(record.metadata.kind) ||
      record.metadata.encoding == PayloadEncoding::unknown ||
      record.metadata.priority < RecordPriority::advisory ||
      record.metadata.priority > RecordPriority::mandatory ||
      record.metadata.schema_version.major == 0U ||
      record.metadata.created_process_monotonic_time_ns == 0U ||
      record.metadata.recorded_wall_clock_utc_time_ns <= 0 ||
      !record.metadata.global_event_id.valid() || record.payload.empty()) {
    return Status::invalid_argument;
  }
  if (record.payload.size() > kMaximumPersistentPayloadBytes) {
    return Status::payload_too_large;
  }
  if (record.metadata.encoding == PayloadEncoding::canonical_audit_envelope &&
      validate_envelope) {
    common::ValidatedAuditEnvelopeView view{};
    const auto result =
        common::validate_size_prefixed_audit_envelope(record.payload, &view);
    if (!result.ok() || !canonical_binding_matches(record.metadata, view)) {
      return Status::invalid_payload;
    }
  }
  return Status::ok;
}

struct SegmentLocation {
  std::uint64_t segment_id{};
  std::uint64_t file_bytes{};
  std::uint64_t offset{};
};

[[nodiscard]] Status parse_record_at(const int descriptor,
                                     const SegmentLocation& location,
                                     const ScanOptions& options,
                                     PersistedRecord& output,
                                     std::uint64_t& next_offset) {
  if (location.offset > location.file_bytes ||
      location.file_bytes - location.offset < detail::kRecordHeaderBytes) {
    return Status::truncated_tail;
  }
  detail::RecordHeaderBytes header_bytes{};
  auto status = pread_all(descriptor, header_bytes, location.offset);
  if (status != Status::ok) {
    return status;
  }
  RecordHeaderFields header{};
  status = detail::decode_record_header(header_bytes, header);
  if (status != Status::ok) {
    return status;
  }
  if (header.total_frame_bytes > location.file_bytes - location.offset) {
    return Status::truncated_tail;
  }
  std::vector<std::uint8_t> payload(header.payload_bytes);
  status = pread_all(descriptor, payload, location.offset + detail::kRecordHeaderBytes);
  if (status != Status::ok) {
    return status;
  }
  if (common::sha256(payload) != header.payload_sha256) {
    return Status::corrupt_record;
  }
  const auto digest = detail::record_digest(header_bytes);
  detail::RecordTrailerBytes trailer{};
  status =
      pread_all(descriptor, trailer,
                location.offset + detail::kRecordHeaderBytes + header.payload_bytes);
  if (status != Status::ok) {
    return status;
  }
  status =
      detail::decode_record_trailer(trailer, header, digest, header_bytes, payload);
  if (status != Status::ok) {
    return status;
  }
  if (header.metadata.encoding == PayloadEncoding::canonical_audit_envelope &&
      options.validate_canonical_envelopes) {
    common::ValidatedAuditEnvelopeView envelope{};
    const auto validation =
        common::validate_size_prefixed_audit_envelope(payload, &envelope);
    if (!validation.ok() || !canonical_binding_matches(header.metadata, envelope)) {
      return Status::invalid_payload;
    }
  }
  output.metadata = header.metadata;
  output.journal_sequence = header.sequence;
  output.segment_id = location.segment_id;
  output.file_offset = location.offset;
  output.previous_record_sha256 = header.previous_record_sha256;
  output.payload_sha256 = header.payload_sha256;
  output.record_sha256 = digest;
  if (options.load_payloads) {
    output.payload = std::move(payload);
  }
  next_offset = location.offset + header.total_frame_bytes;
  return Status::ok;
}

[[nodiscard]] std::vector<std::filesystem::path>
list_segment_paths(const std::filesystem::path& directory, Status& status) {
  std::vector<std::filesystem::path> paths;
  std::error_code error;
  const std::filesystem::directory_iterator end;
  for (std::filesystem::directory_iterator iterator(directory, error);
       !error && iterator != end; iterator.increment(error)) {
    if (iterator->is_regular_file(error) && !error &&
        is_segment_path(iterator->path())) {
      paths.push_back(iterator->path());
    }
  }
  if (error) {
    status = Status::io_error;
    return {};
  }
  std::ranges::sort(paths);
  status = Status::ok;
  return paths;
}

[[nodiscard]] bool
is_recoverable_directory_tail(const DirectoryReport& report) noexcept {
  return report.status == Status::truncated_tail &&
         std::ranges::any_of(report.segments, [&report](const auto& segment) {
           return segment.path == report.issue.path && segment.recoverable_tail;
         });
}

} // namespace

class SegmentWriter::Impl final {
public:
  ~Impl() { (void)close(); }

  [[nodiscard]] Status open(const WriterConfig& config) {
    if (open_) {
      return Status::already_open;
    }
    if (!valid_writer_config(config)) {
      return Status::invalid_argument;
    }
    std::error_code error;
    std::filesystem::create_directories(config.directory, error);
    if (error) {
      return Status::io_error;
    }

    const auto existing = RecoveryScanner::scan_directory(
        config.directory,
        {.load_payloads = false,
         .validate_canonical_envelopes = config.validate_canonical_envelopes,
         .permit_recoverable_tail = true});
    const auto recoverable_tail = is_recoverable_directory_tail(existing);
    if (existing.status != Status::ok && !recoverable_tail) {
      return existing.status;
    }
    config_ = config;
    next_sequence_ = existing.valid_record_count == 0U ? config.initial_sequence
                                                       : existing.last_sequence + 1U;
    previous_record_sha256_ = existing.terminal_record_sha256;
    std::uint64_t maximum_segment_id{};
    for (const auto& segment : existing.segments) {
      maximum_segment_id = std::max(maximum_segment_id, segment.segment_id);
    }
    next_segment_id_ = maximum_segment_id + 1U;
    metrics_.next_sequence = next_sequence_;
    const auto status = open_segment();
    if (status != Status::ok) {
      return status;
    }
    open_ = true;
    return Status::ok;
  }

  [[nodiscard]] AppendResult append(const RecordView& record) {
    AppendResult result{};
    if (!open_ || failed_) {
      result.status = failed_ ? failure_status_ : Status::not_open;
      return result;
    }
    result.status = validate_record(record, config_.validate_canonical_envelopes);
    if (result.status != Status::ok) {
      return result;
    }
    const auto frame_bytes = detail::kRecordHeaderBytes + record.payload.size() +
                             detail::kRecordTrailerBytes;
    if (frame_bytes > std::numeric_limits<std::uint32_t>::max()) {
      result.status = Status::payload_too_large;
      return result;
    }
    if (detail::kSegmentHeaderBytes + frame_bytes + detail::kSegmentFooterBytes >
        config_.maximum_segment_bytes) {
      result.status = Status::payload_too_large;
      return result;
    }
    if (segment_record_count_ != 0U &&
        (segment_record_count_ >= config_.maximum_records_per_segment ||
         current_offset_ + frame_bytes + detail::kSegmentFooterBytes >
             config_.maximum_segment_bytes)) {
      auto status = seal_segment();
      if (status == Status::ok) {
        ++metrics_.rotated_segments;
        status = open_segment();
      }
      if (status != Status::ok) {
        fail(status);
        result.status = status;
        return result;
      }
    }

    const auto payload_digest = common::sha256(record.payload);
    const RecordHeaderFields fields{
        .metadata = record.metadata,
        .sequence = next_sequence_,
        .payload_bytes = static_cast<std::uint32_t>(record.payload.size()),
        .total_frame_bytes = static_cast<std::uint32_t>(frame_bytes),
        .previous_record_sha256 = previous_record_sha256_,
        .payload_sha256 = payload_digest,
    };
    const auto header = detail::encode_record_header(fields);
    const auto digest = detail::record_digest(header);
    const auto trailer =
        detail::encode_record_trailer(fields, digest, header, record.payload);
    const auto record_offset = current_offset_;
    auto status = write_frame(journal_fd_, header, record.payload, trailer);
    if (status != Status::ok) {
      fail(status);
      result.status = status;
      return result;
    }
    current_offset_ += frame_bytes;
    ++segment_record_count_;
    ++records_since_sync_;
    previous_record_sha256_ = digest;

    if (segment_record_count_ == 1U ||
        ((segment_record_count_ - 1U) % config_.index_stride_records) == 0U) {
      const auto entry = detail::encode_index_entry({.sequence = next_sequence_,
                                                     .file_offset = record_offset,
                                                     .record_sha256 = digest});
      status = write_all(index_fd_, entry);
      if (status != Status::ok) {
        fail(status);
        result.status = status;
        return result;
      }
      ++index_entry_count_;
    }

    bool durable{};
    if (config_.sync_policy == SyncPolicy::every_record ||
        (config_.sync_policy == SyncPolicy::periodic_records &&
         records_since_sync_ >= config_.periodic_sync_records)) {
      status = flush();
      durable = status == Status::ok;
      if (status != Status::ok) {
        result.status = status;
        return result;
      }
    }

    result.status = Status::ok;
    result.journal_sequence = next_sequence_;
    result.record_sha256 = digest;
    result.durable_at_return = durable;
    ++next_sequence_;
    ++metrics_.appended_records;
    metrics_.appended_bytes += frame_bytes;
    metrics_.next_sequence = next_sequence_;
    return result;
  }

  [[nodiscard]] Status flush() {
    if (!open_ || failed_) {
      return failed_ ? failure_status_ : Status::not_open;
    }
    auto status = sync_descriptor(journal_fd_);
    if (status == Status::ok) {
      status = sync_descriptor(index_fd_);
    }
    if (status != Status::ok) {
      fail(status);
      return status;
    }
    ++metrics_.sync_calls;
    records_since_sync_ = 0U;
    return Status::ok;
  }

  [[nodiscard]] Status close() {
    if (!open_) {
      return failed_ ? failure_status_ : Status::ok;
    }
    Status status{};
    if (failed_) {
      status = failure_status_;
      close_descriptors();
    } else if (segment_record_count_ == 0U) {
      close_descriptors();
      std::error_code ignored;
      std::filesystem::remove(open_segment_path_, ignored);
      std::filesystem::remove(open_index_path_, ignored);
      status = Status::ok;
    } else {
      status = seal_segment();
    }
    open_ = false;
    return status;
  }

  [[nodiscard]] bool is_open() const noexcept { return open_ && !failed_; }
  [[nodiscard]] WriterMetrics metrics() const noexcept { return metrics_; }

private:
  [[nodiscard]] Status open_segment() {
    const auto created_wall = wall_now_ns();
    const auto created_monotonic = monotonic_now_ns();
    if (next_segment_id_ == 0U || next_sequence_ == 0U || created_wall <= 0 ||
        created_monotonic == 0U) {
      return Status::invalid_argument;
    }
    open_segment_path_ = segment_path(config_.directory, next_segment_id_, "open");
    open_index_path_ = segment_path(config_.directory, next_segment_id_, "idx.open");
    if (open_segment_path_.empty() || open_index_path_.empty()) {
      return Status::invalid_argument;
    }
    journal_fd_ = ::open(open_segment_path_.c_str(),
                         O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (journal_fd_ < 0) {
      return errno == EEXIST ? Status::target_exists : errno_status(errno);
    }
    index_fd_ = ::open(open_index_path_.c_str(),
                       O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (index_fd_ < 0) {
      const auto status = errno == EEXIST ? Status::target_exists : errno_status(errno);
      (void)::close(journal_fd_);
      journal_fd_ = -1;
      return status;
    }

    segment_header_fields_ = {
        .segment_id = next_segment_id_,
        .first_sequence = next_sequence_,
        .created_wall_clock_utc_ns = created_wall,
        .created_process_monotonic_ns = created_monotonic,
        .writer_instance_id = config_.writer_instance_id,
        .configuration_sha256 = config_.configuration_sha256,
        .build_sha256 = config_.build_sha256,
        .previous_terminal_record_sha256 = previous_record_sha256_,
        .flags = static_cast<std::uint32_t>(config_.sync_policy),
    };
    segment_header_bytes_ = detail::encode_segment_header(segment_header_fields_);
    auto status = write_all(journal_fd_, segment_header_bytes_);
    if (status == Status::ok) {
      const auto index_header =
          detail::encode_index_header({.segment_id = next_segment_id_,
                                       .first_sequence = next_sequence_,
                                       .last_sequence = 0U,
                                       .entry_count = 0U,
                                       .stride = config_.index_stride_records,
                                       .terminal_record_sha256 = {}});
      status = write_all(index_fd_, index_header);
    }
    if (status == Status::ok && config_.preallocate_bytes != 0U) {
      const auto requested =
          std::min(config_.preallocate_bytes, config_.maximum_segment_bytes);
      const auto result =
          ::posix_fallocate(journal_fd_, 0, static_cast<off_t>(requested));
      if (result != 0 && result != EOPNOTSUPP && result != ENOSYS) {
        status = errno_status(result);
      }
    }
    if (status == Status::ok) {
      (void)::posix_fadvise(journal_fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
      current_offset_ = detail::kSegmentHeaderBytes;
      segment_record_count_ = 0U;
      index_entry_count_ = 0U;
      records_since_sync_ = 0U;
      metrics_.current_segment_id = next_segment_id_;
      ++next_segment_id_;
      return Status::ok;
    }
    close_descriptors();
    return status;
  }

  [[nodiscard]] Status seal_segment() {
    if (journal_fd_ < 0 || index_fd_ < 0 || segment_record_count_ == 0U) {
      return Status::not_open;
    }
    if (::ftruncate(journal_fd_, static_cast<off_t>(current_offset_)) != 0) {
      return errno_status(errno);
    }
    const SegmentFooterFields footer_fields{
        .segment_id = segment_header_fields_.segment_id,
        .first_sequence = segment_header_fields_.first_sequence,
        .last_sequence = next_sequence_ - 1U,
        .record_count = segment_record_count_,
        .data_bytes = current_offset_,
        .terminal_record_sha256 = previous_record_sha256_,
        .segment_header_sha256 = common::sha256(segment_header_bytes_),
    };
    const auto footer = detail::encode_segment_footer(footer_fields);
    auto status = write_all(journal_fd_, footer);
    if (status == Status::ok) {
      status = sync_descriptor(journal_fd_);
      ++metrics_.sync_calls;
    }

    const auto index_header = detail::encode_index_header(
        {.segment_id = segment_header_fields_.segment_id,
         .first_sequence = segment_header_fields_.first_sequence,
         .last_sequence = next_sequence_ - 1U,
         .entry_count = index_entry_count_,
         .stride = config_.index_stride_records,
         .terminal_record_sha256 = previous_record_sha256_});
    if (status == Status::ok) {
      status = pwrite_all(index_fd_, index_header, 0U);
    }
    if (status == Status::ok) {
      status = sync_descriptor(index_fd_);
      ++metrics_.sync_calls;
    }
    close_descriptors();
    if (status != Status::ok) {
      return status;
    }

    const auto sealed_segment =
        segment_path(config_.directory, segment_header_fields_.segment_id, "ajl");
    const auto sealed_index =
        segment_path(config_.directory, segment_header_fields_.segment_id, "idx");
    std::error_code error;
    std::filesystem::rename(open_segment_path_, sealed_segment, error);
    if (error) {
      return Status::io_error;
    }
    std::filesystem::rename(open_index_path_, sealed_index, error);
    if (error) {
      return Status::io_error;
    }
    return sync_directory(config_.directory);
  }

  void close_descriptors() noexcept {
    if (journal_fd_ >= 0) {
      (void)::close(journal_fd_);
      journal_fd_ = -1;
    }
    if (index_fd_ >= 0) {
      (void)::close(index_fd_);
      index_fd_ = -1;
    }
  }

  void fail(const Status status) noexcept {
    failed_ = true;
    failure_status_ = status;
    ++metrics_.io_failures;
    if (status == Status::disk_full) {
      ++metrics_.disk_full_failures;
    }
  }

  WriterConfig config_{};
  int journal_fd_{-1};
  int index_fd_{-1};
  bool open_{false};
  bool failed_{false};
  Status failure_status_{Status::ok};
  std::filesystem::path open_segment_path_;
  std::filesystem::path open_index_path_;
  SegmentHeaderFields segment_header_fields_{};
  detail::SegmentHeaderBytes segment_header_bytes_{};
  std::uint64_t current_offset_{};
  std::uint64_t segment_record_count_{};
  std::uint64_t index_entry_count_{};
  std::uint64_t records_since_sync_{};
  std::uint64_t next_sequence_{1U};
  std::uint64_t next_segment_id_{1U};
  common::Sha256Digest previous_record_sha256_{};
  WriterMetrics metrics_{};
};

SegmentWriter::SegmentWriter() : impl_(std::make_unique<Impl>()) {}
SegmentWriter::~SegmentWriter() = default;
SegmentWriter::SegmentWriter(SegmentWriter&&) noexcept = default;
SegmentWriter& SegmentWriter::operator=(SegmentWriter&&) noexcept = default;

Status SegmentWriter::open(const WriterConfig& config) { return impl_->open(config); }
AppendResult SegmentWriter::append(const RecordView& record) {
  return impl_->append(record);
}
Status SegmentWriter::flush() { return impl_->flush(); }
Status SegmentWriter::close() { return impl_->close(); }
bool SegmentWriter::is_open() const noexcept { return impl_->is_open(); }
WriterMetrics SegmentWriter::metrics() const noexcept { return impl_->metrics(); }

// This scanner keeps each fail-closed validation adjacent to its failure offset.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
SegmentReport RecoveryScanner::scan_segment(const std::filesystem::path& path,
                                            const ScanOptions& options) {
  SegmentReport report{};
  report.path = path;
  report.sealed = is_sealed_path(path);
  const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    report.status = errno_status(errno);
    report.issue = {.status = report.status, .path = path, .detail = "open failed"};
    return report;
  }
  struct stat info{};
  if (::fstat(descriptor, &info) != 0) {
    report.status = errno_status(errno);
    report.issue = {.status = report.status, .path = path, .detail = "stat failed"};
    (void)::close(descriptor);
    return report;
  }
  report.file_bytes = checked_file_size(info);
  if (report.file_bytes < detail::kSegmentHeaderBytes) {
    report.status = Status::truncated_tail;
    report.recoverable_tail = options.permit_recoverable_tail && !report.sealed;
    report.issue = {
        .status = report.status, .path = path, .detail = "partial segment header"};
    (void)::close(descriptor);
    return report;
  }

  detail::SegmentHeaderBytes segment_header_bytes{};
  auto status = pread_all(descriptor, segment_header_bytes, 0U);
  SegmentHeaderFields segment_header{};
  if (status == Status::ok) {
    status = detail::decode_segment_header(segment_header_bytes, segment_header);
  }
  if (status != Status::ok) {
    report.status = status;
    report.issue = {.status = status, .path = path, .detail = "invalid segment header"};
    (void)::close(descriptor);
    return report;
  }
  report.segment_id = segment_header.segment_id;
  report.first_sequence = segment_header.first_sequence;
  report.previous_terminal_record_sha256 =
      segment_header.previous_terminal_record_sha256;
  report.terminal_record_sha256 = segment_header.previous_terminal_record_sha256;
  report.valid_bytes = detail::kSegmentHeaderBytes;

  auto offset = static_cast<std::uint64_t>(detail::kSegmentHeaderBytes);
  auto expected_sequence = segment_header.first_sequence;
  auto previous_digest = segment_header.previous_terminal_record_sha256;
  bool found_footer{};
  while (offset < report.file_bytes) {
    const auto remaining = report.file_bytes - offset;
    std::array<std::uint8_t, 8> prefix{};
    if (remaining < prefix.size()) {
      status = Status::truncated_tail;
      break;
    }
    status = pread_all(descriptor, prefix, offset);
    if (status != Status::ok) {
      break;
    }
    if (detail::all_zero(prefix)) {
      status = Status::truncated_tail;
      break;
    }
    if (detail::is_segment_footer_magic(prefix)) {
      if (remaining < detail::kSegmentFooterBytes) {
        status = Status::truncated_tail;
        break;
      }
      detail::SegmentFooterBytes footer_bytes{};
      status = pread_all(descriptor, footer_bytes, offset);
      SegmentFooterFields footer{};
      if (status == Status::ok) {
        status = detail::decode_segment_footer(footer_bytes, footer);
      }
      if (status != Status::ok || footer.segment_id != segment_header.segment_id ||
          footer.first_sequence != segment_header.first_sequence ||
          footer.last_sequence != report.last_sequence ||
          footer.record_count != report.valid_record_count ||
          footer.data_bytes != offset ||
          footer.terminal_record_sha256 != previous_digest ||
          footer.segment_header_sha256 != common::sha256(segment_header_bytes)) {
        status = status == Status::ok ? Status::corrupt_record : status;
        break;
      }
      offset += detail::kSegmentFooterBytes;
      if (offset != report.file_bytes) {
        status = Status::corrupt_record;
        break;
      }
      found_footer = true;
      report.valid_bytes = offset;
      status = Status::ok;
      break;
    }

    PersistedRecord record{};
    std::uint64_t next_offset{};
    status = parse_record_at(descriptor,
                             {.segment_id = segment_header.segment_id,
                              .file_bytes = report.file_bytes,
                              .offset = offset},
                             options, record, next_offset);
    if (status != Status::ok) {
      break;
    }
    if (record.journal_sequence != expected_sequence) {
      status = Status::sequence_gap;
      break;
    }
    if (record.previous_record_sha256 != previous_digest) {
      status = Status::chain_mismatch;
      break;
    }
    ++expected_sequence;
    previous_digest = record.record_sha256;
    report.last_sequence = record.journal_sequence;
    report.terminal_record_sha256 = record.record_sha256;
    ++report.valid_record_count;
    report.valid_bytes = next_offset;
    if (options.load_payloads) {
      report.records.push_back(std::move(record));
    }
    offset = next_offset;
  }
  (void)::close(descriptor);

  if (status == Status::ok && report.sealed && !found_footer) {
    status = Status::truncated_tail;
  }
  if (status == Status::ok && !report.sealed && found_footer) {
    status = Status::corrupt_record;
  }
  report.status = status;
  if (status == Status::truncated_tail) {
    report.recoverable_tail = options.permit_recoverable_tail && !report.sealed;
  }
  if (status != Status::ok) {
    report.issue = {
        .status = status,
        .path = path,
        .file_offset = offset,
        .expected_sequence = expected_sequence,
        .detail = status == Status::truncated_tail
                      ? "incomplete EOF frame or preallocated tail"
                      : "integrity validation failed",
    };
  }
  return report;
}

// Segment continuity is deliberately evaluated in one ordered fail-closed pass.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
DirectoryReport RecoveryScanner::scan_directory(const std::filesystem::path& directory,
                                                const ScanOptions& options) {
  DirectoryReport report{};
  std::error_code error;
  if (!std::filesystem::exists(directory, error)) {
    report.status = error ? Status::io_error : Status::ok;
    return report;
  }
  Status list_status{};
  auto paths = list_segment_paths(directory, list_status);
  if (list_status != Status::ok) {
    report.status = list_status;
    return report;
  }
  for (const auto& path : paths) {
    auto segment = scan_segment(path, options);
    if (segment.segment_id == 0U) {
      report.status = segment.status;
      report.issue = segment.issue;
      report.segments.push_back(std::move(segment));
      return report;
    }
    report.segments.push_back(std::move(segment));
  }
  std::ranges::sort(report.segments, {}, &SegmentReport::segment_id);

  bool saw_recoverable_tail{};
  std::uint64_t expected_sequence{};
  common::Sha256Digest expected_digest{};
  bool first = true;
  for (auto& segment : report.segments) {
    if (!first && segment.first_sequence != expected_sequence) {
      report.status = Status::sequence_gap;
      report.issue = {.status = report.status,
                      .path = segment.path,
                      .expected_sequence = expected_sequence,
                      .detail = "segment sequence discontinuity"};
      return report;
    }
    if (!first && segment.previous_terminal_record_sha256 != expected_digest) {
      report.status = Status::chain_mismatch;
      report.issue = {.status = report.status,
                      .path = segment.path,
                      .expected_sequence = expected_sequence,
                      .detail = "segment chain discontinuity"};
      return report;
    }
    if (segment.valid_record_count != 0U) {
      if (first) {
        report.first_sequence = segment.first_sequence;
      }
      report.last_sequence = segment.last_sequence;
      report.valid_record_count += segment.valid_record_count;
      report.terminal_record_sha256 = segment.terminal_record_sha256;
      expected_sequence = segment.last_sequence + 1U;
      expected_digest = segment.terminal_record_sha256;
      first = false;
    } else if (first) {
      expected_sequence = segment.first_sequence;
      expected_digest = segment.previous_terminal_record_sha256;
    }
    if (segment.status != Status::ok &&
        (segment.status != Status::truncated_tail || !segment.recoverable_tail)) {
      report.status = segment.status;
      report.issue = segment.issue;
      return report;
    }
    saw_recoverable_tail = saw_recoverable_tail || segment.recoverable_tail;
  }
  report.status = saw_recoverable_tail ? Status::truncated_tail : Status::ok;
  if (saw_recoverable_tail) {
    for (const auto& segment : report.segments) {
      if (segment.recoverable_tail) {
        report.issue = segment.issue;
        break;
      }
    }
  }
  return report;
}

// Sparse index validation and forward frame validation share one custody path.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
Status RecoveryScanner::read_record(const std::filesystem::path& directory,
                                    const std::uint64_t sequence,
                                    PersistedRecord& output,
                                    const ScanOptions& options) {
  if (sequence == 0U) {
    return Status::invalid_argument;
  }
  const auto directory_report = scan_directory(
      directory, {.load_payloads = false,
                  .validate_canonical_envelopes = options.validate_canonical_envelopes,
                  .permit_recoverable_tail = options.permit_recoverable_tail});
  if (directory_report.status != Status::ok &&
      !is_recoverable_directory_tail(directory_report)) {
    return directory_report.status;
  }
  const SegmentReport* selected{};
  for (const auto& segment : directory_report.segments) {
    if (segment.valid_record_count != 0U && sequence >= segment.first_sequence &&
        sequence <= segment.last_sequence) {
      selected = &segment;
      break;
    }
  }
  if (selected == nullptr) {
    return Status::record_not_found;
  }

  auto index_path = selected->path;
  index_path.replace_extension(selected->sealed ? ".idx" : ".idx.open");
  const auto index_fd = ::open(index_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (index_fd < 0) {
    return Status::corrupt_index;
  }
  detail::IndexHeaderBytes header_bytes{};
  auto status = pread_all(index_fd, header_bytes, 0U);
  IndexHeaderFields header{};
  if (status == Status::ok) {
    status = detail::decode_index_header(header_bytes, header);
  }
  if (status != Status::ok || header.segment_id != selected->segment_id) {
    (void)::close(index_fd);
    return status == Status::ok ? Status::corrupt_index : status;
  }
  std::uint64_t candidate_offset = detail::kSegmentHeaderBytes;
  common::Sha256Digest candidate_digest{};
  for (std::uint64_t index = 0U; index < header.entry_count; ++index) {
    detail::IndexEntryBytes entry_bytes{};
    status = pread_all(index_fd, entry_bytes,
                       detail::kIndexHeaderBytes + (index * detail::kIndexEntryBytes));
    IndexEntryFields entry{};
    if (status == Status::ok) {
      status = detail::decode_index_entry(entry_bytes, entry);
    }
    if (status != Status::ok) {
      (void)::close(index_fd);
      return Status::corrupt_index;
    }
    if (entry.sequence <= sequence) {
      candidate_offset = entry.file_offset;
      candidate_digest = entry.record_sha256;
    } else {
      break;
    }
  }
  (void)::close(index_fd);

  const auto segment_fd = ::open(selected->path.c_str(), O_RDONLY | O_CLOEXEC);
  if (segment_fd < 0) {
    return errno_status(errno);
  }
  struct stat info{};
  if (::fstat(segment_fd, &info) != 0) {
    (void)::close(segment_fd);
    return errno_status(errno);
  }
  auto offset = candidate_offset;
  for (;;) {
    PersistedRecord record{};
    std::uint64_t next_offset{};
    status = parse_record_at(
        segment_fd,
        {.segment_id = selected->segment_id,
         .file_bytes = checked_file_size(info),
         .offset = offset},
        {.load_payloads = true,
         .validate_canonical_envelopes = options.validate_canonical_envelopes,
         .permit_recoverable_tail = options.permit_recoverable_tail},
        record, next_offset);
    if (status != Status::ok) {
      break;
    }
    if (offset == candidate_offset && !common::is_zero_digest(candidate_digest) &&
        record.record_sha256 != candidate_digest) {
      status = Status::corrupt_index;
      break;
    }
    if (record.journal_sequence == sequence) {
      output = std::move(record);
      status = Status::ok;
      break;
    }
    if (record.journal_sequence > sequence) {
      status = Status::record_not_found;
      break;
    }
    offset = next_offset;
  }
  (void)::close(segment_fd);
  return status;
}

// Copy-only salvage intentionally retains explicit checks for each custody step.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
Status repair_copy(const std::filesystem::path& source,
                   const WriterConfig& target_config, RepairReport& report) {
  if (source.empty() || !valid_writer_config(target_config)) {
    return Status::invalid_argument;
  }
  std::error_code error;
  if (std::filesystem::exists(target_config.directory, error)) {
    if (error || !std::filesystem::is_directory(target_config.directory, error)) {
      return Status::target_exists;
    }
    if (!std::filesystem::is_empty(target_config.directory, error) || error) {
      return Status::target_not_empty;
    }
  }

  DirectoryReport source_report{};
  if (std::filesystem::is_directory(source, error) && !error) {
    source_report =
        RecoveryScanner::scan_directory(source, {.load_payloads = true,
                                                 .validate_canonical_envelopes = true,
                                                 .permit_recoverable_tail = true});
  } else {
    auto segment =
        RecoveryScanner::scan_segment(source, {.load_payloads = true,
                                               .validate_canonical_envelopes = true,
                                               .permit_recoverable_tail = true});
    source_report.status = segment.status;
    source_report.first_sequence = segment.first_sequence;
    source_report.last_sequence = segment.last_sequence;
    source_report.valid_record_count = segment.valid_record_count;
    source_report.terminal_record_sha256 = segment.terminal_record_sha256;
    source_report.issue = segment.issue;
    source_report.segments.push_back(std::move(segment));
  }
  report.source_status = source_report.status;
  report.stopped_at_source_offset = source_report.issue.file_offset;
  report.source_terminal_sha256 = source_report.terminal_record_sha256;
  if (source_report.status != Status::ok &&
      source_report.status != Status::truncated_tail &&
      source_report.valid_record_count == 0U) {
    return source_report.status;
  }
  if (source_report.valid_record_count == 0U) {
    return Status::truncated_tail;
  }

  auto config = target_config;
  config.initial_sequence = source_report.first_sequence;
  SegmentWriter writer;
  auto status = writer.open(config);
  if (status != Status::ok) {
    report.target_status = status;
    return status;
  }
  std::uint64_t expected = source_report.first_sequence;
  for (const auto& segment : source_report.segments) {
    for (const auto& record : segment.records) {
      if (report.copied_records >= source_report.valid_record_count) {
        break;
      }
      if (record.journal_sequence != expected) {
        report.target_status = Status::sequence_gap;
        (void)writer.close();
        return Status::sequence_gap;
      }
      const auto result =
          writer.append({.metadata = record.metadata, .payload = record.payload});
      if (result.status != Status::ok || result.journal_sequence != expected) {
        report.target_status = result.status;
        (void)writer.close();
        return result.status;
      }
      ++expected;
      ++report.copied_records;
      report.target_terminal_sha256 = result.record_sha256;
    }
    if (report.copied_records >= source_report.valid_record_count) {
      break;
    }
  }
  if (report.copied_records != source_report.valid_record_count) {
    report.target_status = Status::sequence_gap;
    (void)writer.close();
    return Status::sequence_gap;
  }
  report.target_status = writer.close();
  return report.target_status;
}

Status replay_verified(const std::filesystem::path& source,
                       const std::uint64_t first_sequence,
                       const std::uint64_t last_sequence,
                       const ReplayCallback& callback,
                       std::uint64_t& replayed_records) {
  replayed_records = 0U;
  if (!callback || (last_sequence != 0U && first_sequence > last_sequence)) {
    return Status::invalid_argument;
  }
  DirectoryReport report{};
  std::error_code error;
  if (std::filesystem::is_directory(source, error) && !error) {
    report =
        RecoveryScanner::scan_directory(source, {.load_payloads = true,
                                                 .validate_canonical_envelopes = true,
                                                 .permit_recoverable_tail = true});
  } else {
    auto segment =
        RecoveryScanner::scan_segment(source, {.load_payloads = true,
                                               .validate_canonical_envelopes = true,
                                               .permit_recoverable_tail = true});
    report.status = segment.status;
    report.segments.push_back(std::move(segment));
  }
  if (report.status != Status::ok) {
    return report.status;
  }
  for (const auto& segment : report.segments) {
    for (const auto& record : segment.records) {
      if (first_sequence != 0U && record.journal_sequence < first_sequence) {
        continue;
      }
      if (last_sequence != 0U && record.journal_sequence > last_sequence) {
        return Status::ok;
      }
      const auto status = callback(record);
      if (status != Status::ok) {
        return status;
      }
      ++replayed_records;
    }
  }
  return Status::ok;
}

// Retention keeps verification, age, floor, and unlink checks in visible order.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
RetentionReport apply_retention(const std::filesystem::path& directory,
                                const RetentionPolicy& policy,
                                const std::int64_t now_wall_clock_utc_ns) {
  RetentionReport result{};
  if (!policy.enabled) {
    return result;
  }
  if (directory.empty() || now_wall_clock_utc_ns <= 0 ||
      policy.retain_at_least_segments == 0U || policy.retain_at_least_records == 0U ||
      policy.maximum_total_bytes == 0U) {
    result.status = Status::invalid_argument;
    return result;
  }
  auto report =
      RecoveryScanner::scan_directory(directory, {.load_payloads = false,
                                                  .validate_canonical_envelopes = true,
                                                  .permit_recoverable_tail = true});
  if (report.status != Status::ok) {
    result.status = report.status;
    return result;
  }
  std::vector<const SegmentReport*> sealed;
  std::uint64_t total_bytes{};
  std::uint64_t total_records{};
  for (const auto& segment : report.segments) {
    if (segment.sealed && segment.status == Status::ok) {
      sealed.push_back(&segment);
      total_bytes += segment.file_bytes;
      total_records += segment.valid_record_count;
    }
  }
  for (std::size_t index = 0U; total_bytes > policy.maximum_total_bytes &&
                               sealed.size() - index > policy.retain_at_least_segments;
       ++index) {
    const auto& segment = *sealed[index];
    if (total_records - segment.valid_record_count < policy.retain_at_least_records) {
      break;
    }
    const auto descriptor = ::open(segment.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
      result.status = errno_status(errno);
      return result;
    }
    detail::SegmentHeaderBytes bytes{};
    auto status = pread_all(descriptor, bytes, 0U);
    (void)::close(descriptor);
    SegmentHeaderFields header{};
    if (status == Status::ok) {
      status = detail::decode_segment_header(bytes, header);
    }
    if (status != Status::ok) {
      result.status = status;
      return result;
    }
    if (header.created_wall_clock_utc_ns > now_wall_clock_utc_ns) {
      break;
    }
    const auto age = static_cast<std::uint64_t>(now_wall_clock_utc_ns -
                                                header.created_wall_clock_utc_ns);
    if (age < policy.minimum_age_ns) {
      break;
    }
    auto index_path = segment.path;
    index_path.replace_extension(".idx");
    std::error_code error;
    if (std::filesystem::exists(index_path, error) && !error) {
      std::filesystem::remove(index_path, error);
    }
    if (error) {
      result.status = Status::io_error;
      return result;
    }
    std::filesystem::remove(segment.path, error);
    if (error) {
      result.status = Status::io_error;
      return result;
    }
    result.removed_paths.push_back(segment.path);
    ++result.removed_segments;
    result.removed_bytes += segment.file_bytes;
    total_bytes -= segment.file_bytes;
    total_records -= segment.valid_record_count;
  }
  result.status = sync_directory(directory);
  return result;
}

const char* status_name(const Status status) noexcept {
  switch (status) {
  case Status::ok:
    return "OK";
  case Status::invalid_argument:
    return "INVALID_ARGUMENT";
  case Status::invalid_payload:
    return "INVALID_PAYLOAD";
  case Status::payload_too_large:
    return "PAYLOAD_TOO_LARGE";
  case Status::unsupported_format:
    return "UNSUPPORTED_FORMAT";
  case Status::target_exists:
    return "TARGET_EXISTS";
  case Status::target_not_empty:
    return "TARGET_NOT_EMPTY";
  case Status::not_open:
    return "NOT_OPEN";
  case Status::already_open:
    return "ALREADY_OPEN";
  case Status::io_error:
    return "IO_ERROR";
  case Status::disk_full:
    return "DISK_FULL";
  case Status::corrupt_header:
    return "CORRUPT_HEADER";
  case Status::corrupt_record:
    return "CORRUPT_RECORD";
  case Status::corrupt_index:
    return "CORRUPT_INDEX";
  case Status::truncated_tail:
    return "TRUNCATED_TAIL";
  case Status::sequence_gap:
    return "SEQUENCE_GAP";
  case Status::chain_mismatch:
    return "CHAIN_MISMATCH";
  case Status::queue_full:
    return "QUEUE_FULL";
  case Status::queue_contention:
    return "QUEUE_CONTENTION";
  case Status::inhibited:
    return "INHIBITED";
  case Status::stopped:
    return "STOPPED";
  case Status::deadline_exceeded:
    return "DEADLINE_EXCEEDED";
  case Status::record_not_found:
    return "RECORD_NOT_FOUND";
  }
  return "UNKNOWN_STATUS";
}

const char* record_kind_name(const RecordKind kind) noexcept {
  switch (kind) {
  case RecordKind::unknown:
    return "UNKNOWN";
  case RecordKind::raw_packet_metadata:
    return "RAW_PACKET_METADATA";
  case RecordKind::normalized_market_event:
    return "NORMALIZED_MARKET_EVENT";
  case RecordKind::book_validity_change:
    return "BOOK_VALIDITY_CHANGE";
  case RecordKind::feature_snapshot:
    return "FEATURE_SNAPSHOT";
  case RecordKind::model_forecast:
    return "MODEL_FORECAST";
  case RecordKind::ensemble_decision:
    return "ENSEMBLE_DECISION";
  case RecordKind::risk_result:
    return "RISK_RESULT";
  case RecordKind::order_command:
    return "ORDER_COMMAND";
  case RecordKind::gateway_response:
    return "GATEWAY_RESPONSE";
  case RecordKind::fill:
    return "FILL";
  case RecordKind::position_change:
    return "POSITION_CHANGE";
  case RecordKind::kill_switch:
    return "KILL_SWITCH";
  case RecordKind::configuration_change:
    return "CONFIGURATION_CHANGE";
  case RecordKind::operator_action:
    return "OPERATOR_ACTION";
  case RecordKind::data_quality:
    return "DATA_QUALITY";
  case RecordKind::clock_quality:
    return "CLOCK_QUALITY";
  }
  return "UNKNOWN";
}

bool is_known_record_kind(const RecordKind kind) noexcept {
  return kind >= RecordKind::raw_packet_metadata && kind <= RecordKind::clock_quality;
}

} // namespace aegis::journal
