#include "aegis/market_data/synthetic/config.hpp"
#include "aegis/models/microstructure_dataset.hpp"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

template <typename Integer>
[[nodiscard]] bool parse_integer(const std::string_view value,
                                 Integer& output) noexcept {
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), output);
  return error == std::errc{} && end == value.data() + value.size();
}

} // namespace

int main(const int argc, char** argv) {
  if (argc != 6 || std::string_view{argv[1]} != "--output" ||
      std::string_view{argv[3]} != "--seed") {
    std::cerr
        << "Usage: synth-microstructure-dataset --output PATH --seed N N_EVENTS\n";
    return 2;
  }
  auto config = aegis::market_data::synthetic::make_default_config();
  if (!parse_integer(std::string_view{argv[4]}, config.seed) ||
      !parse_integer(std::string_view{argv[5]}, config.event_count) ||
      aegis::market_data::synthetic::validate_config(config) !=
          aegis::market_data::synthetic::ConfigError::none) {
    std::cerr << "invalid deterministic generator configuration\n";
    return 2;
  }
  std::ofstream output{argv[2], std::ios::binary | std::ios::trunc};
  aegis::models::MicrostructureDatasetSummary summary{};
  if (!output.is_open() ||
      !aegis::models::write_synthetic_microstructure_dataset(config, output, summary)) {
    std::cerr << "dataset generation failed\n";
    return 1;
  }
  std::cout << "seed=" << summary.seed
            << " source_events=" << summary.source_event_count
            << " rows=" << summary.row_count << " stable_hash=" << summary.stable_hash
            << " economic_value_claim=false\n";
  return 0;
}
