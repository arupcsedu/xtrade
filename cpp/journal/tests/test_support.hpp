#ifndef AEGIS_JOURNAL_TEST_SUPPORT_HPP
#define AEGIS_JOURNAL_TEST_SUPPORT_HPP

#include "aegis/common/sha256.hpp"
#include "aegis/journal/journal.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cstdlib>

namespace aegis::journal::test {

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    std::array<char, 64> pattern{};
    constexpr std::string_view prefix = "/tmp/aegis-journal-test.XXXXXX";
    std::copy(prefix.begin(), prefix.end(), pattern.begin());
    auto* value = ::mkdtemp(pattern.data());
    if (value != nullptr) {
      path_ = value;
    }
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

[[nodiscard]] inline common::Sha256Digest digest(const std::string_view text) {
  return common::sha256(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

[[nodiscard]] inline WriterConfig config(const std::filesystem::path& directory) {
  return {
      .directory = directory,
      .maximum_segment_bytes = 1024U * 1024U,
      .maximum_records_per_segment = 1'000U,
      .index_stride_records = 2U,
      .sync_policy = SyncPolicy::every_record,
      .periodic_sync_records = 1U,
      .configuration_sha256 = digest("test-configuration-v1"),
      .build_sha256 = digest("test-build-v1"),
      .writer_instance_id = common::GlobalEventId(0xA11D17U, 0xB01D17U),
      .validate_canonical_envelopes = true,
  };
}

[[nodiscard]] inline RecordMetadata
metadata(const std::uint64_t ordinal,
         const RecordKind kind = RecordKind::normalized_market_event,
         const PayloadEncoding encoding = PayloadEncoding::opaque_binary,
         const RecordPriority priority = RecordPriority::mandatory,
         const SchemaVersion schema = {.major = 1U, .minor = 0U, .patch = 0U}) {
  return {
      .kind = kind,
      .encoding = encoding,
      .priority = priority,
      .schema_version = schema,
      .created_process_monotonic_time_ns = 10'000U + ordinal,
      .recorded_wall_clock_utc_time_ns =
          1'800'000'000'000'000'000LL + static_cast<std::int64_t>(ordinal),
      .source_event_sequence = ordinal,
      .global_event_id = common::GlobalEventId(0xABCDU, ordinal + 1U),
  };
}

[[nodiscard]] inline std::vector<std::uint8_t> payload(const std::uint64_t ordinal,
                                                       const std::size_t size = 32U) {
  std::vector<std::uint8_t> bytes(size);
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>((ordinal + index * 17U) & 0xFFU);
  }
  return bytes;
}

[[nodiscard]] inline std::filesystem::path
first_with_extension(const std::filesystem::path& directory,
                     const std::string_view extension) {
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() == extension) {
      return entry.path();
    }
  }
  return {};
}

} // namespace aegis::journal::test

#endif // AEGIS_JOURNAL_TEST_SUPPORT_HPP
