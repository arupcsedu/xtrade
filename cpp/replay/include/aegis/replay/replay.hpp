#ifndef AEGIS_REPLAY_REPLAY_HPP
#define AEGIS_REPLAY_REPLAY_HPP

#include "aegis/common/identifiers.hpp"
#include "aegis/common/sha256.hpp"
#include "aegis/journal/journal.hpp"
#include "aegis/time/time_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aegis::replay {

inline constexpr std::size_t kOutputCategoryCount = 10U;
inline constexpr std::size_t kMaximumReplayRecords = 10'000'000U;
inline constexpr std::uint64_t kMaximumReplayPayloadBytes =
    16ULL * 1024U * 1024U * 1024U;

enum class Status : std::uint8_t {
  ok = 0,
  paused,
  complete,
  invalid_configuration,
  source_open_failed,
  source_invalid,
  source_too_large,
  canonical_validation_failed,
  decode_failed,
  target_failed,
  time_overflow,
  missing_model_selection,
  recomputer_required,
  recompute_failed,
  io_error,
};

enum class SourceKind : std::uint8_t { journal = 1, synthetic_capture = 2 };
enum class ReplayMode : std::uint8_t { exact = 1, recompute = 2 };
enum class SpeedMode : std::uint8_t {
  original = 1,
  accelerated = 2,
  maximum = 3,
  single_step = 4,
};

enum class FaultKind : std::uint8_t {
  latency = 1,
  message_loss = 2,
  duplication = 3,
  reorder_adjacent = 4,
  clock_drift = 5,
  feed_outage = 6,
  process_crash = 7,
  trading_halt = 8,
  reopening = 9,
  news_injection = 10,
  macro_injection = 11,
};

enum class EventKind : std::uint8_t {
  journal_record = 1,
  synthetic_packet = 2,
  process_crash = 3,
  trading_halt = 4,
  reopening = 5,
  news = 6,
  macro = 7,
};

enum class OutputCategory : std::uint8_t {
  books = 0,
  features = 1,
  model_requests = 2,
  model_outputs = 3,
  ensemble_decisions = 4,
  risk_decisions = 5,
  orders = 6,
  fills = 7,
  positions = 8,
  pnl = 9,
};

struct ModelSelection {
  common::ModelId model_id{};
  common::ModelVersion version{};
};

struct FaultSpec {
  FaultKind kind{FaultKind::latency};
  std::uint64_t first_ordinal{1U};
  std::uint64_t last_ordinal{1U};
  std::uint64_t every_nth{1U};
  std::int64_t value{};
  std::string payload;
};

struct ReplayConfig {
  SourceKind source_kind{SourceKind::journal};
  ReplayMode mode{ReplayMode::exact};
  SpeedMode speed{SpeedMode::maximum};
  std::filesystem::path source;
  std::uint64_t seed{20'260'831U};
  std::uint64_t acceleration{1U};
  std::uint64_t first_sequence{};
  std::uint64_t last_sequence{};
  std::uint64_t from_process_monotonic_time_ns{};
  std::uint64_t to_process_monotonic_time_ns{};
  std::uint64_t maximum_records{1'000'000U};
  std::uint64_t maximum_total_payload_bytes{1ULL * 1024U * 1024U * 1024U};
  std::vector<common::InstrumentId> instruments;
  std::vector<ModelSelection> selected_models;
  std::vector<FaultSpec> faults;
  common::Sha256Digest configuration_sha256{};
  std::string environment_label{"unspecified"};
};

struct ReplayEvent {
  EventKind kind{EventKind::journal_record};
  std::uint64_t source_ordinal{};
  std::uint64_t source_sequence{};
  std::uint64_t source_process_monotonic_time_ns{};
  std::uint64_t replay_process_monotonic_time_ns{};
  std::uint64_t process_epoch{1U};
  common::InstrumentId instrument_id{};
  journal::RecordKind record_kind{journal::RecordKind::unknown};
  journal::PayloadEncoding encoding{journal::PayloadEncoding::unknown};
  std::span<const std::uint8_t> payload;
  std::span<const std::uint8_t> source_payload;
  bool duplicate{false};
  bool counterfactual{false};
  std::string_view injected_text;
};

struct CategoryEvidence {
  std::uint64_t count{};
  common::Sha256Digest sha256{};

  bool operator==(const CategoryEvidence&) const = default;
};

