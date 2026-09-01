#include "aegis/market_data/synthetic/artifacts.hpp"
#include "aegis/market_data/synthetic/config.hpp"
#include "cli_common.hpp"

#include "aegis/common/build_info.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace synthetic = aegis::market_data::synthetic;
namespace cli = synthetic::cli;

namespace {

void usage(std::ostream& output) {
  output << "Usage: synth-exchange-generate [options]\n"
         << "  --output-prefix PATH       Artifact prefix (default synthetic)\n"
         << "  --seed N                   Explicit deterministic seed\n"
         << "  --events N                 Logical event count\n"
         << "  --scenario NAME            See synthetic protocol documentation\n"
         << "  --venues N                 Venue count (1..8)\n"
         << "  --instruments N            Instrument count (1..64)\n"
         << "  --feed-mode mixed|order|price\n"
         << "  --tick-value-nanos N       Tick value in currency nanounits\n"
         << "  --session-start-ns N       Exchange event UTC epoch nanoseconds\n"
         << "  --session-end-ns N         Exchange event UTC epoch nanoseconds\n"
         << "  --rate N                   Normal messages per second\n"
         << "  --burst-rate N             Burst messages per second\n"
         << "  --volatility-ppm N         One-tick move probability per event\n"
         << "  --tight-spread N --normal-spread N --wide-spread N\n"
         << "  --spread-regime-events N   Events per spread regime\n"
         << "  --size-distribution fixed|uniform|two-point\n"
         << "  --min-quantity N --max-quantity N\n"
         << "  --cancel-ppm N --market-ppm N\n"
         << "  --auction-start N --auction-end N\n"
         << "  --stale-threshold-ns N --max-active-orders N\n"
         << "  --no-canonical             Omit the AMAE canonical stream\n"
         << "  --version\n";
}

[[nodiscard]] std::optional<std::string_view> next_value(const int argc, char** argv,
                                                         int& index) noexcept {
  if (index + 1 >= argc) {
    return std::nullopt;
  }
  ++index;
  return std::string_view{argv[index]};
}

template <typename Integer>
[[nodiscard]] bool parse_next(const int argc, char** argv, int& index,
                              Integer& output) noexcept {
  const auto value = next_value(argc, argv, index);
  if (!value.has_value()) {
    return false;
  }
  const auto parsed = cli::parse_integer<Integer>(*value);
  if (!parsed.has_value()) {
    return false;
  }
  output = *parsed;
  return true;
}

} // namespace

