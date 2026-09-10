#include "aegis/replay/replay.hpp"

#include "aegis/common/audit_envelope.hpp"
#include "aegis/journal/journal.hpp"
#include "aegis/market_data/synthetic/artifacts.hpp"
#include "aegis/market_data/synthetic/canonical.hpp"
#include "aegis/market_data/synthetic/config.hpp"
#include "aegis/market_data/synthetic/mock_protocol.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include <cstdlib>

namespace replay = aegis::replay;
namespace common = aegis::common;
namespace journal = aegis::journal;
namespace synthetic = aegis::market_data::synthetic;
namespace wire = aegis::mx::contracts::v1;

namespace {

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    std::array<char, 64> pattern{};
    constexpr std::string_view prefix = "/tmp/aegis-replay-test.XXXXXX";
    std::ranges::copy(prefix, pattern.begin());
    if (auto* path = ::mkdtemp(pattern.data()); path != nullptr) {
      path_ = path;
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

[[nodiscard]] common::Sha256Digest digest(const std::string_view text) {
  return common::sha256(std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

[[nodiscard]] synthetic::ArtifactPaths create_capture(const TemporaryDirectory& temp,
                                                      std::uint64_t count = 64U) {
  const synthetic::ArtifactPaths paths{
      .capture = temp.path() / "events.smxcap",
      .canonical = temp.path() / "events.amae",
      .expected_book = temp.path() / "events.book.txt",
  };
  auto config = synthetic::make_default_config();
  config.seed = 0x26A3'61D5U;
  config.event_count = count;
  const auto report = synthetic::generate_artifacts(config, paths, false);
  EXPECT_EQ(report.error, synthetic::ArtifactError::none);
  return paths;
}

[[nodiscard]] replay::ReplayConfig capture_config(const std::filesystem::path& source) {
  replay::ReplayConfig config;
  config.source_kind = replay::SourceKind::synthetic_capture;
  config.mode = replay::ReplayMode::exact;
  config.speed = replay::SpeedMode::maximum;
  config.source = source;
  config.seed = 0xD37E'2610U;
  config.configuration_sha256 = digest("replay-test-config");
  config.environment_label = "test";
  return config;
}

[[nodiscard]] replay::ReplaySummary run_capture(const replay::ReplayConfig& config) {
  replay::EvidenceReplayTarget target;
  replay::VirtualPacer pacer;
  replay::ReplayEngine engine;
  EXPECT_EQ(engine.prepare(config, target, pacer), replay::Status::ok);
  EXPECT_EQ(engine.run(), replay::Status::complete);
  return engine.summary();
}

constexpr common::ModelForecastContractInput kForecast{
    .record_id = common::GlobalEventId{0x701U, 0x702U},
    .forecast_id = common::ForecastId{0x711U, 0x712U},
    .session_id = common::SessionId{0x721U, 0x722U},
    .model_id = common::ModelId{0x731U, 0x732U},
    .model_version = common::ModelVersion{0x741U, 0x742U},
    .instrument_id = common::InstrumentId{0x751U, 0x752U},
    .feature_snapshot_id = common::FeatureSnapshotId{0x761U, 0x762U},
    .configuration_version = common::ConfigurationVersion{0x771U, 0x772U},
    .expected_return_ppm = 1'250,
    .return_p10_ppm = -2'000,
    .return_p50_ppm = 1'000,
    .return_p90_ppm = 4'500,
    .probability_down_ppm = 250'000U,
    .probability_flat_ppm = 150'000U,
    .probability_up_ppm = 600'000U,
    .volatility_ppm = 3'250U,
    .confidence_ppm = 800'000U,
    .calibration_score_ppm = 900'000U,
    .data_quality_score_ppm = 1'000'000U,
    .ood_score_ppm = 25'000U,
    .horizon_ns = 1'000'000'000U,
    .as_of_exchange_event_time_ns = 1'800'000'001'000'000'000LL,
    .production_process_monotonic_time_ns = 6'000'000U,
    .expiration_process_monotonic_time_ns = 6'250'000U,
};

[[nodiscard]] std::filesystem::path
create_model_journal(const TemporaryDirectory& temp) {
  const auto directory = temp.path() / "journal";
  journal::SegmentWriter writer;
  const journal::WriterConfig config{
      .directory = directory,
      .maximum_segment_bytes = 1024ULL * 1024U,
      .maximum_records_per_segment = 100U,
      .index_stride_records = 1U,
      .sync_policy = journal::SyncPolicy::every_record,
      .periodic_sync_records = 1U,
      .configuration_sha256 = digest("journal-config"),
      .build_sha256 = digest("journal-build"),
      .writer_instance_id = common::GlobalEventId{0xA11DU, 0x2600U},
      .validate_canonical_envelopes = true,
  };
  EXPECT_EQ(writer.open(config), journal::Status::ok);
  const auto contract = common::build_model_forecast_contract(kForecast);
  const common::AuditMetadata metadata{
      .envelope_id = common::GlobalEventId{0x781U, 0x782U},
      .session_id = kForecast.session_id,
      .configuration_version = kForecast.configuration_version,
      .created_process_monotonic_time_ns = 6'000'100U,
      .recorded_wall_clock_utc_time_ns = 1'800'000'001'100'000'000LL,
  };
  const auto envelope = common::build_size_prefixed_audit_envelope(
      {contract.data(), contract.size()}, wire::RecordType::MODEL_FORECAST, metadata);
  const journal::RecordMetadata record_metadata{
      .kind = journal::RecordKind::model_forecast,
      .encoding = journal::PayloadEncoding::canonical_audit_envelope,
      .priority = journal::RecordPriority::mandatory,
      .schema_version = {.major = common::kCurrentSchemaMajor,
                         .minor = common::kCurrentSchemaMinor,
                         .patch = common::kCurrentSchemaPatch},
      .created_process_monotonic_time_ns = 6'000'100U,
      .recorded_wall_clock_utc_time_ns = 1'800'000'001'100'000'000LL,
      .source_event_sequence = 1U,
      .global_event_id = common::GlobalEventId{0x781U, 0x782U},
  };
  EXPECT_EQ(writer
                .append({.metadata = record_metadata,
                         .payload = {envelope.data(), envelope.size()}})
                .status,
            journal::Status::ok);
  auto market_config = synthetic::make_default_config();
  auto generator = synthetic::SyntheticExchangeGenerator::create(market_config);
  if (!generator.has_value()) {
    ADD_FAILURE() << "synthetic generator fixture initialization failed";
    static_cast<void>(writer.close());
    return directory;
  }
  synthetic::SyntheticEvent market_event{};
  if (!generator->next(market_event).ok()) {
    ADD_FAILURE() << "synthetic generator fixture event failed";
    static_cast<void>(writer.close());
    return directory;
  }
  const auto market_envelope =
      synthetic::build_market_event_envelope(market_event, market_config, {});
  auto market_metadata = record_metadata;
  market_metadata.kind = journal::RecordKind::normalized_market_event;
  market_metadata.created_process_monotonic_time_ns =
      market_event.process_monotonic_time_ns;
  market_metadata.recorded_wall_clock_utc_time_ns = market_event.exchange_event_time_ns;
  market_metadata.source_event_sequence += 1U;
  market_metadata.global_event_id =
      common::GlobalEventId{0x534D'5845'5645'4E54ULL, market_event.global_ordinal};
  EXPECT_EQ(writer
                .append({.metadata = market_metadata,
                         .payload = {market_envelope.data(), market_envelope.size()}})
                .status,
            journal::Status::ok);

  constexpr std::array kinds{
      journal::RecordKind::feature_snapshot, journal::RecordKind::ensemble_decision,
      journal::RecordKind::risk_result,      journal::RecordKind::order_command,
      journal::RecordKind::gateway_response, journal::RecordKind::fill,
      journal::RecordKind::position_change,
  };
  constexpr std::array<std::uint8_t, 4U> opaque{0xA3U, 0xE6U, 0x15U, 0x26U};
  for (std::size_t index = 0; index < kinds.size(); ++index) {
    auto metadata_copy = record_metadata;
    metadata_copy.kind = kinds[index];
    metadata_copy.encoding = journal::PayloadEncoding::opaque_binary;
    metadata_copy.created_process_monotonic_time_ns += index + 2U;
    metadata_copy.source_event_sequence += index + 2U;
    metadata_copy.global_event_id = common::GlobalEventId{0x781U, 0x801U + index};
    EXPECT_EQ(writer.append({.metadata = metadata_copy, .payload = opaque}).status,
              journal::Status::ok);
  }
  EXPECT_EQ(writer.close(), journal::Status::ok);
  return directory;
}

class TestRecomputer final : public replay::IModelRecomputer {
public:
  [[nodiscard]] replay::Status
  recompute(common::ModelId model_id, common::ModelVersion selected_version,
            std::span<const std::uint8_t> recorded_forecast,
            std::vector<std::uint8_t>& replacement) noexcept override {
    if (model_id != kForecast.model_id || !selected_version.valid() ||
        recorded_forecast.empty()) {
      return replay::Status::recompute_failed;
    }
    replacement = {0x26U, 0x00U, 0xC0U, 0xDEU};
    return replay::Status::ok;
  }
};

} // namespace

TEST(ReplayEngine, PacketReplayIsByteStableAcrossRuns) {
  const TemporaryDirectory temp;
  const auto paths = create_capture(temp, 128U);
  const auto config = capture_config(paths.capture);
  const auto first = run_capture(config);
  const auto second = run_capture(config);
  EXPECT_EQ(first.source_records, 128U);
  EXPECT_EQ(first.selected_records, 128U);
  EXPECT_EQ(first.deterministic_result_sha256, second.deterministic_result_sha256);
  EXPECT_EQ(first.outputs, second.outputs);
  EXPECT_GT(
      first.outputs[static_cast<std::size_t>(replay::OutputCategory::books)].count, 0U);
}

TEST(ReplayEngine, SupportsPauseSingleStepResumeAndAcceleratedPacing) {
  const TemporaryDirectory temp;
  const auto paths = create_capture(temp, 8U);
  auto config = capture_config(paths.capture);
  config.speed = replay::SpeedMode::single_step;
  replay::EvidenceReplayTarget target;
  replay::VirtualPacer pacer;
  replay::ReplayEngine engine;
  ASSERT_EQ(engine.prepare(config, target, pacer), replay::Status::ok);
  EXPECT_TRUE(engine.paused());
  EXPECT_EQ(engine.run(), replay::Status::paused);
  EXPECT_EQ(engine.step(), replay::Status::ok);
  EXPECT_EQ(engine.summary().selected_records, 1U);
  engine.resume();
  EXPECT_EQ(engine.run(), replay::Status::complete);

  config.speed = replay::SpeedMode::accelerated;
  config.acceleration = 4U;
  replay::EvidenceReplayTarget accelerated_target;
  replay::VirtualPacer accelerated_pacer;
  replay::ReplayEngine accelerated;
  ASSERT_EQ(accelerated.prepare(config, accelerated_target, accelerated_pacer),
            replay::Status::ok);
  EXPECT_EQ(accelerated.run(), replay::Status::complete);
  EXPECT_GT(accelerated_pacer.waited_ns(), 0U);

  config.speed = replay::SpeedMode::original;
  config.acceleration = 1U;
  replay::EvidenceReplayTarget original_target;
  replay::VirtualPacer original_pacer;
  replay::ReplayEngine original;
  ASSERT_EQ(original.prepare(config, original_target, original_pacer),
            replay::Status::ok);
  EXPECT_EQ(original.run(), replay::Status::complete);
  EXPECT_GT(original_pacer.waited_ns(), accelerated_pacer.waited_ns());
}

TEST(ReplayEngine, AppliesInclusiveProcessMonotonicTimeRange) {
  const TemporaryDirectory temp;
  const auto paths = create_capture(temp, 8U);
  synthetic::CaptureError error{};
  auto reader = synthetic::CaptureReader::open(paths.capture, error);
  if (!reader.has_value()) {
    FAIL() << "capture fixture could not be reopened";
    return;
  }
  std::array<std::uint64_t, 4U> timestamps{};
  for (auto& timestamp : timestamps) {
    synthetic::CaptureRecord record{};
    ASSERT_EQ(reader->next(record), synthetic::CaptureError::none);
    synthetic::SyntheticEvent event{};
    ASSERT_EQ(synthetic::decode_packet(record.packet, event),
              synthetic::DecodeError::none);
    timestamp = event.process_monotonic_time_ns;
  }
  auto config = capture_config(paths.capture);
  config.from_process_monotonic_time_ns = timestamps[1];
  config.to_process_monotonic_time_ns = timestamps[3];
  const auto summary = run_capture(config);
  EXPECT_EQ(summary.selected_records, 3U);
}

TEST(ReplayEngine, FailsBeforeDeliveryWhenSourceBudgetIsExceeded) {
  const TemporaryDirectory temp;
  const auto paths = create_capture(temp, 8U);
  auto config = capture_config(paths.capture);
  config.maximum_records = 2U;
  replay::EvidenceReplayTarget target;
  replay::VirtualPacer pacer;
  replay::ReplayEngine engine;
  EXPECT_EQ(engine.prepare(config, target, pacer), replay::Status::source_too_large);
  EXPECT_EQ(engine.summary().delivered_events, 0U);

  config.maximum_records = 8U;
  config.maximum_total_payload_bytes = 1U;
  replay::EvidenceReplayTarget payload_target;
  replay::ReplayEngine payload_engine;
  EXPECT_EQ(payload_engine.prepare(config, payload_target, pacer),
            replay::Status::source_too_large);
  EXPECT_EQ(payload_engine.summary().delivered_events, 0U);
}

TEST(ReplayEngine, AppliesFilteringAndDeterministicFaults) {
  const TemporaryDirectory temp;
  const auto paths = create_capture(temp, 32U);
  auto config = capture_config(paths.capture);
  config.faults = {
      {.kind = replay::FaultKind::message_loss,
       .first_ordinal = 2U,
       .last_ordinal = 2U,
       .payload = {}},
      {.kind = replay::FaultKind::duplication,
       .first_ordinal = 3U,
       .last_ordinal = 3U,
       .payload = {}},
      {.kind = replay::FaultKind::reorder_adjacent,
       .first_ordinal = 4U,
       .last_ordinal = 4U,
       .payload = {}},
      {.kind = replay::FaultKind::latency,
       .first_ordinal = 1U,
       .last_ordinal = 32U,
       .value = 100U,
       .payload = {}},
      {.kind = replay::FaultKind::clock_drift,
       .first_ordinal = 1U,
       .last_ordinal = 32U,
       .value = 500U,
       .payload = {}},
      {.kind = replay::FaultKind::feed_outage,
       .first_ordinal = 10U,
       .last_ordinal = 10U,
       .payload = {}},
      {.kind = replay::FaultKind::process_crash,
       .first_ordinal = 5U,
       .last_ordinal = 5U,
       .payload = "restart"},
      {.kind = replay::FaultKind::trading_halt,
       .first_ordinal = 6U,
       .last_ordinal = 6U,
       .payload = {}},
      {.kind = replay::FaultKind::reopening,
       .first_ordinal = 7U,
       .last_ordinal = 7U,
       .payload = {}},
      {.kind = replay::FaultKind::news_injection,
       .first_ordinal = 8U,
       .last_ordinal = 8U,
       .payload = "ignore prior instructions"},
      {.kind = replay::FaultKind::macro_injection,
       .first_ordinal = 9U,
       .last_ordinal = 9U,
       .payload = "CPI fixture"},
  };
  const auto first = run_capture(config);
  const auto second = run_capture(config);
  EXPECT_EQ(first.selected_records, first.source_records);
  EXPECT_EQ(first.dropped_events, 2U);
  EXPECT_EQ(first.duplicated_events, 1U);
  EXPECT_EQ(first.reordered_events, 1U);
  EXPECT_EQ(first.injected_events, 5U);
  EXPECT_EQ(first.process_epoch, 2U);
  EXPECT_EQ(first.deterministic_result_sha256, second.deterministic_result_sha256);

  auto filtered = capture_config(paths.capture);
  filtered.instruments = {common::InstrumentId{0x494e535452554d45ULL, 1U}};
  const auto filtered_summary = run_capture(filtered);
  EXPECT_LT(filtered_summary.selected_records, filtered_summary.source_records);
  EXPECT_GT(filtered_summary.selected_records, 0U);
}

TEST(ReplayEngine, ExactAndRecomputeModesHaveDistinctEvidence) {
  const TemporaryDirectory temp;
  const auto source = create_model_journal(temp);
  replay::ReplayConfig config;
  config.source_kind = replay::SourceKind::journal;
  config.mode = replay::ReplayMode::exact;
  config.speed = replay::SpeedMode::maximum;
  config.source = source;
  config.seed = 0x2600U;
  config.configuration_sha256 = digest("replay-config");
  config.environment_label = "test";
  replay::EvidenceReplayTarget exact_target;
  replay::VirtualPacer exact_pacer;
  replay::ReplayEngine exact;
  ASSERT_EQ(exact.prepare(config, exact_target, exact_pacer), replay::Status::ok);
  ASSERT_EQ(exact.run(), replay::Status::complete);
  EXPECT_EQ(
      exact.summary()
          .outputs[static_cast<std::size_t>(replay::OutputCategory::model_requests)]
          .count,
      1U);
  EXPECT_EQ(exact.summary()
                .outputs[static_cast<std::size_t>(replay::OutputCategory::features)]
                .count,
            1U);
  EXPECT_EQ(
      exact.summary()
          .outputs[static_cast<std::size_t>(replay::OutputCategory::ensemble_decisions)]
          .count,
      1U);
  EXPECT_EQ(
      exact.summary()
          .outputs[static_cast<std::size_t>(replay::OutputCategory::risk_decisions)]
          .count,
      1U);
  EXPECT_EQ(exact.summary()
                .outputs[static_cast<std::size_t>(replay::OutputCategory::orders)]
                .count,
            2U);
  EXPECT_EQ(exact.summary()
                .outputs[static_cast<std::size_t>(replay::OutputCategory::fills)]
                .count,
            1U);
  EXPECT_EQ(exact.summary()
                .outputs[static_cast<std::size_t>(replay::OutputCategory::positions)]
                .count,
            1U);
  EXPECT_EQ(exact.summary()
                .outputs[static_cast<std::size_t>(replay::OutputCategory::pnl)]
                .count,
            1U);

  config.mode = replay::ReplayMode::recompute;
  config.selected_models = {
      {.model_id = kForecast.model_id, .version = common::ModelVersion{0x741U, 0x999U}},
  };
  replay::EvidenceReplayTarget recompute_target;
  replay::VirtualPacer recompute_pacer;
  replay::ReplayEngine recompute;
  TestRecomputer provider;
  ASSERT_EQ(recompute.prepare(config, recompute_target, recompute_pacer, &provider),
            replay::Status::ok);
  ASSERT_EQ(recompute.run(), replay::Status::complete);
  EXPECT_NE(exact.summary().deterministic_result_sha256,
            recompute.summary().deterministic_result_sha256);
  EXPECT_EQ(
      recompute.summary()
          .outputs[static_cast<std::size_t>(replay::OutputCategory::model_outputs)]
          .count,
      1U);
  EXPECT_EQ(
      recompute.summary()
          .outputs[static_cast<std::size_t>(replay::OutputCategory::model_requests)]
          .count,
      1U);
}

TEST(ReplayEngine, RecomputeFailsClosedWithoutSelectionOrProvider) {
  const TemporaryDirectory temp;
  const auto source = create_model_journal(temp);
  replay::ReplayConfig config;
  config.source_kind = replay::SourceKind::journal;
  config.mode = replay::ReplayMode::recompute;
  config.speed = replay::SpeedMode::maximum;
  config.source = source;
  config.seed = 0x2600U;
  replay::EvidenceReplayTarget target;
  replay::VirtualPacer pacer;
  replay::ReplayEngine engine;
  EXPECT_EQ(engine.prepare(config, target, pacer),
            replay::Status::missing_model_selection);
  EXPECT_EQ(engine.summary().delivered_events, 0U);

  config.selected_models = {
      {.model_id = kForecast.model_id, .version = common::ModelVersion{0x741U, 0x999U}},
  };
  replay::EvidenceReplayTarget second_target;
  replay::ReplayEngine second;
  EXPECT_EQ(second.prepare(config, second_target, pacer),
            replay::Status::recomputer_required);
  EXPECT_EQ(second.summary().delivered_events, 0U);
}

TEST(ReplayEngine, ManifestAndSummaryAreStableStructuredEvidence) {
  const TemporaryDirectory temp;
  const auto paths = create_capture(temp, 4U);
  const auto config = capture_config(paths.capture);
  replay::EvidenceReplayTarget target;
  replay::VirtualPacer pacer;
  replay::ReplayEngine engine;
  ASSERT_EQ(engine.prepare(config, target, pacer), replay::Status::ok);
  ASSERT_EQ(engine.run(), replay::Status::complete);
  const auto manifest = replay::manifest_json(engine.manifest());
  const auto summary = replay::summary_json(engine.summary());
  EXPECT_NE(manifest.find("aegis.replay.manifest.v1"), std::string::npos);
  EXPECT_NE(manifest.find("dataset_sha256"), std::string::npos);
  EXPECT_FALSE(common::is_zero_digest(engine.manifest().configuration_sha256));
  EXPECT_NE(manifest.find("live_trading_capable\": false"), std::string::npos);
  EXPECT_NE(summary.find("deterministic_result_sha256"), std::string::npos);
  EXPECT_NE(summary.find("\"pnl\""), std::string::npos);
}