struct ReplaySummary {
  Status status{Status::ok};
  std::uint64_t source_records{};
  std::uint64_t selected_records{};
  std::uint64_t delivered_events{};
  std::uint64_t dropped_events{};
  std::uint64_t duplicated_events{};
  std::uint64_t reordered_events{};
  std::uint64_t injected_events{};
  std::uint64_t process_epoch{1U};
  std::uint64_t final_replay_process_monotonic_time_ns{};
  std::array<CategoryEvidence, kOutputCategoryCount> outputs{};
  common::Sha256Digest event_stream_sha256{};
  common::Sha256Digest deterministic_result_sha256{};
};

struct ReplayManifest {
  common::Sha256Digest dataset_sha256{};
  common::Sha256Digest configuration_sha256{};
  common::Sha256Digest manifest_sha256{};
  std::uint64_t seed{};
  SourceKind source_kind{SourceKind::journal};
  ReplayMode mode{ReplayMode::exact};
  std::vector<ModelSelection> selected_models;
  std::string project;
  std::string version;
  std::string source_revision;
  std::string compiler;
  std::string build_type;
  std::string environment_label;
  bool live_trading_capable{};
};

class IReplayPacer {
public:
  virtual ~IReplayPacer() = default;
  [[nodiscard]] virtual Status wait(aegis::time::DurationNs duration) noexcept = 0;
};

class VirtualPacer final : public IReplayPacer {
public:
  [[nodiscard]] Status wait(aegis::time::DurationNs duration) noexcept override;
  [[nodiscard]] std::uint64_t waited_ns() const noexcept;

private:
  std::uint64_t waited_ns_{};
};

class SystemPacer final : public IReplayPacer {
public:
  [[nodiscard]] Status wait(aegis::time::DurationNs duration) noexcept override;
};

class IModelRecomputer {
public:
  virtual ~IModelRecomputer() = default;
  [[nodiscard]] virtual Status
  recompute(common::ModelId model_id, common::ModelVersion selected_version,
            std::span<const std::uint8_t> recorded_forecast,
            std::vector<std::uint8_t>& replacement) noexcept = 0;
};

class IReplayTarget {
public:
  virtual ~IReplayTarget() = default;
  [[nodiscard]] virtual Status consume(const ReplayEvent& event) noexcept = 0;
  [[nodiscard]] virtual Status finish(ReplaySummary& summary) noexcept = 0;
};

class EvidenceReplayTarget final : public IReplayTarget {
public:
  EvidenceReplayTarget() noexcept;
  ~EvidenceReplayTarget();
  EvidenceReplayTarget(EvidenceReplayTarget&&) noexcept;
  EvidenceReplayTarget& operator=(EvidenceReplayTarget&&) noexcept;
  EvidenceReplayTarget(const EvidenceReplayTarget&) = delete;
  EvidenceReplayTarget& operator=(const EvidenceReplayTarget&) = delete;
  [[nodiscard]] Status consume(const ReplayEvent& event) noexcept override;
  [[nodiscard]] Status finish(ReplaySummary& summary) noexcept override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

class ReplayEngine final {
public:
  ReplayEngine();
  ~ReplayEngine();
  ReplayEngine(ReplayEngine&&) noexcept;
  ReplayEngine& operator=(ReplayEngine&&) noexcept;
  ReplayEngine(const ReplayEngine&) = delete;
  ReplayEngine& operator=(const ReplayEngine&) = delete;

  [[nodiscard]] Status prepare(const ReplayConfig& config, IReplayTarget& target,
                               IReplayPacer& pacer,
                               IModelRecomputer* recomputer = nullptr);
  [[nodiscard]] Status run();
  [[nodiscard]] Status step();
  void pause() noexcept;
  void resume() noexcept;
  [[nodiscard]] bool paused() const noexcept;
  [[nodiscard]] const ReplayManifest& manifest() const noexcept;
  [[nodiscard]] const ReplaySummary& summary() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string manifest_json(const ReplayManifest& manifest);
[[nodiscard]] std::string summary_json(const ReplaySummary& summary);
[[nodiscard]] bool write_text_file(const std::filesystem::path& path,
                                   std::string_view contents) noexcept;
[[nodiscard]] std::string_view status_name(Status status) noexcept;
[[nodiscard]] std::string_view category_name(OutputCategory category) noexcept;

} // namespace aegis::replay

#endif // AEGIS_REPLAY_REPLAY_HPP
