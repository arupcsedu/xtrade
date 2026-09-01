#include "aegis/market_data/synthetic/artifacts.hpp"

#include "aegis/common/audit_envelope.hpp"
#include "aegis/common/sha256.hpp"
#include "aegis/market_data/synthetic/canonical.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <limits>
#include <ostream>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace aegis::market_data::synthetic {
namespace {

struct ChannelReplayState {
  std::uint32_t venue_number{};
  std::uint32_t channel_number{};
  std::uint64_t last_sequence{};
  std::int64_t last_capture_time_ns{};
  bool valid{true};
};

[[nodiscard]] bool ensure_parent(const std::filesystem::path& path) noexcept {
  const auto parent = path.parent_path();
  if (parent.empty()) {
    return true;
  }
  std::error_code error;
  std::filesystem::create_directories(parent, error);
  return !error;
}

[[nodiscard]] ArtifactError write_packet(CaptureWriter& writer,
                                         const EncodedPacket& packet,
                                         const std::int64_t capture_time,
                                         GenerationReport& report) noexcept {
  const auto error =
      writer.write(CaptureRecord{.capture_time_ns = capture_time, .packet = packet});
  if (error != CaptureError::none) {
    report.capture_error = error;
    return ArtifactError::capture_failed;
  }
  ++report.physical_packets;
  return ArtifactError::none;
}

[[nodiscard]] ChannelReplayState*
find_channel(std::array<ChannelReplayState, kMaximumInstruments>& channels,
             std::size_t& channel_count, const SyntheticEvent& event) noexcept {
  for (std::size_t index = 0; index < channel_count; ++index) {
    if (channels[index].venue_number == event.venue_number &&
        channels[index].channel_number == event.channel_number) {
      return &channels[index];
    }
  }
  if (channel_count >= channels.size()) {
    return nullptr;
  }
  channels[channel_count].venue_number = event.venue_number;
  channels[channel_count].channel_number = event.channel_number;
  return &channels[channel_count++];
}

[[nodiscard]] bool final_book_required(const Scenario scenario) noexcept {
  return scenario != Scenario::missing_sequence &&
         scenario != Scenario::out_of_order_packet;
}

[[nodiscard]] bool elapsed_exceeds(const std::int64_t current, const std::int64_t prior,
                                   const std::uint64_t threshold) noexcept {
  return current > prior && std::cmp_greater(current - prior, threshold);
}

} // namespace

