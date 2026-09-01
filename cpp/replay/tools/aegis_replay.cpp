#include "aegis/replay/replay.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <utility>

namespace replay = aegis::replay;

namespace {

void usage() {
  std::cerr << "usage: aegis-replay (--journal PATH | --capture PATH) "
               "[--speed maximum|original|step|Nx] [--seed N] [--from-ns N] "
               "[--to-ns N] [--first-sequence N] [--last-sequence N] "
               "[--max-records N] [--max-payload-bytes N] "
               "[--instrument HIGH:LOW] "
               "[--fault KIND:FIRST:LAST:EVERY:VALUE[:OPAQUE_TEXT]] "
               "[--manifest PATH] [--summary PATH]\n"
               "Recompute mode is available through the C++ API with an explicitly "
               "registered model recomputer. This tool never transmits orders.\n";
}

[[nodiscard]] bool parse_u64(const std::string_view text,
                             std::uint64_t& output) noexcept {
  const auto result = std::from_chars(text.data(), text.data() + text.size(), output);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_i64(const std::string_view text,
                             std::int64_t& output) noexcept {
  const auto result = std::from_chars(text.data(), text.data() + text.size(), output);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_hex_u64(const std::string_view text,
                                 std::uint64_t& output) noexcept {
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), output, 16);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_instrument(const std::string_view text,
                                    aegis::common::InstrumentId& output) noexcept {
  const auto separator = text.find(':');
  std::uint64_t high{};
  std::uint64_t low{};
  if (separator == std::string_view::npos ||
      !parse_hex_u64(text.substr(0U, separator), high) ||
      !parse_hex_u64(text.substr(separator + 1U), low) || (high == 0U && low == 0U)) {
    return false;
  }
  output = aegis::common::InstrumentId{high, low};
  return true;
}

[[nodiscard]] bool parse_fault_kind(const std::string_view text,
                                    replay::FaultKind& output) noexcept {
  constexpr std::array names{
      std::pair{"latency", replay::FaultKind::latency},
      std::pair{"loss", replay::FaultKind::message_loss},
      std::pair{"duplicate", replay::FaultKind::duplication},
      std::pair{"reorder", replay::FaultKind::reorder_adjacent},
      std::pair{"drift", replay::FaultKind::clock_drift},
      std::pair{"outage", replay::FaultKind::feed_outage},
      std::pair{"crash", replay::FaultKind::process_crash},
      std::pair{"halt", replay::FaultKind::trading_halt},
      std::pair{"reopen", replay::FaultKind::reopening},
      std::pair{"news", replay::FaultKind::news_injection},
      std::pair{"macro", replay::FaultKind::macro_injection},
  };
  for (const auto& [name, kind] : names) {
    if (text == name) {
      output = kind;
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool parse_fault(const std::string_view text, replay::FaultSpec& output) {
  std::array<std::string_view, 5U> fields{};
  std::size_t begin{};
  for (std::size_t index = 0U; index < 4U; ++index) {
    const auto separator = text.find(':', begin);
    if (separator == std::string_view::npos) {
      return false;
    }
    fields[index] = text.substr(begin, separator - begin);
    begin = separator + 1U;
  }
  fields[4] = text.substr(begin);
  const auto payload_separator = fields[4].find(':');
  const auto value = fields[4].substr(0U, payload_separator);
  const auto payload = payload_separator == std::string_view::npos
                           ? std::string_view{}
                           : fields[4].substr(payload_separator + 1U);
  if (!parse_fault_kind(fields[0], output.kind) ||
      !parse_u64(fields[1], output.first_ordinal) ||
      !parse_u64(fields[2], output.last_ordinal) ||
      !parse_u64(fields[3], output.every_nth) || !parse_i64(value, output.value)) {
    return false;
  }
  output.payload = payload;
  return true;
}

[[nodiscard]] bool parse_speed(const std::string_view text,
                               replay::ReplayConfig& config) noexcept {
  if (text == "maximum") {
    config.speed = replay::SpeedMode::maximum;
    return true;
  }
  if (text == "original") {
    config.speed = replay::SpeedMode::original;
    return true;
  }
  if (text == "step") {
    config.speed = replay::SpeedMode::single_step;
    return true;
  }
  if (text.size() > 1U && text.back() == 'x') {
    std::uint64_t acceleration{};
    if (parse_u64(text.substr(0U, text.size() - 1U), acceleration) &&
        acceleration > 0U) {
      config.speed = replay::SpeedMode::accelerated;
      config.acceleration = acceleration;
      return true;
    }
  }
  return false;
}

} // namespace

// CLI option and single-step dispatch are intentionally explicit so malformed
// unit-bearing values have one visible fail-closed path.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int main(const int argc, char** argv) {
  replay::ReplayConfig config{};
  std::filesystem::path manifest_path{"replay-manifest.json"};
  std::filesystem::path summary_path{"replay-summary.json"};
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    const auto value = [&]() -> std::string_view {
      if (index + 1 >= argc) {
        return {};
      }
      return argv[++index];
    };
    if (argument == "--journal") {
      config.source_kind = replay::SourceKind::journal;
      config.source = value();
    } else if (argument == "--capture") {
      config.source_kind = replay::SourceKind::synthetic_capture;
      config.source = value();
    } else if (argument == "--speed") {
      if (!parse_speed(value(), config)) {
        usage();
        return 2;
      }
    } else if (argument == "--seed") {
      if (!parse_u64(value(), config.seed)) {
        usage();
        return 2;
      }
    } else if (argument == "--from-ns") {
      if (!parse_u64(value(), config.from_process_monotonic_time_ns)) {
        usage();
        return 2;
      }
    } else if (argument == "--to-ns") {
      if (!parse_u64(value(), config.to_process_monotonic_time_ns)) {
        usage();
        return 2;
      }
    } else if (argument == "--first-sequence") {
      if (!parse_u64(value(), config.first_sequence)) {
        usage();
        return 2;
      }
    } else if (argument == "--last-sequence") {
      if (!parse_u64(value(), config.last_sequence)) {
        usage();
        return 2;
      }
    } else if (argument == "--max-records") {
      if (!parse_u64(value(), config.maximum_records)) {
        usage();
        return 2;
      }
    } else if (argument == "--max-payload-bytes") {
      if (!parse_u64(value(), config.maximum_total_payload_bytes)) {
        usage();
        return 2;
      }
    } else if (argument == "--instrument") {
      aegis::common::InstrumentId instrument{};
      if (!parse_instrument(value(), instrument)) {
        usage();
        return 2;
      }
      config.instruments.push_back(instrument);
    } else if (argument == "--fault") {
      replay::FaultSpec fault{};
      if (!parse_fault(value(), fault)) {
        usage();
        return 2;
      }
      config.faults.push_back(std::move(fault));
    } else if (argument == "--manifest") {
      manifest_path = value();
    } else if (argument == "--summary") {
      summary_path = value();
    } else if (argument == "--help") {
      usage();
      return 0;
    } else {
      usage();
      return 2;
    }
  }
  if (config.source.empty()) {
    usage();
    return 2;
  }

  replay::EvidenceReplayTarget target;
  replay::VirtualPacer virtual_pacer;
  replay::SystemPacer system_pacer;
  replay::IReplayPacer& pacer = config.speed == replay::SpeedMode::original
                                    ? static_cast<replay::IReplayPacer&>(system_pacer)
                                    : static_cast<replay::IReplayPacer&>(virtual_pacer);
  replay::ReplayEngine engine;
  auto status = engine.prepare(config, target, pacer);
  if (status != replay::Status::ok) {
    std::cerr << "prepare failed: " << replay::status_name(status) << '\n';
    return 1;
  }
  if (config.speed == replay::SpeedMode::single_step) {
    std::cerr << "single-step: ENTER=next, r=run, q=quit\n";
    for (std::string command; std::getline(std::cin, command);) {
      if (command == "q") {
        break;
      }
      if (command == "r") {
        engine.resume();
        status = engine.run();
      } else {
        status = engine.step();
      }
      if (status == replay::Status::complete) {
        break;
      }
      if (status != replay::Status::ok) {
        std::cerr << "replay failed: " << replay::status_name(status) << '\n';
        return 1;
      }
    }
  } else {
    status = engine.run();
  }
  if (status != replay::Status::complete) {
    std::cerr << "replay incomplete: " << replay::status_name(status) << '\n';
    return 1;
  }
  if (!replay::write_text_file(manifest_path,
                               replay::manifest_json(engine.manifest())) ||
      !replay::write_text_file(summary_path, replay::summary_json(engine.summary()))) {
    std::cerr << "failed to write replay evidence\n";
    return 1;
  }
  std::cout << "status=complete source_records=" << engine.summary().source_records
            << " delivered_events=" << engine.summary().delivered_events
            << " manifest=" << manifest_path << " summary=" << summary_path << '\n';
  return 0;
}
