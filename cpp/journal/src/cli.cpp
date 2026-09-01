#include "cli.hpp"

#include "aegis/journal/journal.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace aegis::journal::cli {
namespace {

[[nodiscard]] DirectoryReport scan_source(const std::filesystem::path& source,
                                          const bool payloads) {
  std::error_code error;
  if (std::filesystem::is_directory(source, error) && !error) {
    return RecoveryScanner::scan_directory(source,
                                           {.load_payloads = payloads,
                                            .validate_canonical_envelopes = true,
                                            .permit_recoverable_tail = true});
  }
  auto segment =
      RecoveryScanner::scan_segment(source, {.load_payloads = payloads,
                                             .validate_canonical_envelopes = true,
                                             .permit_recoverable_tail = true});
  DirectoryReport report{};
  report.status = segment.status;
  report.first_sequence = segment.first_sequence;
  report.last_sequence = segment.last_sequence;
  report.valid_record_count = segment.valid_record_count;
  report.terminal_record_sha256 = segment.terminal_record_sha256;
  report.issue = segment.issue;
  report.segments.push_back(std::move(segment));
  return report;
}

void write_hex(std::ostream& output, const std::span<const std::uint8_t> bytes) {
  constexpr std::string_view digits = "0123456789abcdef";
  for (const auto byte : bytes) {
    output.put(digits[byte >> 4U]);
    output.put(digits[byte & 0x0FU]);
  }
}

[[nodiscard]] common::Sha256Digest digest_literal(const std::string_view value) {
  return common::sha256(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

[[nodiscard]] WriterConfig repair_config(const std::filesystem::path& directory) {
  return {
      .directory = directory,
      .maximum_segment_bytes = 256ULL * 1024U * 1024U,
      .maximum_records_per_segment = 1'000'000U,
      .index_stride_records = 1'024U,
      .sync_policy = SyncPolicy::every_record,
      .periodic_sync_records = 1U,
      .configuration_sha256 = digest_literal("aegis-journal-repair-copy-v1"),
      .build_sha256 = digest_literal("aegis-journal-tools-v1"),
      .writer_instance_id =
          common::GlobalEventId(0x524550414952434FULL, 0x50592D5630303031ULL),
      .validate_canonical_envelopes = true,
  };
}

[[nodiscard]] bool parse_u64(const std::string_view text,
                             std::uint64_t& output) noexcept {
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), output);
  return error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] int integrity_exit(const Status status) noexcept {
  return status == Status::ok ? 0 : 2;
}

template <typename Integer>
void write_little_endian(std::ostream& output, const Integer value) {
  using Unsigned = std::make_unsigned_t<Integer>;
  const auto bits = static_cast<Unsigned>(value);
  for (std::size_t index = 0U; index < sizeof(Integer); ++index) {
    output.put(static_cast<char>(bits >> static_cast<unsigned>(index * 8U)));
  }
}

} // namespace

int inspect_main(const int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: journal-inspect <journal-directory-or-segment>\n";
    return 1;
  }
  const auto report = scan_source(argv[1], false);
  std::cout << "status=" << status_name(report.status)
            << " segments=" << report.segments.size()
            << " records=" << report.valid_record_count
            << " first_sequence=" << report.first_sequence
            << " last_sequence=" << report.last_sequence << '\n';
  for (const auto& segment : report.segments) {
    std::cout << "segment_id=" << segment.segment_id
              << " path=" << segment.path.string()
              << " sealed=" << (segment.sealed ? "true" : "false")
              << " status=" << status_name(segment.status)
              << " records=" << segment.valid_record_count
              << " valid_bytes=" << segment.valid_bytes
              << " file_bytes=" << segment.file_bytes << '\n';
  }
  if (report.status != Status::ok) {
    std::cout << "issue_path=" << report.issue.path.string()
              << " issue_offset=" << report.issue.file_offset
              << " expected_sequence=" << report.issue.expected_sequence
              << " detail=" << report.issue.detail << '\n';
  }
  return integrity_exit(report.status);
}