// This offline transaction coordinator retains exact first-failure provenance
// and leaves a failed capture detectably unfinalized.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
GenerationReport generate_artifacts(const GeneratorConfig& config,
                                    const ArtifactPaths& paths,
                                    const bool write_canonical_stream) {
  GenerationReport report{.configuration_hash = config_hash(config)};
  if (validate_config(config) != ConfigError::none ||
      paths.capture == paths.expected_book ||
      (write_canonical_stream &&
       (paths.capture == paths.canonical || paths.canonical == paths.expected_book)) ||
      !ensure_parent(paths.capture) || !ensure_parent(paths.expected_book) ||
      (write_canonical_stream && !ensure_parent(paths.canonical))) {
    report.error = ArtifactError::invalid_config;
    return report;
  }
  auto generator = SyntheticExchangeGenerator::create(config);
  const auto physical_count = expected_physical_packet_count(config);
  if (!generator.has_value() || !physical_count.has_value()) {
    report.error = ArtifactError::invalid_config;
    return report;
  }
  const CaptureHeader header{
      .scenario = config.scenario,
      .seed = config.seed,
      .configuration_hash = report.configuration_hash,
      .logical_event_count = config.event_count,
      .physical_packet_count = *physical_count,
      .start_exchange_time_ns = config.session_start_exchange_time_ns,
      .stale_threshold_ns = config.stale_threshold_ns,
  };
  auto capture = CaptureWriter::create(paths.capture, header);
  if (!capture.has_value()) {
    report.error = ArtifactError::capture_failed;
    report.capture_error = CaptureError::open_failed;
    return report;
  }
  std::ofstream canonical;
  if (write_canonical_stream) {
    canonical.open(paths.canonical, std::ios::binary | std::ios::trunc);
    if (!canonical.is_open()) {
      report.error = ArtifactError::canonical_open_failed;
      return report;
    }
  }

  aegis::common::Sha256Digest previous_digest{};
  EncodedPacket held_packet{};
  std::int64_t held_capture_time{};
  bool packet_held = false;
  const auto injection = scenario_injection_ordinal(config);

  for (std::uint64_t index = 0; index < config.event_count; ++index) {
    SyntheticEvent event{};
    const auto generated = generator->next(event);
    if (!generated.ok()) {
      report.error = ArtifactError::generation_failed;
      report.generation_error = generated.error;
      return report;
    }
    ++report.logical_events;

    if (write_canonical_stream) {
      const auto envelope = build_market_event_envelope(event, config, previous_digest);
      const auto encoded =
          std::span<const std::uint8_t>{envelope.data(), envelope.size()};
      if (!aegis::common::validate_size_prefixed_audit_envelope(encoded).ok()) {
        report.error = ArtifactError::canonical_validation_failed;
        return report;
      }
      canonical.write(reinterpret_cast<const char*>(encoded.data()),
                      static_cast<std::streamsize>(encoded.size()));
      if (!canonical.good()) {
        report.error = ArtifactError::canonical_write_failed;
        return report;
      }
      previous_digest = aegis::common::sha256(encoded);
      ++report.canonical_records;
    }

    const auto packet = encode_packet(event);
    if (config.scenario == Scenario::missing_sequence &&
        event.global_ordinal == injection) {
      continue;
    }
    if (config.scenario == Scenario::out_of_order_packet &&
        event.global_ordinal == injection) {
      held_packet = packet;
      held_capture_time = event.nic_receive_time_ns;
      packet_held = true;
      continue;
    }
    auto write_error =
        write_packet(*capture, packet, event.nic_receive_time_ns, report);
    if (write_error != ArtifactError::none) {
      report.error = write_error;
      return report;
    }
    if (config.scenario == Scenario::duplicate_packet &&
        event.global_ordinal == injection) {
      write_error = write_packet(*capture, packet, event.nic_receive_time_ns, report);
      if (write_error != ArtifactError::none) {
        report.error = write_error;
        return report;
      }
    }
    if (config.scenario == Scenario::out_of_order_packet && packet_held &&
        event.global_ordinal == injection + 1U) {
      write_error = write_packet(*capture, held_packet, held_capture_time, report);
      if (write_error != ArtifactError::none) {
        report.error = write_error;
        return report;
      }
      packet_held = false;
    }
  }
  if (packet_held) {
    report.error = ArtifactError::generation_failed;
    report.generation_error = GenerationError::book_invariant_failure;
    return report;
  }
  report.final_book_hash = generator->expected_book().stable_hash();
  if (!write_book_snapshot(paths.expected_book, config, generator->expected_book())) {
    report.error = ArtifactError::book_write_failed;
    return report;
  }
  report.capture_error = capture->finalize(report.final_book_hash);
  if (report.capture_error != CaptureError::none) {
    report.error = ArtifactError::capture_failed;
    return report;
  }
  if (write_canonical_stream) {
    canonical.flush();
    if (!canonical.good()) {
      report.error = ArtifactError::canonical_write_failed;
      return report;
    }
  }
  return report;
}

