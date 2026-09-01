#ifndef AEGIS_MARKET_DATA_SYNTHETIC_ARTIFACTS_HPP
#define AEGIS_MARKET_DATA_SYNTHETIC_ARTIFACTS_HPP

#include "aegis/market_data/synthetic/book.hpp"
#include "aegis/market_data/synthetic/capture.hpp"
#include "aegis/market_data/synthetic/config.hpp"
#include "aegis/market_data/synthetic/generator.hpp"
#include "aegis/market_data/synthetic/mock_protocol.hpp"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string_view>

namespace aegis::market_data::synthetic {

enum class ArtifactError : std::uint8_t {
  none = 0,
  invalid_config,
  generation_failed,
  capture_failed,
  canonical_open_failed,
  canonical_write_failed,
  canonical_validation_failed,
  book_write_failed,
  book_read_failed,
  verification_failed,
  stream_open_failed,
  stream_write_failed,
  inspect_failed,
};

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct ArtifactPaths {
  std::filesystem::path capture;
  std::filesystem::path canonical;
  std::filesystem::path expected_book;
};

struct VerificationPaths {
  std::filesystem::path capture;
  std::filesystem::path expected_book;
};

struct GenerationReport {
  ArtifactError error{ArtifactError::none};
  GenerationError generation_error{GenerationError::none};
  CaptureError capture_error{CaptureError::none};
  std::uint64_t logical_events{};
  std::uint64_t physical_packets{};
  std::uint64_t canonical_records{};
  std::uint64_t final_book_hash{};
  std::uint64_t configuration_hash{};
};

struct VerificationReport {
  ArtifactError error{ArtifactError::none};
  CaptureError capture_error{CaptureError::none};
  DecodeError decode_error{DecodeError::none};
  std::uint64_t packets{};
  std::uint64_t applied_events{};
  std::uint64_t duplicate_packets{};
  std::uint64_t sequence_gaps{};
  std::uint64_t out_of_order_packets{};
  std::uint64_t stale_intervals{};
  std::uint64_t crossed_books{};
  std::uint64_t halted_update_attempts{};
  std::uint64_t calculated_book_hash{};
  std::uint64_t expected_book_hash{};
  bool scenario_expectation_met{false};
  bool final_book_matches{false};

  [[nodiscard]] constexpr bool ok() const noexcept {
    return error == ArtifactError::none;
  }
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] GenerationReport generate_artifacts(const GeneratorConfig& config,
                                                  const ArtifactPaths& paths,
                                                  bool write_canonical_stream = true);

[[nodiscard]] VerificationReport
verify_artifacts(const VerificationPaths& paths) noexcept;

[[nodiscard]] ArtifactError stream_capture(const std::filesystem::path& capture_path,
                                           std::ostream& output,
                                           std::uint64_t acceleration,
                                           std::uint64_t& packets_streamed);

[[nodiscard]] ArtifactError inspect_capture(const std::filesystem::path& capture_path,
                                            std::ostream& output,
                                            std::uint64_t maximum_records);

[[nodiscard]] std::string_view artifact_error_name(ArtifactError error) noexcept;

} // namespace aegis::market_data::synthetic

#endif // AEGIS_MARKET_DATA_SYNTHETIC_ARTIFACTS_HPP
