#ifndef AEGIS_MARKET_DATA_SYNTHETIC_CAPTURE_HPP
#define AEGIS_MARKET_DATA_SYNTHETIC_CAPTURE_HPP

#include "aegis/market_data/synthetic/book.hpp"
#include "aegis/market_data/synthetic/config.hpp"
#include "aegis/market_data/synthetic/event.hpp"
#include "aegis/market_data/synthetic/mock_protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string_view>

namespace aegis::market_data::synthetic {

inline constexpr std::size_t kCaptureHeaderBytes = 80U;
inline constexpr std::size_t kCaptureRecordHeaderBytes = 12U;
using EncodedCaptureHeader = std::array<std::uint8_t, kCaptureHeaderBytes>;

enum class CaptureError : std::uint8_t {
  none = 0,
  invalid_config,
  open_failed,
  read_failed,
  write_failed,
  seek_failed,
  truncated,
  trailing_bytes,
  invalid_magic,
  unsupported_version,
  invalid_header_length,
  invalid_flags,
  invalid_scenario,
  invalid_count,
  invalid_timestamp,
  checksum_mismatch,
  invalid_record_length,
  packet_error,
  unfinalized,
};

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct CaptureHeader {
  Scenario scenario{Scenario::normal};
  std::uint64_t seed{};
  std::uint64_t configuration_hash{};
  std::uint64_t logical_event_count{};
  std::uint64_t physical_packet_count{};
  std::int64_t start_exchange_time_ns{};
  std::uint64_t stale_threshold_ns{};
  std::uint64_t expected_book_hash{};
};

struct CaptureRecord {
  std::int64_t capture_time_ns{};
  EncodedPacket packet{};
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] EncodedCaptureHeader
encode_capture_header(const CaptureHeader& header) noexcept;
[[nodiscard]] CaptureError decode_capture_header(std::span<const std::uint8_t> bytes,
                                                 CaptureHeader& output) noexcept;

class CaptureWriter {
public:
  [[nodiscard]] static std::optional<CaptureWriter>
  create(const std::filesystem::path& path, const CaptureHeader& header);

  CaptureWriter(CaptureWriter&&) noexcept = default;
  CaptureWriter& operator=(CaptureWriter&&) noexcept = default;
  ~CaptureWriter() = default;

  CaptureWriter(const CaptureWriter&) = delete;
  CaptureWriter& operator=(const CaptureWriter&) = delete;

  [[nodiscard]] CaptureError write(const CaptureRecord& record) noexcept;
  [[nodiscard]] CaptureError finalize(std::uint64_t expected_book_hash) noexcept;
  [[nodiscard]] std::uint64_t records_written() const noexcept;

private:
  CaptureWriter(std::ofstream stream, CaptureHeader header) noexcept;

  std::ofstream stream_;
  CaptureHeader header_{};
  std::uint64_t records_written_{};
  bool finalized_{false};
};

class CaptureReader {
public:
  [[nodiscard]] static std::optional<CaptureReader>
  open(const std::filesystem::path& path, CaptureError& error);

  CaptureReader(CaptureReader&&) noexcept = default;
  CaptureReader& operator=(CaptureReader&&) noexcept = default;

  CaptureReader(const CaptureReader&) = delete;
  CaptureReader& operator=(const CaptureReader&) = delete;

  [[nodiscard]] const CaptureHeader& header() const noexcept;
  [[nodiscard]] CaptureError next(CaptureRecord& output) noexcept;
  [[nodiscard]] CaptureError finish() noexcept;
  [[nodiscard]] std::uint64_t records_read() const noexcept;

private:
  CaptureReader(std::ifstream stream, CaptureHeader header) noexcept;

  std::ifstream stream_;
  CaptureHeader header_{};
  std::uint64_t records_read_{};
};

[[nodiscard]] std::string_view capture_error_name(CaptureError error) noexcept;
[[nodiscard]] bool write_book_snapshot(const std::filesystem::path& path,
                                       const GeneratorConfig& config,
                                       const BookSet& books) noexcept;
[[nodiscard]] std::optional<std::uint64_t>
read_book_snapshot_hash(const std::filesystem::path& path) noexcept;

} // namespace aegis::market_data::synthetic

#endif // AEGIS_MARKET_DATA_SYNTHETIC_CAPTURE_HPP