// Replay keeps fail-closed sequence transitions together so invalid-channel
// state cannot accidentally be bypassed by a helper boundary.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
VerificationReport verify_artifacts(const VerificationPaths& paths) noexcept {
  VerificationReport report{};
  const auto snapshot_hash = read_book_snapshot_hash(paths.expected_book);
  if (!snapshot_hash.has_value()) {
    report.error = ArtifactError::book_read_failed;
    return report;
  }
  CaptureError open_error = CaptureError::none;
  auto reader = CaptureReader::open(paths.capture, open_error);
  if (!reader.has_value()) {
    report.error = ArtifactError::capture_failed;
    report.capture_error = open_error;
    return report;
  }
  report.expected_book_hash = reader->header().expected_book_hash;
  if (*snapshot_hash != report.expected_book_hash) {
    report.error = ArtifactError::verification_failed;
    return report;
  }

  BookSet books;
  std::array<ChannelReplayState, kMaximumInstruments> channels{};
  std::size_t channel_count = 0U;
  std::uint64_t burst_packets = 0U;
  std::uint64_t scenario_packets = 0U;
  std::uint64_t shock_messages = 0U;
  std::uint64_t hidden_messages = 0U;
  std::uint64_t halt_statuses = 0U;
  std::uint64_t auction_statuses = 0U;
  std::uint64_t open_statuses = 0U;

  for (std::uint64_t index = 0; index < reader->header().physical_packet_count;
       ++index) {
    CaptureRecord record{};
    report.capture_error = reader->next(record);
    if (report.capture_error != CaptureError::none) {
      report.error = ArtifactError::capture_failed;
      return report;
    }
    ++report.packets;
    SyntheticEvent event{};
    report.decode_error = decode_packet(record.packet, event);
    if (report.decode_error != DecodeError::none) {
      report.error = ArtifactError::verification_failed;
      return report;
    }
    burst_packets += (event.packet_flags & kPacketFlagBurst) != 0U ? 1U : 0U;
    scenario_packets += (event.packet_flags & kPacketFlagScenario) != 0U ? 1U : 0U;
    shock_messages += (event.message_flags & kMessageFlagShock) != 0U ? 1U : 0U;
    hidden_messages +=
        (event.message_flags & kMessageFlagHiddenReplenishment) != 0U ? 1U : 0U;
    if (event.type == NativeMessageType::trading_status) {
      halt_statuses += event.status == TradingStatus::halted ? 1U : 0U;
      auction_statuses += event.status == TradingStatus::auction ? 1U : 0U;
      open_statuses += event.status == TradingStatus::open ? 1U : 0U;
    }

    auto* channel = find_channel(channels, channel_count, event);
    if (channel == nullptr) {
      report.error = ArtifactError::verification_failed;
      return report;
    }
    if (channel->last_sequence != 0U &&
        event.channel_sequence == channel->last_sequence) {
      ++report.duplicate_packets;
      continue;
    }
    if (channel->last_sequence != 0U) {
      if (channel->last_sequence == std::numeric_limits<std::uint64_t>::max()) {
        report.error = ArtifactError::verification_failed;
        return report;
      }
      const auto expected = channel->last_sequence + 1U;
      if (event.channel_sequence > expected) {
        ++report.sequence_gaps;
        channel->valid = false;
      } else if (event.channel_sequence < expected) {
        ++report.out_of_order_packets;
        channel->valid = false;
        continue;
      }
    }
    if (channel->last_capture_time_ns != 0 &&
        elapsed_exceeds(record.capture_time_ns, channel->last_capture_time_ns,
                        reader->header().stale_threshold_ns)) {
      ++report.stale_intervals;
    }
    if ((event.packet_flags & kPacketFlagAfterStaleGap) != 0U &&
        (channel->last_capture_time_ns == 0 ||
         !elapsed_exceeds(record.capture_time_ns, channel->last_capture_time_ns,
                          reader->header().stale_threshold_ns))) {
      ++report.stale_intervals;
    }
    channel->last_sequence = event.channel_sequence;
    channel->last_capture_time_ns = record.capture_time_ns;
    if (!channel->valid) {
      continue;
    }
    if (!books.register_instrument(event.venue_number, event.instrument_number)) {
      report.error = ArtifactError::verification_failed;
      return report;
    }
    const auto book_error = books.apply(event);
    if (book_error == BookApplyError::crossed_book) {
      ++report.crossed_books;
    } else if (book_error == BookApplyError::update_while_halted) {
      ++report.halted_update_attempts;
    } else if (book_error != BookApplyError::none) {
      report.error = ArtifactError::verification_failed;
      return report;
    }
    ++report.applied_events;
  }
  report.capture_error = reader->finish();
  if (report.capture_error != CaptureError::none) {
    report.error = ArtifactError::capture_failed;
    return report;
  }
  report.calculated_book_hash = books.stable_hash();
  report.final_book_matches = report.calculated_book_hash == report.expected_book_hash;

  switch (reader->header().scenario) {
  case Scenario::normal:
    report.scenario_expectation_met = scenario_packets == 0U;
    break;
  case Scenario::high_message_rate_burst:
    report.scenario_expectation_met = burst_packets > 0U;
    break;
  case Scenario::crossed_book_fault:
    report.scenario_expectation_met = report.crossed_books > 0U;
    break;
  case Scenario::duplicate_packet:
    report.scenario_expectation_met = report.duplicate_packets == 1U;
    break;
  case Scenario::missing_sequence:
    report.scenario_expectation_met = report.sequence_gaps == 1U;
    break;
  case Scenario::out_of_order_packet:
    report.scenario_expectation_met =
        report.sequence_gaps == 1U && report.out_of_order_packets == 1U;
    break;
  case Scenario::stale_feed:
    report.scenario_expectation_met = report.stale_intervals >= 1U;
    break;
  case Scenario::trading_halt:
    report.scenario_expectation_met = halt_statuses >= 1U;
    break;
  case Scenario::reopening_auction:
    report.scenario_expectation_met =
        halt_statuses >= 1U && auction_statuses >= 1U && open_statuses >= 1U;
    break;
  case Scenario::earnings_shock:
  case Scenario::macro_shock:
  case Scenario::index_rebalance:
    report.scenario_expectation_met = shock_messages >= 1U;
    break;
  case Scenario::hidden_liquidity_replenishment:
    report.scenario_expectation_met = hidden_messages == 2U;
    break;
  }
  if (!report.scenario_expectation_met ||
      (final_book_required(reader->header().scenario) && !report.final_book_matches)) {
    report.error = ArtifactError::verification_failed;
  }
  return report;
}

