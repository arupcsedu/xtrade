#include "aegis/replay/replay.hpp"

#include "aegis/common/audit_envelope.hpp"
#include "aegis/common/build_info.hpp"
#include "aegis/market_data/synthetic/book.hpp"
#include "aegis/market_data/synthetic/capture.hpp"
#include "aegis/market_data/synthetic/mock_protocol.hpp"
#include "aegis/time/clocks.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <thread>
#include <type_traits>
#include <utility>

namespace aegis::replay {
namespace {

namespace synthetic = aegis::market_data::synthetic;
namespace wire = aegis::common::wire;

#ifdef __GNUC__
#define AEGIS_REPLAY_NOINLINE __attribute__((noinline))
#else
#define AEGIS_REPLAY_NOINLINE
#endif

struct OwnedRecord {
  EventKind kind{EventKind::journal_record};
  std::uint64_t ordinal{};
  std::uint64_t sequence{};
  std::uint64_t timestamp_ns{};
  common::InstrumentId instrument_id;
  journal::RecordKind record_kind{journal::RecordKind::unknown};
  journal::PayloadEncoding encoding{journal::PayloadEncoding::unknown};
  common::Sha256Digest source_hash{};
  std::vector<std::uint8_t> payload;
};

[[nodiscard]] std::string digest_hex(const common::Sha256Digest& digest) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (const auto byte : digest) {
    stream << std::setw(2) << static_cast<unsigned>(byte);
  }
  return stream.str();
}

template <typename Integer>
AEGIS_REPLAY_NOINLINE void append_integer(std::vector<std::uint8_t>& bytes,
                                          const Integer value) {
  using Unsigned = std::make_unsigned_t<Integer>;
  const auto encoded = static_cast<Unsigned>(value);
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    bytes.push_back(static_cast<std::uint8_t>(encoded >> (index * 8U)));
  }
}

void append_digest(std::vector<std::uint8_t>& bytes,
                   const common::Sha256Digest& digest) {
  bytes.insert(bytes.end(), digest.begin(), digest.end());
}

[[nodiscard]] common::Sha256Digest
chain_digest(const common::Sha256Digest& previous,
             const std::span<const std::uint8_t> payload,
             const std::uint64_t discriminator = 0U) {
  std::vector<std::uint8_t> input;
  input.reserve(previous.size() + sizeof(discriminator) + payload.size());
  append_digest(input, previous);
  append_integer(input, discriminator);
  input.insert(input.end(), payload.begin(), payload.end());
  return common::sha256(input);
}

[[nodiscard]] common::Sha256Digest digest_file(const std::filesystem::path& path,
                                               bool& ok) {
  ok = false;
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    return {};
  }
  std::vector<std::uint8_t> bytes;
  std::array<char, std::size_t{64U} * 1024U> block{};
  while (stream.good()) {
    stream.read(block.data(), static_cast<std::streamsize>(block.size()));
    const auto count = stream.gcount();
    if (count > 0) {
      const auto* first = reinterpret_cast<const std::uint8_t*>(block.data());
      bytes.insert(bytes.end(), first, first + count);
    }
  }
  if (!stream.eof()) {
    return {};
  }
  ok = true;
  return common::sha256(bytes);
}

[[nodiscard]] common::InstrumentId to_instrument(const wire::InstrumentId* value) {
  if (value == nullptr) {
    return {};
  }
  return {value->high(), value->low()};
}

[[nodiscard]] common::ModelId to_model(const wire::ModelId* value) {
  if (value == nullptr) {
    return {};
  }
  return {value->high(), value->low()};
}

#ifdef __GNUC__
#pragma GCC diagnostic push
// GCC 14's optimized -Wnull-dereference analysis does not carry the preceding
// FlatBuffers verifier proof through generated table accessors. Each caller
// validates the complete envelope first, and explicit pointer checks remain.
#pragma GCC diagnostic ignored "-Wnull-dereference"
#endif

[[nodiscard]] AEGIS_REPLAY_NOINLINE std::optional<common::InstrumentId>
canonical_instrument(const common::ValidatedAuditEnvelopeView& view) {
  if (view.record == nullptr) {
    return std::nullopt;
  }
  switch (view.record->payload_type()) {
  case wire::ContractPayload::InstrumentReferenceData:
    return to_instrument(
        view.record->payload_as_InstrumentReferenceData()->instrument_id());
  case wire::ContractPayload::MarketEvent:
    return to_instrument(view.record->payload_as_MarketEvent()->instrument_id());
  case wire::ContractPayload::BookUpdate:
    return to_instrument(view.record->payload_as_BookUpdate()->instrument_id());
  case wire::ContractPayload::TradeEvent:
    return to_instrument(view.record->payload_as_TradeEvent()->instrument_id());
  case wire::ContractPayload::QuoteEvent:
    return to_instrument(view.record->payload_as_QuoteEvent()->instrument_id());
  case wire::ContractPayload::AuctionImbalance:
    return to_instrument(view.record->payload_as_AuctionImbalance()->instrument_id());
  case wire::ContractPayload::TradingStatus:
    return to_instrument(view.record->payload_as_TradingStatus()->instrument_id());
  case wire::ContractPayload::FeatureSnapshotMetadata:
    return to_instrument(
        view.record->payload_as_FeatureSnapshotMetadata()->instrument_id());
  case wire::ContractPayload::ModelForecast:
    return to_instrument(view.record->payload_as_ModelForecast()->instrument_id());
  case wire::ContractPayload::EnsembleForecast:
    return to_instrument(view.record->payload_as_EnsembleForecast()->instrument_id());
  case wire::ContractPayload::OrderIntent:
    return to_instrument(view.record->payload_as_OrderIntent()->instrument_id());
  case wire::ContractPayload::RiskDecision:
    return to_instrument(view.record->payload_as_RiskDecision()->instrument_id());
  case wire::ContractPayload::OrderEvent:
    return to_instrument(view.record->payload_as_OrderEvent()->instrument_id());
  case wire::ContractPayload::FillEvent:
    return to_instrument(view.record->payload_as_FillEvent()->instrument_id());
  case wire::ContractPayload::EventIntelligenceRecord:
    return to_instrument(
        view.record->payload_as_EventIntelligenceRecord()->instrument_id());
  case wire::ContractPayload::NONE:
  case wire::ContractPayload::PositionSnapshot:
  case wire::ContractPayload::DataQualityState:
  case wire::ContractPayload::ClockQualityState:
  case wire::ContractPayload::KillSwitchEvent:
    return std::nullopt;
  }
  return std::nullopt;
}

