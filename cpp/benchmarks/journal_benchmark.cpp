#include "aegis/journal/async_journal.hpp"
#include "aegis/journal/journal.hpp"

#include "allocation_probe.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

#include <cstdlib>

namespace journal = aegis::journal;
namespace common = aegis::common;

namespace {

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    std::array<char, 64> pattern{};
    constexpr std::string_view value = "/tmp/aegis-journal-bench.XXXXXX";
    std::ranges::copy(value, pattern.begin());
    const auto* directory = ::mkdtemp(pattern.data());
    if (directory != nullptr) {
      path_ = directory;
    }
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

[[nodiscard]] common::Sha256Digest digest(const std::string_view text) {
  return common::sha256(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

[[nodiscard]] journal::WriterConfig
configuration(const std::filesystem::path& directory) {
  return {
      .directory = directory,
      .maximum_segment_bytes = 512ULL * 1024U * 1024U,
      .maximum_records_per_segment = 10'000'000U,
      .index_stride_records = 1'024U,
      .sync_policy = journal::SyncPolicy::segment_close,
      .periodic_sync_records = 4'096U,
      .configuration_sha256 = digest("journal-benchmark-config"),
      .build_sha256 = digest("journal-benchmark-build"),
      .writer_instance_id = common::GlobalEventId(0xBEEFU, 0x2500U),
      .validate_canonical_envelopes = false,
  };
}

[[nodiscard]] journal::RecordMetadata metadata(const std::uint64_t ordinal) {
  return {
      .kind = journal::RecordKind::normalized_market_event,
      .encoding = journal::PayloadEncoding::opaque_binary,
      .priority = journal::RecordPriority::mandatory,
      .schema_version = {.major = 1U, .minor = 8U, .patch = 0U},
      .created_process_monotonic_time_ns = 1'000U + ordinal,
      .recorded_wall_clock_utc_time_ns =
          1'800'000'000'000'000'000LL + static_cast<std::int64_t>(ordinal),
      .source_event_sequence = ordinal,
      .global_event_id = common::GlobalEventId(0x25U, ordinal + 1U),
  };
}

void benchmark_journal_hot_publish(benchmark::State& state) {
  const TemporaryDirectory temporary;
  auto service = std::make_unique<journal::AsyncJournal>();
  if (service->open(configuration(temporary.path()),
                    journal::BackpressurePolicy::fail_closed) != journal::Status::ok) {
    state.SkipWithError("journal open failed");
    return;
  }
  const std::array<std::uint8_t, 256> payload{};
  std::uint64_t sequence{};
  std::uint64_t allocations{};
  for (auto iteration : state) {
    static_cast<void>(iteration);
    const auto before = allocation_probe::count();
    const auto result =
        service->try_publish({.metadata = metadata(sequence++), .payload = payload});
    allocations += allocation_probe::count() - before;
    if (result.status != journal::Status::ok) {
      state.SkipWithError("unexpected publication rejection");
      return;
    }
    state.PauseTiming();
    std::size_t drained{};
    if (service->drain_once(1U, drained) != journal::Status::ok || drained != 1U) {
      state.SkipWithError("journal drain failed");
      return;
    }
    state.ResumeTiming();
  }
  state.PauseTiming();
  if (service->shutdown(std::chrono::seconds(5)) != journal::Status::ok) {
    state.SkipWithError("journal shutdown failed");
  }
  state.ResumeTiming();
  state.SetItemsProcessed(state.iterations());
  state.counters["steady_state_allocations"] =
      benchmark::Counter(static_cast<double>(allocations));
  state.counters["queue_rejections"] = 0.0;
}

void benchmark_journal_append_256_bytes(benchmark::State& state) {
  const TemporaryDirectory temporary;
  journal::SegmentWriter writer;
  if (writer.open(configuration(temporary.path())) != journal::Status::ok) {
    state.SkipWithError("journal open failed");
    return;
  }
  const std::array<std::uint8_t, 256> payload{};
  std::uint64_t sequence{};
  const auto allocations_before = allocation_probe::count();
  for (auto iteration : state) {
    static_cast<void>(iteration);
    const auto result =
        writer.append({.metadata = metadata(sequence++), .payload = payload});
    if (result.status != journal::Status::ok) {
      state.SkipWithError("journal append failed");
      return;
    }
  }
  const auto steady_state_allocations = allocation_probe::count() - allocations_before;
  state.PauseTiming();
  if (writer.close() != journal::Status::ok) {
    state.SkipWithError("journal close failed");
  }
  state.ResumeTiming();
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() *
                          static_cast<std::int64_t>(payload.size()));
  state.counters["steady_state_allocations"] =
      benchmark::Counter(static_cast<double>(steady_state_allocations));
}

void benchmark_journal_recovery_scan(benchmark::State& state) {
  constexpr std::uint64_t kRecordCount = 1'024U;
  const TemporaryDirectory temporary;
  journal::SegmentWriter writer;
  const auto writer_config = configuration(temporary.path());
  if (writer.open(writer_config) != journal::Status::ok) {
    state.SkipWithError("journal open failed");
    return;
  }
  const std::array<std::uint8_t, 256> payload{};
  for (std::uint64_t sequence = 0U; sequence < kRecordCount; ++sequence) {
    if (writer.append({.metadata = metadata(sequence), .payload = payload}).status !=
        journal::Status::ok) {
      state.SkipWithError("journal setup append failed");
      return;
    }
  }
  if (writer.close() != journal::Status::ok) {
    state.SkipWithError("journal setup close failed");
    return;
  }
  for (auto iteration : state) {
    static_cast<void>(iteration);
    auto report = journal::RecoveryScanner::scan_directory(
        temporary.path(), {.load_payloads = false,
                           .validate_canonical_envelopes = false,
                           .permit_recoverable_tail = false});
    if (report.status != journal::Status::ok ||
        report.valid_record_count != kRecordCount) {
      state.SkipWithError("recovery scan failed");
      return;
    }
    benchmark::DoNotOptimize(report.terminal_record_sha256);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kRecordCount));
}

BENCHMARK(benchmark_journal_hot_publish);
BENCHMARK(benchmark_journal_append_256_bytes);
BENCHMARK(benchmark_journal_recovery_scan);

} // namespace