int verify_main(const int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: journal-verify <journal-directory-or-segment>\n";
    return 1;
  }
  const auto report = scan_source(argv[1], false);
  std::cout << "status=" << status_name(report.status)
            << " verified_records=" << report.valid_record_count
            << " last_sequence=" << report.last_sequence;
  if (report.status != Status::ok) {
    std::cout << " issue_path=" << report.issue.path.string()
              << " issue_offset=" << report.issue.file_offset
              << " detail=" << report.issue.detail;
  }
  std::cout << '\n';
  return integrity_exit(report.status);
}

int repair_main(const int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: journal-repair-copy <source> <new-empty-target-directory>\n";
    return 1;
  }
  RepairReport report{};
  const auto status = repair_copy(argv[1], repair_config(argv[2]), report);
  std::cout << "repair_status=" << status_name(status)
            << " source_status=" << status_name(report.source_status)
            << " target_status=" << status_name(report.target_status)
            << " copied_records=" << report.copied_records
            << " stopped_at_source_offset=" << report.stopped_at_source_offset
            << " source_modified=false\n";
  return integrity_exit(status);
}

int export_main(const int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: journal-export <source> <output.jsonl>\n";
    return 1;
  }
  std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
  if (!output) {
    std::cerr << "cannot open output\n";
    return 1;
  }
  std::uint64_t exported{};
  const auto status = replay_verified(
      argv[1], 0U, 0U,
      [&](const PersistedRecord& record) {
        output << "{\"sequence\":" << record.journal_sequence << R"(,"segment_id":)"
               << record.segment_id << R"(,"kind":")"
               << record_kind_name(record.metadata.kind) << R"(","schema":")"
               << record.metadata.schema_version.major << '.'
               << record.metadata.schema_version.minor << '.'
               << record.metadata.schema_version.patch << R"(","process_monotonic_ns":)"
               << record.metadata.created_process_monotonic_time_ns
               << R"(,"wall_clock_utc_ns":)"
               << record.metadata.recorded_wall_clock_utc_time_ns
               << R"(,"payload_sha256":")";
        write_hex(output, record.payload_sha256);
        output << R"(","record_sha256":")";
        write_hex(output, record.record_sha256);
        output << R"(","payload_hex":")";
        write_hex(output, record.payload);
        output << R"("})" << '\n';
        return output.good() ? Status::ok : Status::io_error;
      },
      exported);
  output.flush();
  std::cout << "status=" << status_name(status) << " exported_records=" << exported
            << '\n';
  return status == Status::ok && output.good() ? 0 : 2;
}

int replay_main(const int argc, char** argv) {
  if (argc < 3 || argc > 5) {
    std::cerr << "usage: journal-replay <source> <output.ajrp> [first-sequence] "
                 "[last-sequence]\n";
    return 1;
  }
  std::uint64_t first{};
  std::uint64_t last{};
  if ((argc >= 4 && !parse_u64(argv[3], first)) ||
      (argc == 5 && !parse_u64(argv[4], last))) {
    std::cerr << "invalid sequence bound\n";
    return 1;
  }
  std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
  if (!output) {
    std::cerr << "cannot open output\n";
    return 1;
  }
  constexpr std::array<char, 8> kReplayMagic{'A', 'J', 'R', 'P', '0', '0', '0', '1'};
  output.write(kReplayMagic.data(), static_cast<std::streamsize>(kReplayMagic.size()));
  std::uint64_t replayed{};
  const auto status = replay_verified(
      argv[1], first, last,
      [&](const PersistedRecord& record) {
        write_little_endian(output, record.journal_sequence);
        write_little_endian(output, static_cast<std::uint16_t>(record.metadata.kind));
        write_little_endian(output,
                            static_cast<std::uint8_t>(record.metadata.encoding));
        write_little_endian(output, static_cast<std::uint8_t>(0U));
        write_little_endian(output, static_cast<std::uint32_t>(record.payload.size()));
        output.write(reinterpret_cast<const char*>(record.payload.data()),
                     static_cast<std::streamsize>(record.payload.size()));
        return output.good() ? Status::ok : Status::io_error;
      },
      replayed);
  output.flush();
  std::cout << "status=" << status_name(status) << " replayed_records=" << replayed
            << " transmission_capability=false\n";
  return status == Status::ok && output.good() ? 0 : 2;
}

} // namespace aegis::journal::cli