ArtifactError stream_capture(const std::filesystem::path& capture_path,
                             std::ostream& output, const std::uint64_t acceleration,
                             std::uint64_t& packets_streamed) {
  packets_streamed = 0U;
  CaptureError open_error = CaptureError::none;
  auto reader = CaptureReader::open(capture_path, open_error);
  if (!reader.has_value()) {
    return ArtifactError::stream_open_failed;
  }
  std::int64_t previous_capture_time = 0;
  for (std::uint64_t index = 0; index < reader->header().physical_packet_count;
       ++index) {
    CaptureRecord record{};
    if (reader->next(record) != CaptureError::none) {
      return ArtifactError::capture_failed;
    }
    if (acceleration != 0U && previous_capture_time != 0 &&
        record.capture_time_ns > previous_capture_time) {
      const auto delay =
          static_cast<std::uint64_t>(record.capture_time_ns - previous_capture_time) /
          acceleration;
      if (delay != 0U) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(delay));
      }
    }
    output.write(reinterpret_cast<const char*>(record.packet.data()),
                 static_cast<std::streamsize>(record.packet.size()));
    if (!output.good()) {
      return ArtifactError::stream_write_failed;
    }
    previous_capture_time = record.capture_time_ns;
    ++packets_streamed;
  }
  return reader->finish() == CaptureError::none ? ArtifactError::none
                                                : ArtifactError::capture_failed;
}

ArtifactError inspect_capture(const std::filesystem::path& capture_path,
                              std::ostream& output,
                              const std::uint64_t maximum_records) {
  CaptureError open_error = CaptureError::none;
  auto reader = CaptureReader::open(capture_path, open_error);
  if (!reader.has_value()) {
    return ArtifactError::inspect_failed;
  }
  const auto& header = reader->header();
  output << "capture=SMX/1 scenario=" << scenario_name(header.scenario)
         << " seed=" << header.seed << " logical_events=" << header.logical_event_count
         << " physical_packets=" << header.physical_packet_count << " config_hash=0x"
         << std::hex << header.configuration_hash << " expected_book_hash=0x"
         << header.expected_book_hash << std::dec << '\n';
  const auto count = std::min(maximum_records, header.physical_packet_count);
  for (std::uint64_t index = 0; index < count; ++index) {
    CaptureRecord record{};
    if (reader->next(record) != CaptureError::none) {
      return ArtifactError::inspect_failed;
    }
    SyntheticEvent event{};
    if (decode_packet(record.packet, event) != DecodeError::none) {
      return ArtifactError::inspect_failed;
    }
    output << "packet=" << (index + 1U) << " capture_ns=" << record.capture_time_ns
           << " venue=" << event.venue_number << " channel=" << event.channel_number
           << " sequence=" << event.channel_sequence
           << " ordinal=" << event.global_ordinal
           << " instrument=" << event.instrument_number
           << " type=" << message_type_name(event.type)
           << " side=" << side_name(event.side)
           << " action=" << action_name(event.action)
           << " price_ticks=" << event.price_ticks
           << " quantity_units=" << event.quantity_units << " event_hash=0x" << std::hex
           << event.event_hash << std::dec << '\n';
  }
  return output.good() ? ArtifactError::none : ArtifactError::inspect_failed;
}

std::string_view artifact_error_name(const ArtifactError error) noexcept {
  switch (error) {
  case ArtifactError::none:
    return "none";
  case ArtifactError::invalid_config:
    return "invalid-config";
  case ArtifactError::generation_failed:
    return "generation-failed";
  case ArtifactError::capture_failed:
    return "capture-failed";
  case ArtifactError::canonical_open_failed:
    return "canonical-open-failed";
  case ArtifactError::canonical_write_failed:
    return "canonical-write-failed";
  case ArtifactError::canonical_validation_failed:
    return "canonical-validation-failed";
  case ArtifactError::book_write_failed:
    return "book-write-failed";
  case ArtifactError::book_read_failed:
    return "book-read-failed";
  case ArtifactError::verification_failed:
    return "verification-failed";
  case ArtifactError::stream_open_failed:
    return "stream-open-failed";
  case ArtifactError::stream_write_failed:
    return "stream-write-failed";
  case ArtifactError::inspect_failed:
    return "inspect-failed";
  }
  return "invalid-error";
}

} // namespace aegis::market_data::synthetic