struct ModelProvenance {
  common::ModelId model_id;
  common::ModelVersion model_version;
  std::uint64_t feature_high{};
  std::uint64_t feature_low{};
};

[[nodiscard]] AEGIS_REPLAY_NOINLINE std::optional<ModelProvenance>
model_provenance(const common::ValidatedAuditEnvelopeView& view) {
  if (view.record == nullptr ||
      view.record->payload_type() != wire::ContractPayload::ModelForecast) {
    return std::nullopt;
  }
  const auto* forecast = view.record->payload_as_ModelForecast();
  if (forecast == nullptr || forecast->model_id() == nullptr ||
      forecast->model_version() == nullptr ||
      forecast->feature_snapshot_id() == nullptr) {
    return std::nullopt;
  }
  return ModelProvenance{
      .model_id = to_model(forecast->model_id()),
      .model_version = common::ModelVersion{forecast->model_version()->high(),
                                            forecast->model_version()->low()},
      .feature_high = forecast->feature_snapshot_id()->high(),
      .feature_low = forecast->feature_snapshot_id()->low(),
  };
}

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

[[nodiscard]] bool selected_instrument(const ReplayConfig& config,
                                       const common::InstrumentId instrument) {
  if (config.instruments.empty()) {
    return true;
  }
  return instrument.valid() &&
         std::ranges::find(config.instruments, instrument) != config.instruments.end();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] bool fault_matches(const FaultSpec& fault, const std::uint64_t ordinal,
                                 const std::uint64_t seed) noexcept {
  if (fault.every_nth == 0U || ordinal < fault.first_ordinal ||
      ordinal > fault.last_ordinal) {
    return false;
  }
  const auto offset = ordinal - fault.first_ordinal;
  return (offset + (seed % fault.every_nth)) % fault.every_nth == 0U;
}