// CLI option dispatch is intentionally explicit so every unit-bearing field has
// one unambiguous parser and error path.
// NOLINTBEGIN(readability-function-cognitive-complexity)
int main(const int argc, char** argv) {
  auto config = synthetic::make_default_config();
  std::filesystem::path output_prefix{"synthetic"};
  cli::RequestedFeedMode feed_mode = cli::RequestedFeedMode::mixed;
  std::int64_t tick_value = 10'000'000;
  bool write_canonical = true;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument == "--help" || argument == "-h") {
      usage(std::cout);
      return 0;
    }
    if (argument == "--version") {
      const auto& build = aegis::common::current_build_info();
      std::cout << build.project << ' ' << build.version
                << " revision=" << build.source_revision
                << " live_trading_capable=" << build.live_trading_capable << '\n';
      return 0;
    }
    if (argument == "--no-canonical") {
      write_canonical = false;
      continue;
    }
    if (argument == "--output-prefix") {
      const auto value = next_value(argc, argv, index);
      if (!value.has_value()) {
        std::cerr << "missing value for --output-prefix\n";
        return 2;
      }
      output_prefix = std::string{*value};
      continue;
    }
    if (argument == "--scenario") {
      const auto value = next_value(argc, argv, index);
      const auto scenario = value.has_value() ? synthetic::parse_scenario(*value)
                                              : std::optional<synthetic::Scenario>{};
      if (!scenario.has_value()) {
        std::cerr << "invalid scenario\n";
        return 2;
      }
      config.scenario = *scenario;
      continue;
    }
    if (argument == "--feed-mode") {
      const auto value = next_value(argc, argv, index);
      if (!value.has_value()) {
        std::cerr << "missing feed mode\n";
        return 2;
      }
      if (*value == "mixed") {
        feed_mode = cli::RequestedFeedMode::mixed;
      } else if (*value == "order") {
        feed_mode = cli::RequestedFeedMode::order_level;
      } else if (*value == "price") {
        feed_mode = cli::RequestedFeedMode::price_level;
      } else {
        std::cerr << "invalid feed mode\n";
        return 2;
      }
      continue;
    }
    if (argument == "--size-distribution") {
      const auto value = next_value(argc, argv, index);
      if (!value.has_value()) {
        std::cerr << "missing size distribution\n";
        return 2;
      }
      if (*value == "fixed") {
        config.order_size_distribution = synthetic::OrderSizeDistribution::fixed;
      } else if (*value == "uniform") {
        config.order_size_distribution = synthetic::OrderSizeDistribution::uniform;
      } else if (*value == "two-point") {
        config.order_size_distribution = synthetic::OrderSizeDistribution::two_point;
      } else {
        std::cerr << "invalid size distribution\n";
        return 2;
      }
      continue;
    }

    bool parsed = true;
    if (argument == "--seed") {
      parsed = parse_next(argc, argv, index, config.seed);
    } else if (argument == "--events") {
      parsed = parse_next(argc, argv, index, config.event_count);
    } else if (argument == "--venues") {
      parsed = parse_next(argc, argv, index, config.venue_count);
    } else if (argument == "--instruments") {
      parsed = parse_next(argc, argv, index, config.instrument_count);
    } else if (argument == "--tick-value-nanos") {
      parsed = parse_next(argc, argv, index, tick_value);
    } else if (argument == "--session-start-ns") {
      parsed = parse_next(argc, argv, index, config.session_start_exchange_time_ns);
    } else if (argument == "--session-end-ns") {
      parsed = parse_next(argc, argv, index, config.session_end_exchange_time_ns);
    } else if (argument == "--rate") {
      parsed = parse_next(argc, argv, index, config.normal_message_rate_per_second);
    } else if (argument == "--burst-rate") {
      parsed = parse_next(argc, argv, index, config.burst_message_rate_per_second);
    } else if (argument == "--volatility-ppm") {
      parsed = parse_next(argc, argv, index, config.volatility_ppm);
    } else if (argument == "--tight-spread") {
      parsed = parse_next(argc, argv, index, config.tight_spread_ticks);
    } else if (argument == "--normal-spread") {
      parsed = parse_next(argc, argv, index, config.normal_spread_ticks);
    } else if (argument == "--wide-spread") {
      parsed = parse_next(argc, argv, index, config.wide_spread_ticks);
    } else if (argument == "--spread-regime-events") {
      parsed = parse_next(argc, argv, index, config.spread_regime_period_events);
    } else if (argument == "--min-quantity") {
      parsed = parse_next(argc, argv, index, config.minimum_order_quantity);
    } else if (argument == "--max-quantity") {
      parsed = parse_next(argc, argv, index, config.maximum_order_quantity);
    } else if (argument == "--cancel-ppm") {
      parsed = parse_next(argc, argv, index, config.cancellation_intensity_ppm);
    } else if (argument == "--market-ppm") {
      parsed = parse_next(argc, argv, index, config.market_order_intensity_ppm);
    } else if (argument == "--auction-start") {
      parsed = parse_next(argc, argv, index, config.auction_start_event);
    } else if (argument == "--auction-end") {
      parsed = parse_next(argc, argv, index, config.auction_end_event);
    } else if (argument == "--stale-threshold-ns") {
      parsed = parse_next(argc, argv, index, config.stale_threshold_ns);
    } else if (argument == "--max-active-orders") {
      parsed =
          parse_next(argc, argv, index, config.maximum_active_orders_per_instrument);
    } else {
      std::cerr << "unknown option: " << argument << '\n';
      return 2;
    }
    if (!parsed) {
      std::cerr << "invalid or missing numeric value for " << argument << '\n';
      return 2;
    }
  }

  if (!cli::configure_instruments(config, feed_mode, tick_value)) {
    std::cerr << "invalid venue, instrument, or tick configuration\n";
    return 2;
  }
  const auto config_error = synthetic::validate_config(config);
  if (config_error != synthetic::ConfigError::none) {
    std::cerr << "invalid configuration: " << synthetic::config_error_name(config_error)
              << '\n';
    return 2;
  }
  const std::string prefix = output_prefix.string();
  const synthetic::ArtifactPaths paths{
      .capture = prefix + ".smxcap",
      .canonical = prefix + ".amae",
      .expected_book = prefix + ".book.txt",
  };
  const auto report = synthetic::generate_artifacts(config, paths, write_canonical);
  if (report.error != synthetic::ArtifactError::none) {
    std::cerr << "generation failed: " << synthetic::artifact_error_name(report.error)
              << " generator="
              << synthetic::generation_error_name(report.generation_error)
              << " capture=" << synthetic::capture_error_name(report.capture_error)
              << '\n';
    return 1;
  }
  std::cout << "scenario=" << synthetic::scenario_name(config.scenario)
            << " seed=" << config.seed << " logical_events=" << report.logical_events
            << " physical_packets=" << report.physical_packets
            << " canonical_records=" << report.canonical_records << " config_hash=0x"
            << std::hex << report.configuration_hash << " final_book_hash=0x"
            << report.final_book_hash << std::dec << '\n';
  return 0;
}
// NOLINTEND(readability-function-cognitive-complexity)