[[nodiscard]] bool valid_config(const ReplayConfig& config) {
  if (config.source.empty() || config.seed == 0U || config.acceleration == 0U ||
      config.maximum_records == 0U || config.maximum_records > kMaximumReplayRecords ||
      config.maximum_total_payload_bytes == 0U ||
      config.maximum_total_payload_bytes > kMaximumReplayPayloadBytes ||
      config.instruments.size() > 4'096U || config.selected_models.size() > 1'024U ||
      config.faults.size() > 4'096U ||
      (config.speed != SpeedMode::accelerated && config.acceleration != 1U) ||
      (config.to_process_monotonic_time_ns != 0U &&
       config.from_process_monotonic_time_ns > config.to_process_monotonic_time_ns)) {
    return false;
  }
  for (const auto& fault : config.faults) {
    if (fault.first_ordinal == 0U || fault.last_ordinal < fault.first_ordinal ||
        fault.every_nth == 0U || fault.payload.size() > common::kMaximumContractBytes) {
      return false;
    }
    if (fault.kind == FaultKind::clock_drift &&
        (fault.value < -1'000'000 || fault.value > 1'000'000)) {
      return false;
    }
    if (fault.kind == FaultKind::latency && fault.value < 0) {
      return false;
    }
  }
  for (std::size_t index = 0; index < config.selected_models.size(); ++index) {
    if (!config.selected_models[index].model_id.valid() ||
        !config.selected_models[index].version.valid()) {
      return false;
    }
    for (std::size_t other = index + 1U; other < config.selected_models.size();
         ++other) {
      if (config.selected_models[index].model_id ==
          config.selected_models[other].model_id) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] common::Sha256Digest replay_config_digest(const ReplayConfig& config) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(256U + (config.instruments.size() * 16U) +
                (config.selected_models.size() * 32U));
  append_integer(bytes, static_cast<std::uint8_t>(config.source_kind));
  append_integer(bytes, static_cast<std::uint8_t>(config.mode));
  append_integer(bytes, static_cast<std::uint8_t>(config.speed));
  append_integer(bytes, config.seed);
  append_integer(bytes, config.acceleration);
  append_integer(bytes, config.first_sequence);
  append_integer(bytes, config.last_sequence);
  append_integer(bytes, config.from_process_monotonic_time_ns);
  append_integer(bytes, config.to_process_monotonic_time_ns);
  append_integer(bytes, config.maximum_records);
  append_integer(bytes, config.maximum_total_payload_bytes);
  for (const auto instrument : config.instruments) {
    append_integer(bytes, instrument.high());
    append_integer(bytes, instrument.low());
  }
  for (const auto& model : config.selected_models) {
    append_integer(bytes, model.model_id.high());
    append_integer(bytes, model.model_id.low());
    append_integer(bytes, model.version.high());
    append_integer(bytes, model.version.low());
  }
  for (const auto& fault : config.faults) {
    append_integer(bytes, static_cast<std::uint8_t>(fault.kind));
    append_integer(bytes, fault.first_ordinal);
    append_integer(bytes, fault.last_ordinal);
    append_integer(bytes, fault.every_nth);
    append_integer(bytes, fault.value);
    bytes.insert(bytes.end(), fault.payload.begin(), fault.payload.end());
    bytes.push_back(0U);
  }
  return common::sha256(bytes);
}

[[nodiscard]] std::optional<common::ModelVersion>
selected_version(const ReplayConfig& config, const common::ModelId model_id) {
  for (const auto& selected : config.selected_models) {
    if (selected.model_id == model_id) {
      return selected.version;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::string json_escape(const std::string_view input) {
  std::string output;
  output.reserve(input.size());
  for (const auto character : input) {
    switch (character) {
    case '\\':
      output += "\\\\";
      break;
    case '"':
      output += "\\\"";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(character) >= 0x20U) {
        output.push_back(character);
      }
      break;
    }
  }
  return output;
}

[[nodiscard]] std::string_view source_name(const SourceKind source) noexcept {
  return source == SourceKind::journal ? "journal" : "synthetic_capture";
}

[[nodiscard]] std::string_view mode_name(const ReplayMode mode) noexcept {
  return mode == ReplayMode::exact ? "exact" : "recompute_counterfactual";
}

[[nodiscard]] std::size_t category_index(const OutputCategory category) noexcept {
  return static_cast<std::size_t>(category);
}

void add_evidence(CategoryEvidence& evidence,
                  const std::span<const std::uint8_t> payload,
                  const std::uint64_t discriminator) {
  evidence.sha256 = chain_digest(evidence.sha256, payload, discriminator);
  ++evidence.count;
}

} // namespace

Status VirtualPacer::wait(const time::DurationNs duration) noexcept {
  if (duration.value() < 0) {
    return Status::invalid_configuration;
  }
  const auto amount = static_cast<std::uint64_t>(duration.value());
  if (waited_ns_ > std::numeric_limits<std::uint64_t>::max() - amount) {
    return Status::time_overflow;
  }
  waited_ns_ += amount;
  return Status::ok;
}

std::uint64_t VirtualPacer::waited_ns() const noexcept { return waited_ns_; }

Status SystemPacer::wait(const time::DurationNs duration) noexcept {
  if (duration.value() < 0) {
    return Status::invalid_configuration;
  }
  std::this_thread::sleep_for(std::chrono::nanoseconds{duration.value()});
  return Status::ok;
}

class EvidenceReplayTarget::Impl final {
public:
  // The implementation object is private to EvidenceReplayTarget; public
  // storage keeps its allocation-free state projector mechanically simple.
  // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
  std::array<CategoryEvidence, kOutputCategoryCount> evidence{};
  synthetic::BookSet books;
  bool saw_packet{false};
  std::array<std::uint64_t, synthetic::kMaximumInstruments> channel_keys{};
  std::array<std::uint64_t, synthetic::kMaximumInstruments> channel_sequences{};
  std::size_t channel_count{};
  // NOLINTEND(misc-non-private-member-variables-in-classes)

  [[nodiscard]] bool accept_sequence(const synthetic::SyntheticEvent& event) noexcept {
    const auto key =
        (static_cast<std::uint64_t>(event.venue_number) << 32U) | event.channel_number;
    for (std::size_t index = 0; index < channel_count; ++index) {
      if (channel_keys[index] == key) {
        if (event.channel_sequence <= channel_sequences[index]) {
          return false;
        }
        channel_sequences[index] = event.channel_sequence;
        return true;
      }
    }
    if (channel_count >= channel_keys.size()) {
      return false;
    }
    channel_keys[channel_count] = key;
    channel_sequences[channel_count] = event.channel_sequence;
    ++channel_count;
    return true;
  }
};

EvidenceReplayTarget::EvidenceReplayTarget() noexcept
    : impl_(std::make_unique<Impl>()) {}
EvidenceReplayTarget::~EvidenceReplayTarget() = default;
EvidenceReplayTarget::EvidenceReplayTarget(EvidenceReplayTarget&&) noexcept = default;
EvidenceReplayTarget&
EvidenceReplayTarget::operator=(EvidenceReplayTarget&&) noexcept = default;

Status EvidenceReplayTarget::consume(const ReplayEvent& event) noexcept {
  if (event.kind == EventKind::synthetic_packet) {
    synthetic::SyntheticEvent decoded{};
    if (synthetic::decode_packet(event.payload, decoded) !=
        synthetic::DecodeError::none) {
      return Status::decode_failed;
    }
    impl_->saw_packet = true;
    if (!impl_->accept_sequence(decoded)) {
      return Status::ok;
    }
    if (impl_->books.find(decoded.venue_number, decoded.instrument_number) == nullptr &&
        !impl_->books.register_instrument(decoded.venue_number,
                                          decoded.instrument_number)) {
      return Status::target_failed;
    }
    // Fault-injected gaps and reordering can make a downstream book invalid.
    // The evidence target preserves the last deterministically applicable state;
    // a production adapter additionally publishes its explicit validity state.
    static_cast<void>(impl_->books.apply(decoded));
    return Status::ok;
  }

  if (event.kind == EventKind::trading_halt || event.kind == EventKind::reopening) {
    add_evidence(impl_->evidence[category_index(OutputCategory::books)], event.payload,
                 static_cast<std::uint64_t>(event.kind));
    return Status::ok;
  }
  if (event.kind != EventKind::journal_record) {
    return Status::ok;
  }

  const auto provenance_payload =
      event.counterfactual ? event.source_payload : event.payload;
  if ((event.encoding == journal::PayloadEncoding::canonical_audit_envelope ||
       event.counterfactual) &&
      !provenance_payload.empty()) {
    common::ValidatedAuditEnvelopeView view{};
    if (!common::validate_size_prefixed_audit_envelope(provenance_payload, &view)
             .ok()) {
      return Status::canonical_validation_failed;
    }
    const auto provenance = model_provenance(view);
    if (provenance.has_value()) {
      std::vector<std::uint8_t> request;
      request.reserve(64U);
      append_integer(request, provenance->feature_high);
      append_integer(request, provenance->feature_low);
      append_integer(request, provenance->model_id.high());
      append_integer(request, provenance->model_id.low());
      append_integer(request, provenance->model_version.high());
      append_integer(request, provenance->model_version.low());
      add_evidence(impl_->evidence[category_index(OutputCategory::model_requests)],
                   request, event.source_sequence);
    }
  }

  std::optional<OutputCategory> category;
  switch (event.record_kind) {
  case journal::RecordKind::normalized_market_event:
  case journal::RecordKind::book_validity_change:
    category = OutputCategory::books;
    break;
  case journal::RecordKind::feature_snapshot:
    category = OutputCategory::features;
    break;
  case journal::RecordKind::model_forecast:
    category = OutputCategory::model_outputs;
    break;
  case journal::RecordKind::ensemble_decision:
    category = OutputCategory::ensemble_decisions;
    break;
  case journal::RecordKind::risk_result:
    category = OutputCategory::risk_decisions;
    break;
  case journal::RecordKind::order_command:
  case journal::RecordKind::gateway_response:
    category = OutputCategory::orders;
    break;
  case journal::RecordKind::fill:
    category = OutputCategory::fills;
    break;
  case journal::RecordKind::position_change:
    category = OutputCategory::positions;
    break;
  case journal::RecordKind::raw_packet_metadata:
  case journal::RecordKind::kill_switch:
  case journal::RecordKind::configuration_change:
  case journal::RecordKind::operator_action:
  case journal::RecordKind::data_quality:
  case journal::RecordKind::clock_quality:
  case journal::RecordKind::unknown:
    break;
  }
  if (category.has_value()) {
    add_evidence(impl_->evidence[category_index(*category)], event.payload,
                 event.source_sequence);
  }
  if (event.record_kind == journal::RecordKind::position_change) {
    add_evidence(impl_->evidence[category_index(OutputCategory::pnl)], event.payload,
                 event.source_sequence);
  }
  return Status::ok;
}

Status EvidenceReplayTarget::finish(ReplaySummary& summary) noexcept {
  summary.outputs = impl_->evidence;
  if (impl_->saw_packet) {
    std::vector<std::uint8_t> state;
    state.reserve(16U);
    append_integer(state, impl_->books.stable_hash());
    append_integer(state, static_cast<std::uint64_t>(impl_->books.size()));
    auto& books = summary.outputs[category_index(OutputCategory::books)];
    books.sha256 = common::sha256(state);
    books.count = impl_->books.size();
  }
  return Status::ok;
}

class ReplayEngine::Impl final {
public:
  // The outer ReplayEngine owns this private implementation and intentionally
  // accesses its state directly at the non-hot orchestration boundary.
  // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
  ReplayConfig config;
  IReplayTarget* target{};
  IReplayPacer* pacer{};
  IModelRecomputer* recomputer{};
  std::vector<OwnedRecord> records;
  ReplayManifest manifest_value;
  ReplaySummary summary_value;
  std::size_t cursor{};
  bool is_paused{};
  bool finished{};
  std::uint64_t previous_scheduled_ns{};
  std::uint64_t first_source_ns{};
  std::uint64_t total_payload_bytes{};
  time::SimulatedClock clock{time::WallClockTimeNs{0}, time::MonotonicTimeNs{0}};
  // NOLINTEND(misc-non-private-member-variables-in-classes)

  [[nodiscard]] Status load_journal() {
    common::Sha256Digest dataset{};
    std::uint64_t replayed{};
    bool too_large = false;
    const auto journal_status = journal::replay_verified(
        config.source, config.first_sequence, config.last_sequence,
        [&](const journal::PersistedRecord& record) {
          ++summary_value.source_records;
          if (summary_value.source_records > kMaximumReplayRecords) {
            too_large = true;
            return journal::Status::invalid_argument;
          }
          dataset =
              chain_digest(dataset, record.record_sha256, record.journal_sequence);
          const auto timestamp = record.metadata.created_process_monotonic_time_ns;
          if (timestamp < config.from_process_monotonic_time_ns ||
              (config.to_process_monotonic_time_ns != 0U &&
               timestamp > config.to_process_monotonic_time_ns)) {
            return journal::Status::ok;
          }
          common::InstrumentId instrument{};
          if (record.metadata.encoding ==
              journal::PayloadEncoding::canonical_audit_envelope) {
            common::ValidatedAuditEnvelopeView view{};
            if (!common::validate_size_prefixed_audit_envelope(record.payload, &view)
                     .ok()) {
              return journal::Status::invalid_payload;
            }
            instrument = canonical_instrument(view).value_or(common::InstrumentId{});
          }
          if (!selected_instrument(config, instrument)) {
            return journal::Status::ok;
          }
          if (records.size() >= config.maximum_records ||
              record.payload.size() >
                  config.maximum_total_payload_bytes - total_payload_bytes) {
            too_large = true;
            return journal::Status::invalid_argument;
          }
          records.push_back(OwnedRecord{
              .kind = EventKind::journal_record,
              .ordinal = record.journal_sequence,
              .sequence = record.journal_sequence,
              .timestamp_ns = timestamp,
              .instrument_id = instrument,
              .record_kind = record.metadata.kind,
              .encoding = record.metadata.encoding,
              .source_hash = record.record_sha256,
              .payload = record.payload,
          });
          total_payload_bytes += record.payload.size();
          return journal::Status::ok;
        },
        replayed);
    if (too_large) {
      return Status::source_too_large;
    }
    if (journal_status != journal::Status::ok) {
      return journal_status == journal::Status::invalid_payload
                 ? Status::canonical_validation_failed
                 : Status::source_invalid;
    }
    manifest_value.dataset_sha256 = dataset;
    return Status::ok;
  }

  [[nodiscard]] Status load_capture() {
    bool digest_ok = false;
    manifest_value.dataset_sha256 = digest_file(config.source, digest_ok);
    if (!digest_ok) {
      return Status::source_open_failed;
    }
    synthetic::CaptureError open_error{};
    auto reader = synthetic::CaptureReader::open(config.source, open_error);
    if (!reader.has_value()) {
      return Status::source_invalid;
    }
    synthetic::CaptureRecord capture{};
    const auto packet_count = reader->header().physical_packet_count;
    for (std::uint64_t packet_index = 0U; packet_index < packet_count; ++packet_index) {
      const auto read = reader->next(capture);
      if (read == synthetic::CaptureError::none) {
        ++summary_value.source_records;
        if (summary_value.source_records > kMaximumReplayRecords) {
          return Status::source_too_large;
        }
        synthetic::SyntheticEvent decoded{};
        if (synthetic::decode_packet(capture.packet, decoded) !=
            synthetic::DecodeError::none) {
          return Status::decode_failed;
        }
        const auto timestamp = decoded.process_monotonic_time_ns;
        const common::InstrumentId instrument{0x494e535452554d45ULL,
                                              decoded.instrument_number};
        if (timestamp < config.from_process_monotonic_time_ns ||
            (config.to_process_monotonic_time_ns != 0U &&
             timestamp > config.to_process_monotonic_time_ns) ||
            !selected_instrument(config, instrument)) {
          continue;
        }
        if (records.size() >= config.maximum_records ||
            capture.packet.size() >
                config.maximum_total_payload_bytes - total_payload_bytes) {
          return Status::source_too_large;
        }
        const auto bytes = std::span<const std::uint8_t>{capture.packet};
        records.push_back(OwnedRecord{
            .kind = EventKind::synthetic_packet,
            .ordinal = summary_value.source_records,
            .sequence = decoded.channel_sequence,
            .timestamp_ns = timestamp,
            .instrument_id = instrument,
            .record_kind = journal::RecordKind::raw_packet_metadata,
            .encoding = journal::PayloadEncoding::synthetic_raw_packet_metadata,
            .source_hash = common::sha256(bytes),
            .payload = std::vector<std::uint8_t>{bytes.begin(), bytes.end()},
        });
        total_payload_bytes += capture.packet.size();
        continue;
      }
      return Status::source_invalid;
    }
    return reader->finish() == synthetic::CaptureError::none ? Status::ok
                                                             : Status::source_invalid;
  }

  [[nodiscard]] Status preflight_recompute() const {
    if (config.mode != ReplayMode::recompute) {
      return Status::ok;
    }
    for (const auto& record : records) {
      if (record.record_kind != journal::RecordKind::model_forecast) {
        continue;
      }
      common::ValidatedAuditEnvelopeView view{};
      if (!common::validate_size_prefixed_audit_envelope(record.payload, &view).ok()) {
        return Status::canonical_validation_failed;
      }
      const auto provenance = model_provenance(view);
      if (!provenance.has_value()) {
        return Status::canonical_validation_failed;
      }
      if (!selected_version(config, provenance->model_id).has_value()) {
        return Status::missing_model_selection;
      }
      if (recomputer == nullptr) {
        return Status::recomputer_required;
      }
    }
    return Status::ok;
  }

  void reorder_records() {
    for (const auto& fault : config.faults) {
      if (fault.kind != FaultKind::reorder_adjacent) {
        continue;
      }
      for (std::size_t index = 0; index + 1U < records.size(); ++index) {
        if (fault_matches(fault, records[index].ordinal, config.seed)) {
          std::swap(records[index], records[index + 1U]);
          ++summary_value.reordered_events;
          ++index;
        }
      }
    }
  }

  [[nodiscard]] Status calculate_schedule(const OwnedRecord& record,
                                          std::uint64_t& scheduled) const noexcept {
    scheduled = record.timestamp_ns;
    for (const auto& fault : config.faults) {
      if (!fault_matches(fault, record.ordinal, config.seed)) {
        continue;
      }
      if (fault.kind == FaultKind::latency) {
        const auto value = static_cast<std::uint64_t>(fault.value);
        if (scheduled > std::numeric_limits<std::uint64_t>::max() - value) {
          return Status::time_overflow;
        }
        scheduled += value;
      } else if (fault.kind == FaultKind::clock_drift &&
                 record.timestamp_ns >= first_source_ns) {
        const auto elapsed = record.timestamp_ns - first_source_ns;
        const auto magnitude =
            static_cast<std::uint64_t>(fault.value < 0 ? -fault.value : fault.value);
        const auto whole = elapsed / 1'000'000'000U;
        const auto remainder = elapsed % 1'000'000'000U;
        if (whole > std::numeric_limits<std::uint64_t>::max() /
                        std::max<std::uint64_t>(magnitude, 1U)) {
          return Status::time_overflow;
        }
        const auto adjustment =
            (whole * magnitude) + ((remainder * magnitude) / 1'000'000'000U);
        if (fault.value >= 0) {
          if (scheduled > std::numeric_limits<std::uint64_t>::max() - adjustment) {
            return Status::time_overflow;
          }
          scheduled += adjustment;
        } else if (scheduled < adjustment) {
          return Status::time_overflow;
        } else {
          scheduled -= adjustment;
        }
      }
    }
    return Status::ok;
  }

  [[nodiscard]] Status advance_clock(const std::uint64_t scheduled) noexcept {
    if (cursor == 0U) {
      clock.set_monotonic(time::MonotonicTimeNs{scheduled});
      previous_scheduled_ns = scheduled;
      return Status::ok;
    }
    const auto source_delta =
        scheduled > previous_scheduled_ns ? scheduled - previous_scheduled_ns : 0U;
    previous_scheduled_ns = std::max(previous_scheduled_ns, scheduled);
    const auto replay_delta = config.speed == SpeedMode::accelerated
                                  ? source_delta / config.acceleration
                                  : source_delta;
    if (replay_delta >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return Status::time_overflow;
    }
    if ((config.speed == SpeedMode::original ||
         config.speed == SpeedMode::accelerated) &&
        replay_delta != 0U) {
      const auto status =
          pacer->wait(time::DurationNs{static_cast<std::int64_t>(replay_delta)});
      if (status != Status::ok) {
        return status;
      }
    }
    return clock.advance(time::DurationNs{static_cast<std::int64_t>(replay_delta)}) ==
                   time::TimeError::none
               ? Status::ok
               : Status::time_overflow;
  }

  [[nodiscard]] Status deliver(ReplayEvent event) noexcept {
    const auto now = clock.monotonic_now();
    if (!now.ok()) {
      return Status::time_overflow;
    }
    event.replay_process_monotonic_time_ns = now.value.value();
    event.process_epoch = summary_value.process_epoch;
    const auto status = target->consume(event);
    if (status != Status::ok) {
      return status;
    }
    std::vector<std::uint8_t> evidence;
    evidence.reserve(64U + event.payload.size() + event.injected_text.size());
    append_integer(evidence, static_cast<std::uint8_t>(event.kind));
    append_integer(evidence, event.source_ordinal);
    append_integer(evidence, event.source_sequence);
    append_integer(evidence, event.replay_process_monotonic_time_ns);
    append_integer(evidence, event.process_epoch);
    append_integer(evidence, static_cast<std::uint8_t>(event.duplicate));
    append_integer(evidence, static_cast<std::uint8_t>(event.counterfactual));
    evidence.insert(evidence.end(), event.payload.begin(), event.payload.end());
    evidence.insert(evidence.end(), event.injected_text.begin(),
                    event.injected_text.end());
    summary_value.event_stream_sha256 =
        chain_digest(summary_value.event_stream_sha256, evidence,
                     summary_value.delivered_events + 1U);
    ++summary_value.delivered_events;
    return Status::ok;
  }

  [[nodiscard]] Status deliver_injections(const OwnedRecord& record) noexcept {
    for (const auto& fault : config.faults) {
      if (!fault_matches(fault, record.ordinal, config.seed)) {
        continue;
      }
      EventKind kind{};
      switch (fault.kind) {
      case FaultKind::process_crash:
        kind = EventKind::process_crash;
        ++summary_value.process_epoch;
        break;
      case FaultKind::trading_halt:
        kind = EventKind::trading_halt;
        break;
      case FaultKind::reopening:
        kind = EventKind::reopening;
        break;
      case FaultKind::news_injection:
        kind = EventKind::news;
        break;
      case FaultKind::macro_injection:
        kind = EventKind::macro;
        break;
      case FaultKind::latency:
      case FaultKind::message_loss:
      case FaultKind::duplication:
      case FaultKind::reorder_adjacent:
      case FaultKind::clock_drift:
      case FaultKind::feed_outage:
        continue;
      }
      const auto status = deliver(ReplayEvent{
          .kind = kind,
          .source_ordinal = record.ordinal,
          .source_sequence = record.sequence,
          .source_process_monotonic_time_ns = record.timestamp_ns,
          .instrument_id = record.instrument_id,
          .payload = {},
          .source_payload = {},
          .injected_text = fault.payload,
      });
      if (status != Status::ok) {
        return status;
      }
      ++summary_value.injected_events;
    }
    return Status::ok;
  }

  [[nodiscard]] bool dropped(const OwnedRecord& record) const noexcept {
    return std::ranges::any_of(config.faults, [&](const FaultSpec& fault) {
      const auto market_source =
          record.kind == EventKind::synthetic_packet ||
          record.record_kind == journal::RecordKind::normalized_market_event ||
          record.record_kind == journal::RecordKind::raw_packet_metadata;
      return fault_matches(fault, record.ordinal, config.seed) &&
             (fault.kind == FaultKind::message_loss ||
              (fault.kind == FaultKind::feed_outage && market_source));
    });
  }

  [[nodiscard]] bool duplicated(const OwnedRecord& record) const noexcept {
    return std::ranges::any_of(config.faults, [&](const FaultSpec& fault) {
      return fault.kind == FaultKind::duplication &&
             fault_matches(fault, record.ordinal, config.seed);
    });
  }

  [[nodiscard]] Status deliver_record(const OwnedRecord& record, const bool duplicate) {
    std::vector<std::uint8_t> replacement;
    auto payload = std::span<const std::uint8_t>{record.payload};
    auto encoding = record.encoding;
    bool counterfactual = false;
    if (config.mode == ReplayMode::recompute &&
        record.record_kind == journal::RecordKind::model_forecast) {
      common::ValidatedAuditEnvelopeView view{};
      if (!common::validate_size_prefixed_audit_envelope(payload, &view).ok() ||
          view.record->payload_type() != wire::ContractPayload::ModelForecast) {
        return Status::canonical_validation_failed;
      }
      const auto provenance = model_provenance(view);
      if (!provenance.has_value()) {
        return Status::canonical_validation_failed;
      }
      const auto model_id = provenance->model_id;
      const auto version = selected_version(config, model_id);
      if (!version.has_value()) {
        return Status::missing_model_selection;
      }
      if (recomputer == nullptr) {
        return Status::recomputer_required;
      }
      const auto status =
          recomputer->recompute(model_id, *version, payload, replacement);
      if (status != Status::ok || replacement.empty()) {
        return Status::recompute_failed;
      }
      payload = replacement;
      encoding = journal::PayloadEncoding::opaque_binary;
      counterfactual = true;
    }
    return deliver(ReplayEvent{
        .kind = record.kind,
        .source_ordinal = record.ordinal,
        .source_sequence = record.sequence,
        .source_process_monotonic_time_ns = record.timestamp_ns,
        .instrument_id = record.instrument_id,
        .record_kind = record.record_kind,
        .encoding = encoding,
        .payload = payload,
        .source_payload = record.payload,
        .duplicate = duplicate,
        .counterfactual = counterfactual,
        .injected_text = {},
    });
  }

  [[nodiscard]] Status step_one() {
    if (finished) {
      return Status::complete;
    }
    if (cursor >= records.size()) {
      return finalize();
    }
    const auto& record = records[cursor];
    std::uint64_t scheduled{};
    auto status = calculate_schedule(record, scheduled);
    if (status == Status::ok) {
      status = advance_clock(scheduled);
    }
    if (status == Status::ok) {
      status = deliver_injections(record);
    }
    if (status == Status::ok && dropped(record)) {
      ++summary_value.dropped_events;
    } else if (status == Status::ok) {
      status = deliver_record(record, false);
      if (status == Status::ok && duplicated(record)) {
        status = deliver_record(record, true);
        if (status == Status::ok) {
          ++summary_value.duplicated_events;
        }
      }
    }
    ++cursor;
    summary_value.selected_records = cursor;
    if (status != Status::ok) {
      summary_value.status = status;
      return status;
    }
    if (cursor >= records.size()) {
      return finalize();
    }
    return Status::ok;
  }

  [[nodiscard]] Status finalize() {
    if (finished) {
      return Status::complete;
    }
    auto status = target->finish(summary_value);
    if (status != Status::ok) {
      summary_value.status = status;
      return status;
    }
    const auto now = clock.monotonic_now();
    if (!now.ok()) {
      summary_value.status = Status::time_overflow;
      return summary_value.status;
    }
    summary_value.final_replay_process_monotonic_time_ns = now.value.value();
    std::vector<std::uint8_t> result;
    result.reserve(512U);
    append_digest(result, manifest_value.dataset_sha256);
    append_digest(result, manifest_value.configuration_sha256);
    append_digest(result, summary_value.event_stream_sha256);
    append_integer(result, summary_value.process_epoch);
    append_integer(result, summary_value.delivered_events);
    append_integer(result, summary_value.dropped_events);
    for (const auto& output : summary_value.outputs) {
      append_integer(result, output.count);
      append_digest(result, output.sha256);
    }
    summary_value.deterministic_result_sha256 = common::sha256(result);
    summary_value.status = Status::complete;
    finished = true;
    return Status::complete;
  }
};

ReplayEngine::ReplayEngine() : impl_(std::make_unique<Impl>()) {}
ReplayEngine::~ReplayEngine() = default;
ReplayEngine::ReplayEngine(ReplayEngine&&) noexcept = default;
ReplayEngine& ReplayEngine::operator=(ReplayEngine&&) noexcept = default;

Status ReplayEngine::prepare(const ReplayConfig& config, IReplayTarget& target,
                             IReplayPacer& pacer, IModelRecomputer* recomputer) {
  if (!valid_config(config)) {
    return Status::invalid_configuration;
  }
  impl_ = std::make_unique<Impl>();
  impl_->config = config;
  impl_->target = &target;
  impl_->pacer = &pacer;
  impl_->recomputer = recomputer;
  auto status = config.source_kind == SourceKind::journal ? impl_->load_journal()
                                                          : impl_->load_capture();
  if (status != Status::ok) {
    impl_->summary_value.status = status;
    return status;
  }
  status = impl_->preflight_recompute();
  if (status != Status::ok) {
    impl_->summary_value.status = status;
    return status;
  }
  impl_->reorder_records();
  impl_->summary_value.selected_records = 0U;
  if (!impl_->records.empty()) {
    impl_->first_source_ns = impl_->records.front().timestamp_ns;
  }
  const auto& build = common::current_build_info();
  impl_->manifest_value.configuration_sha256 =
      common::is_zero_digest(config.configuration_sha256) ? replay_config_digest(config)
                                                          : config.configuration_sha256;
  impl_->manifest_value.seed = config.seed;
  impl_->manifest_value.source_kind = config.source_kind;
  impl_->manifest_value.mode = config.mode;
  impl_->manifest_value.selected_models = config.selected_models;
  impl_->manifest_value.project = build.project;
  impl_->manifest_value.version = build.version;
  impl_->manifest_value.source_revision = build.source_revision;
  impl_->manifest_value.compiler =
      std::string{build.compiler_id} + " " + std::string{build.compiler_version};
  impl_->manifest_value.build_type = build.build_type;
  impl_->manifest_value.environment_label = config.environment_label;
  impl_->manifest_value.live_trading_capable = build.live_trading_capable;
  auto serialized = manifest_json(impl_->manifest_value);
  impl_->manifest_value.manifest_sha256 = common::sha256(std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>(serialized.data()), serialized.size()});
  if (config.speed == SpeedMode::single_step) {
    impl_->is_paused = true;
  }
  return Status::ok;
}

Status ReplayEngine::run() {
  if (impl_->target == nullptr) {
    return Status::invalid_configuration;
  }
  if (impl_->is_paused) {
    return Status::paused;
  }
  for (;;) {
    const auto status = impl_->step_one();
    if (status != Status::ok) {
      return status;
    }
    if (impl_->is_paused) {
      return Status::paused;
    }
  }
}

Status ReplayEngine::step() {
  if (impl_->target == nullptr) {
    return Status::invalid_configuration;
  }
  return impl_->step_one();
}

void ReplayEngine::pause() noexcept { impl_->is_paused = true; }
void ReplayEngine::resume() noexcept { impl_->is_paused = false; }
bool ReplayEngine::paused() const noexcept { return impl_->is_paused; }
const ReplayManifest& ReplayEngine::manifest() const noexcept {
  return impl_->manifest_value;
}
const ReplaySummary& ReplayEngine::summary() const noexcept {
  return impl_->summary_value;
}

// Escaped JSON fragments keep punctuation visible beside streamed values.
// NOLINTBEGIN(modernize-raw-string-literal)
std::string manifest_json(const ReplayManifest& manifest) {
  std::ostringstream stream;
  stream << "{\n"
         << "  \"schema\": \"aegis.replay.manifest.v1\",\n"
         << "  \"dataset_sha256\": \"" << digest_hex(manifest.dataset_sha256) << "\",\n"
         << "  \"configuration_sha256\": \""
         << digest_hex(manifest.configuration_sha256) << "\",\n"
         << "  \"manifest_sha256\": \"" << digest_hex(manifest.manifest_sha256)
         << "\",\n"
         << "  \"seed\": " << manifest.seed << ",\n"
         << "  \"source_kind\": \"" << source_name(manifest.source_kind) << "\",\n"
         << "  \"mode\": \"" << mode_name(manifest.mode) << "\",\n"
         << "  \"selected_models\": [";
  for (std::size_t index = 0; index < manifest.selected_models.size(); ++index) {
    if (index != 0U) {
      stream << ',';
    }
    const auto model = common::to_hex(manifest.selected_models[index].model_id);
    const auto version = common::to_hex(manifest.selected_models[index].version);
    stream << "{\"model_id\":\"" << model.data() << "\",\"version\":\""
           << version.data() << "\"}";
  }
  stream << "],\n"
         << "  \"environment\": {\"project\": \"" << json_escape(manifest.project)
         << "\", \"version\": \"" << json_escape(manifest.version)
         << "\", \"source_revision\": \"" << json_escape(manifest.source_revision)
         << "\", \"compiler\": \"" << json_escape(manifest.compiler)
         << "\", \"build_type\": \"" << json_escape(manifest.build_type)
         << "\", \"label\": \"" << json_escape(manifest.environment_label)
         << "\", \"live_trading_capable\": "
         << (manifest.live_trading_capable ? "true" : "false") << "}\n"
         << "}\n";
  return stream.str();
}

std::string summary_json(const ReplaySummary& summary) {
  std::ostringstream stream;
  stream << "{\n"
         << "  \"schema\": \"aegis.replay.summary.v1\",\n"
         << "  \"status\": \"" << status_name(summary.status) << "\",\n"
         << "  \"source_records\": " << summary.source_records << ",\n"
         << "  \"selected_records\": " << summary.selected_records << ",\n"
         << "  \"delivered_events\": " << summary.delivered_events << ",\n"
         << "  \"dropped_events\": " << summary.dropped_events << ",\n"
         << "  \"duplicated_events\": " << summary.duplicated_events << ",\n"
         << "  \"reordered_events\": " << summary.reordered_events << ",\n"
         << "  \"injected_events\": " << summary.injected_events << ",\n"
         << "  \"process_epoch\": " << summary.process_epoch << ",\n"
         << "  \"final_replay_process_monotonic_time_ns\": "
         << summary.final_replay_process_monotonic_time_ns << ",\n"
         << "  \"outputs\": {\n";
  for (std::size_t index = 0; index < summary.outputs.size(); ++index) {
    const auto category = static_cast<OutputCategory>(index);
    stream << "    \"" << category_name(category)
           << "\": {\"count\": " << summary.outputs[index].count << ", \"sha256\": \""
           << digest_hex(summary.outputs[index].sha256) << "\"}"
           << (index + 1U == summary.outputs.size() ? "\n" : ",\n");
  }
  stream << "  },\n"
         << "  \"event_stream_sha256\": \"" << digest_hex(summary.event_stream_sha256)
         << "\",\n"
         << "  \"deterministic_result_sha256\": \""
         << digest_hex(summary.deterministic_result_sha256) << "\"\n"
         << "}\n";
  return stream.str();
}
// NOLINTEND(modernize-raw-string-literal)

bool write_text_file(const std::filesystem::path& path,
                     const std::string_view contents) noexcept {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.is_open()) {
    return false;
  }
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  stream.flush();
  return stream.good();
}

std::string_view status_name(const Status status) noexcept {
  switch (status) {
  case Status::ok:
    return "ok";
  case Status::paused:
    return "paused";
  case Status::complete:
    return "complete";
  case Status::invalid_configuration:
    return "invalid_configuration";
  case Status::source_open_failed:
    return "source_open_failed";
  case Status::source_invalid:
    return "source_invalid";
  case Status::source_too_large:
    return "source_too_large";
  case Status::canonical_validation_failed:
    return "canonical_validation_failed";
  case Status::decode_failed:
    return "decode_failed";
  case Status::target_failed:
    return "target_failed";
  case Status::time_overflow:
    return "time_overflow";
  case Status::missing_model_selection:
    return "missing_model_selection";
  case Status::recomputer_required:
    return "recomputer_required";
  case Status::recompute_failed:
    return "recompute_failed";
  case Status::io_error:
    return "io_error";
  }
  return "unknown";
}

std::string_view category_name(const OutputCategory category) noexcept {
  switch (category) {
  case OutputCategory::books:
    return "books";
  case OutputCategory::features:
    return "features";
  case OutputCategory::model_requests:
    return "model_requests";
  case OutputCategory::model_outputs:
    return "model_outputs";
  case OutputCategory::ensemble_decisions:
    return "ensemble_decisions";
  case OutputCategory::risk_decisions:
    return "risk_decisions";
  case OutputCategory::orders:
    return "orders";
  case OutputCategory::fills:
    return "fills";
  case OutputCategory::positions:
    return "positions";
  case OutputCategory::pnl:
    return "pnl";
  }
  return "unknown";
}

#undef AEGIS_REPLAY_NOINLINE

} // namespace aegis::replay
